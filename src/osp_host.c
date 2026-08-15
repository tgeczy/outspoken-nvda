/* osp_host.c -- a Macintosh just large enough to run MacinTalk.
 *
 * The 1984 MacinTalk driver (`DRVR 1030`, named `.sp`) is 21,272 bytes of
 * 68000.  We do not emulate a Macintosh; we emulate the handful of things this
 * one driver reaches for.  Everything it touches is documented in
 * docs/sound-model.md and docs/driver-api.md, and both were written by reading
 * the binary rather than by running it, so this host starts from a map rather
 * than from guesses.
 *
 * Design rules carried over from pctalker-nvda and Jayson Smith's EchoTalk,
 * both of which were debugged the hard way:
 *
 *   * Every budget gets a counter and non-zero is a fault, never a silent
 *     truncation.  EchoTalk's first fix failed because the CPU was not given
 *     enough time to finish, and a silent limit is indistinguishable from a
 *     broken program.
 *   * Every unhandled exception is identified by vector, not reported as a
 *     generic stop.  "It stopped" costs an evening; "it took vector 3, address
 *     error, at driver+0x1234" costs a minute.
 *   * Traps we did not implement are counted separately from traps we served.
 *     Returning zero for an unimplemented call is a guess, and guesses that
 *     look like successes are the expensive kind.
 *
 * The engine itself is never distributed with this code.  osp_load_image()
 * takes whatever the user extracted from their own copy.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "m68k.h"

#if defined(_WIN32)
#  define OSP_API __declspec(dllexport)
#else
#  define OSP_API
#endif

/* ---------------------------------------------------------------- memory - */

static unsigned char *g_ram;
static unsigned       g_ram_size;

/* Exception vectors are pointed at this page, one slot of 8 bytes each, so a
 * stop can name the vector that caused it.  Vector 10 is the A-line trap and
 * is the only one we expect; it holds a real `rte`, the rest hold nothing
 * because we stop before executing them. */
#define MAGIC_EXC_BASE  0x00F00000u
#define MAGIC_EXC_SLOT  8u
#define MAGIC_SENTINEL  0x00F10000u

#define STOP_RUNNING    0
#define STOP_SENTINEL   1
#define STOP_BUDGET     2
#define STOP_EXCEPTION  3
#define STOP_FAULT      4
#define STOP_BREAK      5

static int          g_stop_reason;
static int          g_stop_vector;
static unsigned     g_stop_pc;
static long long    g_instr_count;
static long long    g_instr_budget;
static unsigned     g_sentinel;

/* An access outside RAM is a bug in us or a wild pointer in the driver.  It is
 * never normal, so it is recorded rather than quietly returning zero. */
#define MAX_FAULTS 64
typedef struct { unsigned addr; unsigned pc; int write; int size; } Fault;
static Fault    g_faults[MAX_FAULTS];
static int      g_fault_count;

static void note_fault(unsigned addr, int write, int size)
{
    if (g_fault_count < MAX_FAULTS) {
        g_faults[g_fault_count].addr  = addr;
        g_faults[g_fault_count].pc    = m68k_get_reg(NULL, M68K_REG_PPC);
        g_faults[g_fault_count].write = write;
        g_faults[g_fault_count].size  = size;
    }
    g_fault_count++;
}

static int in_ram(unsigned a, unsigned n) { return (a + n) <= g_ram_size; }

/* A write watchpoint.  When a value in emulated memory is wrong and static
 * reading has not found who sets it, ask the machine instead: record the PC of
 * every write into a range.  Cheaper than another hour of disassembly. */
static unsigned g_watch_lo, g_watch_hi;
#define WATCH_CAP 512
typedef struct { unsigned pc, addr, val; int size; } WatchRec;
static WatchRec g_watch[WATCH_CAP];
static int      g_watch_n;

static void note_write(unsigned a, unsigned n, unsigned v, int size)
{
    if (g_watch_hi <= g_watch_lo) return;
    if (a + n <= g_watch_lo || a >= g_watch_hi) return;
    if (g_watch_n < WATCH_CAP) {
        g_watch[g_watch_n].pc = m68k_get_reg(NULL, M68K_REG_PPC);
        g_watch[g_watch_n].addr = a;
        g_watch[g_watch_n].val = v;
        g_watch[g_watch_n].size = size;
    }
    g_watch_n++;
}

/* Read watch: record (pc, addr) for byte reads in a range.  Static reading has
 * twice now failed to settle how far `a6` advances per frame; the machine can
 * simply be asked. */
static unsigned g_rwatch_lo, g_rwatch_hi;
#define RWATCH_CAP 4096
static unsigned g_rwatch_pc[RWATCH_CAP], g_rwatch_addr[RWATCH_CAP];
static int      g_rwatch_n;

unsigned int m68k_read_memory_8(unsigned int a)
{
    if (!in_ram(a, 1)) { note_fault(a, 0, 1); return 0; }
    if (g_rwatch_hi > g_rwatch_lo && a >= g_rwatch_lo && a < g_rwatch_hi) {
        /* Ring, not a one-shot: generation does thousands of reads before
         * playback starts, and a capped log fills with the wrong phase. */
        unsigned i = g_rwatch_n % RWATCH_CAP;
        g_rwatch_pc[i] = m68k_get_reg(NULL, M68K_REG_PPC);
        g_rwatch_addr[i] = a;
        g_rwatch_n++;
    }
    return g_ram[a];
}
/* Word and long reads are watched too.  Hooking only byte reads once made a
 * field with four `move.w` consumers look completely unread. */
static void note_read(unsigned a)
{
    if (g_rwatch_hi > g_rwatch_lo && a >= g_rwatch_lo && a < g_rwatch_hi) {
        unsigned i = g_rwatch_n % RWATCH_CAP;
        g_rwatch_pc[i] = m68k_get_reg(NULL, M68K_REG_PPC);
        g_rwatch_addr[i] = a;
        g_rwatch_n++;
    }
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    if (!in_ram(a, 2)) { note_fault(a, 0, 2); return 0; }
    note_read(a);
    return ((unsigned)g_ram[a] << 8) | g_ram[a + 1];
}
unsigned int m68k_read_memory_32(unsigned int a)
{
    if (!in_ram(a, 4)) { note_fault(a, 0, 4); return 0; }
    note_read(a);
    return ((unsigned)g_ram[a] << 24) | ((unsigned)g_ram[a + 1] << 16)
         | ((unsigned)g_ram[a + 2] << 8) | g_ram[a + 3];
}
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    if (!in_ram(a, 1)) { note_fault(a, 1, 1); return; }
    note_write(a, 1, v, 1);
    g_ram[a] = (unsigned char)v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    if (!in_ram(a, 2)) { note_fault(a, 1, 2); return; }
    note_write(a, 2, v, 2);
    g_ram[a]     = (unsigned char)(v >> 8);
    g_ram[a + 1] = (unsigned char)v;
}
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    if (!in_ram(a, 4)) { note_fault(a, 1, 4); return; }
    note_write(a, 4, v, 4);
    g_ram[a]     = (unsigned char)(v >> 24);
    g_ram[a + 1] = (unsigned char)(v >> 16);
    g_ram[a + 2] = (unsigned char)(v >> 8);
    g_ram[a + 3] = (unsigned char)v;
}
unsigned int m68k_read_disassembler_8 (unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ----------------------------------------------------------------- traps - */

#define MAX_TRAPS 8192
typedef struct {
    unsigned pc;          /* address of the A-trap word itself */
    unsigned short word;
    unsigned d0, a0, a1, sp;
    int      served;      /* 0 = we stubbed it, 1 = a handler ran */
} TrapRec;

static TrapRec g_traps[MAX_TRAPS];
static int     g_trap_count;
static int     g_trap_overflow;
static int     g_stub_count;      /* traps we did NOT implement */

/* Which convention the CPU used for the stacked PC.  The 68000 stacks the
 * address of the offending instruction for A-line, but rather than trust that,
 * we look and record what we actually saw. */
static int g_stackpc_is_instruction = -1;

/* A per-trap canned result, set from Python.  -1 in `have` means "no policy,
 * stub it and count it". */
typedef struct { int have; unsigned d0; unsigned a0; } TrapPolicy;
static TrapPolicy g_policy[0x1000];   /* indexed by trap word & 0x0FFF */

/* ------------------------------------------------------ a bump allocator - */

static unsigned g_heap_base, g_heap_end, g_heap_next;
static int      g_mem_traps;

/* Low memory the driver actually reads.  Both were earned from the
 * disassembly, not from a table written out of memory -- see docs/driver-api.md
 * and the note in tools/disasm.py. */
#define RES_ERR_ADDR  0x0A60u     /* ResErr, tested after every resource call */
#define CPUFLAG_ADDR  0x012Fu     /* 0 = 68000, keeps Prime away from `movec` */

static unsigned g_ticks;          /* _TickCount, one per call */

/* A ring of recently executed addresses.  Reasoning about where 21 KB of
 * unfamiliar 68000 decided to give up is guesswork without this; with it, the
 * exit path is one `git diff`-sized read. */
#define TRACE_CAP 262144u
static unsigned *g_trace;
static unsigned  g_trace_pos;
static int       g_trace_on;

/* Register snapshots at a chosen PC.
 *
 * The trace ring says where the CPU went; it cannot say what a pointer held
 * when it got there.  Deducing a buffer stride from a disassembly listing is
 * exactly the guess that has been wrong three times in this project, so:
 * stop at the loader entry and write a6 down.  17 slots = d0-d7, a0-a7, pc. */
#define SNAP_CAP 64
static unsigned g_snap[SNAP_CAP][17];
static unsigned g_snap_pc;                     /* 0 disables */
static int      g_snap_n;
/* Halt on the Nth arrival, so memory can be read before later passes touch
 * it.  The player rewrites the frame block as it walks it -- +$28EC clears
 * three bytes of every frame it consumes -- which makes a dump taken after
 * the run a record of playback rather than of what playback was handed. */
static int      g_snap_halt;

static unsigned heap_alloc(unsigned size)
{
    unsigned p;
    size = (size + 3u) & ~3u;
    if (g_heap_next + size > g_heap_end) return 0;
    p = g_heap_next;
    g_heap_next += size;
    memset(g_ram + p, 0, size);
    return p;
}

/* A Mac handle is a pointer to a pointer.  We allocate the block, then a
 * two-longword master pointer in front of it, which is enough for code that
 * only ever dereferences and locks. */
static unsigned heap_new_handle(unsigned size)
{
    unsigned blk = heap_alloc(size);
    unsigned mp;
    if (!blk) return 0;
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    return mp;
}

/* Case- and diacritic-insensitive enough for the comparisons this driver makes.
 * The Mac's own folding is table-driven over MacRoman; we fold ASCII case and
 * leave the high half alone, which is correct for every string in `.sp`. */
static int fold(int c, int ignore_case)
{
    if (ignore_case && c >= 'a' && c <= 'z') c -= 32;
    return c;
}

/* Returns 1 if we served the trap. */
static int serve_memory_trap(unsigned short word, unsigned *d0_out, unsigned *a0_out)
{
    unsigned base = (word & 0x0800u) ? (word & 0xF8FFu) : (word & 0xF9FFu);
    unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
    unsigned a0 = m68k_get_reg(NULL, M68K_REG_A0);

    /* _CmpString and its MARKS/CASE variants ($A03C, $A23C, $A43C, $A63C).
     * A0 and A1 already point past the Pascal length bytes; D0 carries len1 in
     * the high word and len2 in the low.  Returns 0 for equal.
     *
     * This must be real, not stubbed.  Returning "equal" sends DriverOpen down
     * a path that stores a NULL where the TALK handle belongs and never calls
     * _GetResource at all -- a clean return that skipped the only thing Open
     * exists to do. */
    if ((word & 0xF0FFu) == 0xA03Cu) {
        unsigned a1 = m68k_get_reg(NULL, M68K_REG_A1);
        unsigned len1 = (d0 >> 16) & 0xFFFFu, len2 = d0 & 0xFFFFu;
        int ignore_case = (word & 0x0400u) ? 1 : 0;
        unsigned i;
        unsigned res = 1;
        if (len1 == len2) {
            res = 0;
            for (i = 0; i < len1; i++) {
                int c1 = fold(m68k_read_memory_8(a0 + i), ignore_case);
                int c2 = fold(m68k_read_memory_8(a1 + i), ignore_case);
                if (c1 != c2) { res = 1; break; }
            }
        }
        *d0_out = res; *a0_out = a0;
        return 1;
    }

    switch (base) {
    case 0xA11E: {                     /* _NewPtr  -- size in D0, ptr in A0 */
        unsigned p = heap_alloc(d0);
        *a0_out = p; *d0_out = p ? 0 : 0x0FFFFFF98u /* memFullErr */;
        if (!p) *d0_out = (unsigned)(-108);
        return 1;
    }
    case 0xA122: {                     /* _NewHandle -- size in D0, hdl A0 */
        unsigned h = heap_new_handle(d0);
        *a0_out = h; *d0_out = h ? 0 : (unsigned)(-108);
        return 1;
    }
    case 0xA029:                       /* _HLock   -- nothing moves here   */
    case 0xA02A:                       /* _HUnlock                         */
    case 0xA023:                       /* _DisposeHandle                   */
    case 0xA01F:                       /* _DisposePtr                      */
    case 0xA049:                       /* _HPurge                          */
    case 0xA04A:                       /* _HNoPurge                        */
    case 0xA036:                       /* _MoreMasters                     */
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA055:                       /* _StripAddress -- 24-bit clean    */
        *d0_out = d0 & 0x00FFFFFFu; *a0_out = a0;
        return 1;
    case 0xA02E: {                     /* _BlockMove -- A0 src A1 dst D0 n */
        unsigned src = a0, dst = m68k_get_reg(NULL, M68K_REG_A1), n = d0, i;
        for (i = 0; i < n; i++)
            m68k_write_memory_8(dst + i, m68k_read_memory_8(src + i));
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    default:
        return 0;
    }
}

/* --------------------------------------------- the Pascal stack convention -
 *
 * A Toolbox trap takes its arguments on the stack and leaves its result there,
 * in a slot the caller pushed *before* the arguments.  The callee removes the
 * arguments.  So after the call, SP points at the result.
 *
 * The complication is that we are standing inside an exception frame.  The
 * CPU pushed SR and PC onto the same stack, so the caller's SP is `exc_sp + 6`,
 * and we cannot simply adjust SP -- the `rte` will pop those six bytes from
 * wherever SP happens to be.  So we relocate the frame: put SR and PC six
 * bytes below where we want the caller's SP to end up, and let `rte` do the
 * arithmetic for us.
 */
#define EXC_FRAME 6u

static void tb_return(unsigned exc_sp, unsigned param_bytes,
                      unsigned result, int result_size)
{
    unsigned caller_sp = exc_sp + EXC_FRAME;
    unsigned result_slot = caller_sp + param_bytes;
    unsigned new_sp, sr, pc;

    if (result_size == 4)      m68k_write_memory_32(result_slot, result);
    else if (result_size == 2) m68k_write_memory_16(result_slot, result);
    else                       result_slot = caller_sp + param_bytes;

    new_sp = result_slot - EXC_FRAME;
    sr = m68k_read_memory_16(exc_sp);          /* read both before writing, */
    pc = m68k_read_memory_32(exc_sp + 2);      /* the ranges can overlap    */
    m68k_write_memory_16(new_sp, sr);
    m68k_write_memory_32(new_sp + 2, pc);
    m68k_set_reg(M68K_REG_SP, new_sp);
}

/* ------------------------------------------------------ resource manager - */

#define MAX_RES 64
typedef struct { unsigned type; short id; unsigned handle; } ResEntry;
static ResEntry g_res[MAX_RES];
static int      g_res_count;
static int      g_res_load = 1;
static short    g_res_err;

#define RES_NOT_FOUND (-192)      /* resNotFound */

static unsigned res_find(unsigned type, short id)
{
    int i;
    for (i = 0; i < g_res_count; i++)
        if (g_res[i].type == type && g_res[i].id == id)
            return g_res[i].handle;
    return 0;
}

/* ------------------------------------------------------- the Sound Manager -
 *
 * The whole audio path, per docs/sound-model.md.  MacinTalk fills a buffer,
 * hands it over with `bufferCmd`, and queues a `callBackCmd` behind it to learn
 * when that buffer has drained.  We take the samples immediately and fire the
 * callback, so the engine runs flat out and never waits on a clock.
 */
#define quietCmd     3
#define flushCmd     4
#define callBackCmd  13
#define bufferCmd    81

#define PCM_CAP       (8u * 1024u * 1024u)
#define MAGIC_CB_RET  0x00F11000u
#define CB_SCRATCH    0x00F20000u   /* the SndCommand, copied out of the
                                     * caller's stack frame -- by the time the
                                     * callback runs, that frame is gone */

static unsigned char *g_pcm;
static unsigned  g_pcm_len;
static int       g_pcm_overflow;
static int       g_buffers_taken;
static unsigned  g_sample_rate;     /* Fixed, from the last SoundHeader */
static int       g_short_buffers;   /* headers whose length was rewritten */

/* Which physical buffer each bufferCmd named, and the length it declared.
 * Double buffering is only correct if these alternate; a duplicate or a skip
 * is audible as a chop at the buffer rate -- 3870 samples at 22254 Hz is
 * 174 ms, so about 6 Hz. */
#define BUFLOG_CAP 8192
static unsigned  g_buflog_addr[BUFLOG_CAP];
static unsigned  g_buflog_len[BUFLOG_CAP];
static int       g_buflog_n;

static int      g_cb_pending;
static int      g_in_callback;
static unsigned g_cb_chan;

/* Take one buffer's worth of samples.
 *
 * `length` is read from the header every time and never assumed.  The driver
 * rewrites it (SetBufLength, +$4C36) so the last buffer of an utterance is
 * short; taking a fixed 3870 would append stale bytes to every phrase and show
 * up as a ~6 Hz chop under the voice. */
static void take_buffer(unsigned hdr)
{
    unsigned len = m68k_read_memory_32(hdr + 4);
    unsigned ptr = m68k_read_memory_32(hdr + 0);
    unsigned area = ptr ? ptr : (hdr + 0x16);
    unsigned i;

    g_sample_rate = m68k_read_memory_32(hdr + 8);
    if (len != 0x0F1E) g_short_buffers++;
    if (len > 0x10000u) {           /* nothing legitimate is this big */
        note_fault(hdr + 4, 0, 4);
        return;
    }
    for (i = 0; i < len; i++) {
        if (g_pcm_len >= PCM_CAP) { g_pcm_overflow++; return; }
        g_pcm[g_pcm_len++] = (unsigned char)m68k_read_memory_8(area + i);
    }
    if (g_buflog_n < BUFLOG_CAP) {
        g_buflog_addr[g_buflog_n] = hdr;
        g_buflog_len[g_buflog_n] = len;
        g_buflog_n++;
    }
    g_buffers_taken++;
}

static int serve_sound_trap(unsigned base, unsigned exc_sp, unsigned csp)
{
    switch (base) {
    case 0xA803:                        /* _SndDoCommand   */
    case 0xA804: {                      /* _SndDoImmediate */
        /* These two do NOT have the same signature:
         *
         *     SndDoCommand  (chan, cmd, noWait)   -- 10 bytes of arguments
         *     SndDoImmediate(chan, cmd)           --  8
         *
         * Treating them alike reads the command pointer two bytes off (giving
         * addresses like $FFDC001D) and, worse, pops two bytes too many, which
         * silently corrupts the caller's frame.  That is how MACSTARTSOUND came
         * back "cleanly" having written none of its SoundHeader fields. */
        int nowait = (base == 0xA803);
        unsigned pbytes = nowait ? 10u : 8u;
        unsigned cmdp = m68k_read_memory_32(csp + (nowait ? 2 : 0));
        unsigned chan = m68k_read_memory_32(csp + (nowait ? 6 : 4));
        unsigned cmd = m68k_read_memory_16(cmdp);
        unsigned param2 = m68k_read_memory_32(cmdp + 4);
        int i;

        if (cmd == bufferCmd) {
            take_buffer(param2);
        } else if (cmd == callBackCmd) {
            /* Copy the command somewhere that outlives the caller's frame,
             * then run the callback once we are safely outside m68k_execute. */
            for (i = 0; i < 8; i++)
                m68k_write_memory_8(CB_SCRATCH + i,
                                    m68k_read_memory_8(cmdp + i));
            g_cb_chan = chan;
            g_cb_pending = 1;
        }
        /* quietCmd / flushCmd: there is no queue to drop, we already took it */
        tb_return(exc_sp, pbytes, 0, 2);
        if (g_cb_pending) m68k_end_timeslice();
        return 1;
    }
    case 0xA800: {                      /* _SoundDispatch, selector in D0 */
        unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
        unsigned selector = d0 & 0xFFFFu;
        if (selector == 8) {            /* SndChannelStatus(chan, len, stat) */
            unsigned stat = m68k_read_memory_32(csp + 0);
            unsigned len = m68k_read_memory_16(csp + 4);
            unsigned i;
            for (i = 0; i < len && i < 64; i++)
                m68k_write_memory_8(stat + i, 0);
            /* scChannelBusy at +12.  We consume buffers instantly, so the
             * channel is never busy and every wait loop exits at once. */
            m68k_write_memory_8(stat + 12, 0);
            tb_return(exc_sp, 10, 0, 2);
            return 1;
        }
        tb_return(exc_sp, 10, 0, 2);
        return 1;
    }
    default:
        return 0;
    }
}

/* Returns 1 if we served the trap.  `exc_sp` is the exception frame address. */
static int serve_toolbox_trap(unsigned short word, unsigned exc_sp)
{
    unsigned csp = exc_sp + EXC_FRAME;
    /* A Toolbox trap number is ten bits; only bit 10 is a flag (auto-pop).
     * Masking bit 9 as well -- the OS-trap rule -- turns $A9A0 into $A8A0 and
     * every resource call falls through to "stubbed". */
    unsigned base = word & 0xFBFFu;

    if (serve_sound_trap(base, exc_sp, csp)) return 1;

    switch (base) {
    case 0xA9A0: {                       /* _GetResource(type, id) -> Handle */
        short id = (short)m68k_read_memory_16(csp);
        unsigned type = m68k_read_memory_32(csp + 2);
        unsigned h = res_find(type, id);
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 6, h, 4);
        return 1;
    }
    case 0xA9A2:                         /* _LoadResource(h) -- already in   */
    case 0xA9A3:                         /* _ReleaseResource(h)              */
    case 0xA992:                         /* _DetachResource(h)               */
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, 0, 0);
        return 1;
    case 0xA99B:                         /* _SetResLoad(Boolean)             */
        g_res_load = m68k_read_memory_8(csp + 1) ? 1 : 0;
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    case 0xA994:                         /* _CurResFile -> short             */
        tb_return(exc_sp, 0, 1, 2);
        return 1;
    case 0xA9A4:                         /* _HomeResFile(h) -> short         */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    case 0xA998:                         /* _UseResFile(short)               */
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    case 0xA997: {                       /* _OpenResFile(name) -> short      */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    }
    case 0xA975: {                       /* _TickCount -> long               */
        /* Only ever feeds `TickCount + 60` timeouts.  It must advance, or a
         * wait loop that checks it becomes infinite; it must not advance fast,
         * or a legitimate wait times out.  One tick per call is both. */
        g_ticks++;
        tb_return(exc_sp, 0, g_ticks, 4);
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------ the A-trap - */

static void service_atrap(void)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    unsigned stacked_pc = m68k_read_memory_32(sp + 2);
    unsigned trap_pc = stacked_pc;
    unsigned short word = (unsigned short)m68k_read_memory_16(trap_pc);
    unsigned d0, a0;
    int served = 0;

    if ((word & 0xF000u) != 0xA000u) {
        /* Not at the stacked address -- the CPU pointed past the word. */
        unsigned short w2 = (unsigned short)m68k_read_memory_16(stacked_pc - 2);
        if ((w2 & 0xF000u) == 0xA000u) {
            trap_pc = stacked_pc - 2;
            word = w2;
            if (g_stackpc_is_instruction < 0) g_stackpc_is_instruction = 0;
        }
    } else if (g_stackpc_is_instruction < 0) {
        g_stackpc_is_instruction = 1;
    }

    d0 = m68k_get_reg(NULL, M68K_REG_D0);
    a0 = m68k_get_reg(NULL, M68K_REG_A0);

    /* Patch the resume address first, so a Toolbox handler that relocates the
     * exception frame copies an already-correct PC. */
    m68k_write_memory_32(sp + 2, trap_pc + 2);

    if (word & 0x0800u) {
        /* Toolbox: arguments and result live on the stack.  A stub here is
         * worse than useless -- it leaves the stack unbalanced and the caller
         * returns into rubbish, which is how this first showed up (vector 4,
         * illegal instruction, four instructions after a stubbed
         * _GetResource). */
        served = serve_toolbox_trap(word, sp);
        if (!served) g_stub_count++;
    } else if (g_mem_traps && serve_memory_trap(word, &d0, &a0)) {
        served = 1;
    } else {
        TrapPolicy *p = &g_policy[word & 0x0FFFu];
        if (p->have) { d0 = p->d0; a0 = p->a0; served = 1; }
        else         { d0 = 0; g_stub_count++; }
    }

    if (g_trap_count < MAX_TRAPS) {
        TrapRec *t = &g_traps[g_trap_count];
        t->pc = trap_pc; t->word = word; t->served = served;
        t->d0 = d0; t->a0 = a0;
        t->a1 = m68k_get_reg(NULL, M68K_REG_A1);
        t->sp = sp;
        g_trap_count++;
    } else {
        g_trap_overflow++;
    }

    if (word & 0x0800u) return;      /* Toolbox already adjusted the frame */

    m68k_set_reg(M68K_REG_D0, d0);
    m68k_set_reg(M68K_REG_A0, a0);

    /* An OS trap returns its error in D0 *and* sets the condition codes from
     * it -- callers branch on the flags, not on D0.  Because the `rte` below
     * restores SR from the stack, setting the live CCR would be thrown away;
     * the stacked copy is the one that survives.
     *
     * Missing this made DriverOpen "succeed" in 14 instructions: _NewHandle
     * returned 0, the following `bne` saw a stale Z flag, and the driver took
     * its error exit and returned noErr.  A clean return that did nothing.
     * Toolbox traps ($A800+) pass results on the stack, so they are left
     * alone. */
    if (!(word & 0x0800u)) {
        unsigned sr = m68k_read_memory_16(sp);
        sr &= ~0x000Fu;                      /* N Z V C */
        if ((d0 & 0xFFFFu) == 0)  sr |= 0x04u;   /* Z */
        if (d0 & 0x8000u)         sr |= 0x08u;   /* N */
        m68k_write_memory_16(sp, sr);
    }
}

/* Run the driver's own callback proc, outside m68k_execute.
 *
 * Doing this from inside the trap handler would mean re-entering the CPU while
 * it is already running.  Instead the handler ends the timeslice, and we get
 * here with the machine stopped: push the two Pascal arguments and a magic
 * return address, jump to the proc, let it run to that address, then put PC and
 * SP back exactly as they were.  Register state is restored; the flag word the
 * callback set is a memory write, so it survives -- which is the whole point.
 */
static void run_pending_callback(void)
{
    unsigned proc = m68k_read_memory_32(g_cb_chan + 8);   /* SndChannel.callBack */
    unsigned save_pc, save_sp, sp;

    g_cb_pending = 0;
    if (!proc) return;

    save_pc = m68k_get_reg(NULL, M68K_REG_PC);
    save_sp = m68k_get_reg(NULL, M68K_REG_SP);

    sp = save_sp;
    sp -= 4; m68k_write_memory_32(sp, g_cb_chan);    /* chan, pushed first  */
    sp -= 4; m68k_write_memory_32(sp, CB_SCRATCH);   /* the SndCommand      */
    sp -= 4; m68k_write_memory_32(sp, MAGIC_CB_RET); /* return address      */
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, proc);

    g_in_callback = 1;
    while (g_in_callback && g_stop_reason == STOP_RUNNING)
        m68k_execute(100000);
    g_in_callback = 0;

    m68k_set_reg(M68K_REG_PC, save_pc);
    m68k_set_reg(M68K_REG_SP, save_sp);
}

static void instr_hook(unsigned int pc)
{
    if (g_trace_on) g_trace[g_trace_pos++ & (TRACE_CAP - 1u)] = pc;
    if (g_snap_pc && pc == g_snap_pc && g_snap_n < SNAP_CAP) {
        static const int R[17] = {
            M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
            M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
            M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
            M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
            M68K_REG_PC };
        int k;
        for (k = 0; k < 17; k++)
            g_snap[g_snap_n][k] = m68k_get_reg(NULL, (m68k_register_t)R[k]);
        g_snap_n++;
        if (g_snap_halt && g_snap_n >= g_snap_halt) {
            g_stop_reason = STOP_BREAK;
            g_stop_pc = pc;
            m68k_end_timeslice();
            return;
        }
    }
    if (pc == MAGIC_CB_RET) {
        g_in_callback = 0;
        m68k_end_timeslice();
        return;
    }
    if (pc >= MAGIC_EXC_BASE && pc < MAGIC_EXC_BASE + 64u * MAGIC_EXC_SLOT) {
        int vec = (int)((pc - MAGIC_EXC_BASE) / MAGIC_EXC_SLOT);
        if (vec == 10) { service_atrap(); return; }   /* A-line: expected   */
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = vec;
        g_stop_pc = m68k_read_memory_32(m68k_get_reg(NULL, M68K_REG_SP) + 2);
        m68k_end_timeslice();
        return;
    }
    if (pc == g_sentinel) {
        g_stop_reason = STOP_SENTINEL;
        g_stop_pc = pc;
        m68k_end_timeslice();
        return;
    }
    if (++g_instr_count > g_instr_budget) {
        g_stop_reason = STOP_BUDGET;      /* counted, never silent */
        g_stop_pc = pc;
        m68k_end_timeslice();
    }
}

/* -------------------------------------------------------------- the API -- */

OSP_API int osp_init(unsigned ram_size)
{
    unsigned v;
    if (g_ram) free(g_ram);
    if (ram_size < 0x00F30000u) ram_size = 0x00F30000u;  /* magic pages fit */
    g_ram = (unsigned char *)calloc(ram_size, 1);
    if (!g_ram) return -1;
    g_ram_size = ram_size;
    if (!g_pcm) g_pcm = (unsigned char *)malloc(PCM_CAP);
    if (!g_pcm) return -1;
    if (!g_trace) g_trace = (unsigned *)malloc(TRACE_CAP * sizeof(unsigned));
    if (!g_trace) return -1;
    g_trace_pos = 0; g_trace_on = 0;
    g_snap_pc = 0; g_snap_n = 0; g_snap_halt = 0;

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_instr_hook_callback(instr_hook);

    /* Vectors go in BEFORE the reset pulse, because reset loads SSP from $0 and
     * PC from $4.  osp_call sets both explicitly so the old order never bit us,
     * but anything that runs without osp_call first would have started from
     * zeroed memory -- and silently. */
    m68k_write_memory_32(0, 0x00200000u);      /* initial SSP */
    m68k_write_memory_32(4, MAGIC_SENTINEL);   /* initial PC  */
    /* Point every vector at its own identifiable slot, and give the A-line
     * slot a real `rte` so servicing can just fall through it. */
    for (v = 2; v < 64; v++)
        m68k_write_memory_32(v * 4u, MAGIC_EXC_BASE + v * MAGIC_EXC_SLOT);
    m68k_write_memory_16(MAGIC_EXC_BASE + 10u * MAGIC_EXC_SLOT, 0x4E73); /* rte */

    m68k_pulse_reset();

    memset(g_policy, 0, sizeof g_policy);
    g_trap_count = g_trap_overflow = g_stub_count = g_fault_count = 0;
    g_stackpc_is_instruction = -1;
    g_res_count = 0; g_res_load = 1; g_res_err = 0; g_ticks = 0;
    g_pcm_len = 0; g_buffers_taken = 0; g_pcm_overflow = 0; g_short_buffers = 0;
    g_buflog_n = 0;
    g_cb_pending = 0; g_in_callback = 0; g_sample_rate = 0;
    g_heap_base = g_heap_end = g_heap_next = 0;
    g_mem_traps = 0;
    return 0;
}

OSP_API void osp_shutdown(void)
{
    if (g_ram) free(g_ram);
    g_ram = NULL; g_ram_size = 0;
}

OSP_API int osp_write_block(unsigned addr, const unsigned char *data, int len)
{
    if (!in_ram(addr, (unsigned)len)) return -1;
    memcpy(g_ram + addr, data, (size_t)len);
    return 0;
}
OSP_API int osp_read_block(unsigned addr, unsigned char *out, int len)
{
    if (!in_ram(addr, (unsigned)len)) return -1;
    memcpy(out, g_ram + addr, (size_t)len);
    return 0;
}

OSP_API void osp_w8 (unsigned a, unsigned v) { m68k_write_memory_8(a, v); }
OSP_API void osp_w16(unsigned a, unsigned v) { m68k_write_memory_16(a, v); }
OSP_API void osp_w32(unsigned a, unsigned v) { m68k_write_memory_32(a, v); }
OSP_API unsigned osp_r8 (unsigned a) { return m68k_read_memory_8(a); }
OSP_API unsigned osp_r16(unsigned a) { return m68k_read_memory_16(a); }
OSP_API unsigned osp_r32(unsigned a) { return m68k_read_memory_32(a); }

OSP_API void osp_heap_init(unsigned base, unsigned size)
{
    g_heap_base = g_heap_next = base;
    g_heap_end = base + size;
}
OSP_API void osp_enable_mem_traps(int on) { g_mem_traps = on ? 1 : 0; }
OSP_API unsigned osp_heap_used(void) { return g_heap_next - g_heap_base; }

/* Load a resource into emulated memory and register it under (type, id).
 * Returns the Handle, or 0 if the heap or the table is full.
 *
 * IDs are the driver's, not the file's: `.sp` asks for TALK 1 while
 * outspoken.bin stores TALK 1001.  The +1000 mapping is the caller's problem,
 * deliberately -- it is a fact about outSPOKEN, not about the Resource
 * Manager. */
OSP_API unsigned osp_add_resource(unsigned type, int id,
                                  const unsigned char *data, int len)
{
    unsigned blk, mp;
    if (g_res_count >= MAX_RES) return 0;
    blk = heap_alloc((unsigned)len);
    if (!blk) return 0;
    memcpy(g_ram + blk, data, (size_t)len);
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    g_res[g_res_count].type = type;
    g_res[g_res_count].id = (short)id;
    g_res[g_res_count].handle = mp;
    g_res_count++;
    return mp;
}

OSP_API unsigned osp_tick_count(void) { return g_ticks; }

OSP_API void osp_set_trap_policy(unsigned word, unsigned d0, unsigned a0)
{
    TrapPolicy *p = &g_policy[word & 0x0FFFu];
    p->have = 1; p->d0 = d0; p->a0 = a0;
}

OSP_API void osp_set_reg(int reg, unsigned v) { m68k_set_reg((m68k_register_t)reg, v); }
OSP_API unsigned osp_get_reg(int reg) { return m68k_get_reg(NULL, (m68k_register_t)reg); }

/* Call a subroutine.  `sentinel` is pushed as the return address, so an `rts`
 * out of the routine lands on it and stops us cleanly. */
OSP_API int osp_call(unsigned entry, unsigned sentinel, long long max_instr)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    sp -= 4;
    m68k_write_memory_32(sp, sentinel);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, entry);

    g_sentinel = sentinel;
    g_stop_reason = STOP_RUNNING;
    g_stop_vector = -1;
    g_instr_count = 0;
    g_instr_budget = max_instr;

    while (g_stop_reason == STOP_RUNNING) {
        m68k_execute(100000);
        if (g_cb_pending && g_stop_reason == STOP_RUNNING)
            run_pending_callback();
    }

    return g_stop_reason;
}

/* Call a routine that takes Pascal arguments already pushed by the caller.
 * MACSTARTSOUND is reached this way, through the export table at driver+$001E. */
OSP_API int osp_call_with_args(unsigned entry, const unsigned *args, int nargs,
                               long long max_instr)
{
    unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
    int i;
    for (i = 0; i < nargs; i++) {
        sp -= 4;
        m68k_write_memory_32(sp, args[i]);
    }
    m68k_set_reg(M68K_REG_SP, sp);
    return osp_call(entry, MAGIC_SENTINEL, max_instr);
}

OSP_API unsigned osp_pcm_len(void)      { return g_pcm_len; }
OSP_API int  osp_buffers_taken(void)    { return g_buffers_taken; }
OSP_API int  osp_pcm_overflow(void)     { return g_pcm_overflow; }
OSP_API int  osp_short_buffers(void)    { return g_short_buffers; }
OSP_API unsigned osp_sample_rate(void)  { return g_sample_rate; }
OSP_API void osp_pcm_reset(void)
{
    g_pcm_len = 0; g_buffers_taken = 0; g_pcm_overflow = 0; g_short_buffers = 0;
}
OSP_API int osp_pcm_get(unsigned char *out, int max)
{
    unsigned n = g_pcm_len;
    if ((int)n > max) n = (unsigned)max;
    memcpy(out, g_pcm, n);
    return (int)n;
}
OSP_API unsigned osp_cb_scratch(void)   { return CB_SCRATCH; }

OSP_API int osp_buflog_n(void) { return g_buflog_n; }
OSP_API int osp_buflog_get(int i, unsigned *addr, unsigned *len)
{
    if (i < 0 || i >= g_buflog_n) return -1;
    *addr = g_buflog_addr[i]; *len = g_buflog_len[i];
    return 0;
}

OSP_API void osp_snap_set(unsigned pc) { g_snap_pc = pc; g_snap_n = 0; }
OSP_API void osp_snap_halt(int nth) { g_snap_halt = nth; }
OSP_API int  osp_snap_n(void) { return g_snap_n; }
OSP_API int  osp_snap_get(int i, unsigned *out)
{
    int k;
    if (i < 0 || i >= g_snap_n) return -1;
    for (k = 0; k < 17; k++) out[k] = g_snap[i][k];
    return 0;
}

OSP_API void osp_rwatch_set(unsigned lo, unsigned hi)
{ g_rwatch_lo = lo; g_rwatch_hi = hi; g_rwatch_n = 0; }
OSP_API int osp_rwatch_n(void) { return g_rwatch_n; }
/* Oldest-first over whatever the ring still holds. */
OSP_API int osp_rwatch_get(int i, unsigned *pc, unsigned *addr)
{
    int have = g_rwatch_n < RWATCH_CAP ? g_rwatch_n : RWATCH_CAP;
    int start = g_rwatch_n - have;
    if (i < 0 || i >= have) return -1;
    *pc = g_rwatch_pc[(start + i) % RWATCH_CAP];
    *addr = g_rwatch_addr[(start + i) % RWATCH_CAP];
    return 0;
}

OSP_API void osp_watch_set(unsigned lo, unsigned hi)
{ g_watch_lo = lo; g_watch_hi = hi; g_watch_n = 0; }
OSP_API int osp_watch_n(void) { return g_watch_n; }
OSP_API int osp_watch_get(int i, unsigned *pc, unsigned *addr, unsigned *val, int *size)
{
    if (i < 0 || i >= g_watch_n || i >= WATCH_CAP) return -1;
    *pc = g_watch[i].pc; *addr = g_watch[i].addr;
    *val = g_watch[i].val; *size = g_watch[i].size;
    return 0;
}

OSP_API void osp_trace_enable(int on) { g_trace_on = on ? 1 : 0; g_trace_pos = 0; }
OSP_API unsigned osp_trace_len(void)
{
    return g_trace_pos < TRACE_CAP ? g_trace_pos : TRACE_CAP;
}
/* Copy the trace out oldest-first. */
OSP_API int osp_trace_get(unsigned *out, int max)
{
    unsigned n = osp_trace_len(), i, start;
    if ((int)n > max) n = (unsigned)max;
    start = g_trace_pos - n;
    for (i = 0; i < n; i++)
        out[i] = g_trace[(start + i) & (TRACE_CAP - 1u)];
    return (int)n;
}

OSP_API int  osp_stop_reason(void)  { return g_stop_reason; }
OSP_API int  osp_stop_vector(void)  { return g_stop_vector; }
OSP_API unsigned osp_stop_pc(void)  { return g_stop_pc; }
OSP_API long long osp_instr_count(void) { return g_instr_count; }
OSP_API int  osp_trap_count(void)   { return g_trap_count; }
OSP_API int  osp_trap_overflow(void){ return g_trap_overflow; }
OSP_API int  osp_stub_count(void)   { return g_stub_count; }
OSP_API int  osp_fault_count(void)  { return g_fault_count; }
OSP_API int  osp_stackpc_convention(void) { return g_stackpc_is_instruction; }

OSP_API int osp_trap_get(int i, unsigned *pc, unsigned *word, unsigned *d0,
                         unsigned *a0, unsigned *a1, int *served)
{
    if (i < 0 || i >= g_trap_count) return -1;
    *pc = g_traps[i].pc; *word = g_traps[i].word; *d0 = g_traps[i].d0;
    *a0 = g_traps[i].a0; *a1 = g_traps[i].a1; *served = g_traps[i].served;
    return 0;
}

OSP_API int osp_fault_get(int i, unsigned *addr, unsigned *pc, int *write, int *size)
{
    if (i < 0 || i >= g_fault_count || i >= MAX_FAULTS) return -1;
    *addr = g_faults[i].addr; *pc = g_faults[i].pc;
    *write = g_faults[i].write; *size = g_faults[i].size;
    return 0;
}

OSP_API unsigned osp_magic_sentinel(void) { return MAGIC_SENTINEL; }
