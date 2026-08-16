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
    /* D0 as the caller set it.  `d0` above is what we answered with, and for
     * the traps that take their argument in D0 -- _Gestalt, _GetTrapAddress,
     * _NewHandle -- that overwrites the only record of what was asked. */
    unsigned d0_in;
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
/* MemErr.  MacinTalk 2's Open reads it directly (`move.w $220.w,d0` at Cecy 3
 * +$16A) as the error it returns when its own _NewHandle came back nil, so an
 * uninitialised word here turns an allocation failure into a random error
 * code -- or worse, an allocation *success* reported as a failure. */
#define MEM_ERR_ADDR  0x0220u

/* _GetTrapAddress answers, one distinct address per trap word.
 *
 * These are never executed, only compared: `TrapAvailable()` asks for a trap
 * and for _Unimplemented ($A89F) and calls the feature present when the two
 * differ.  Handing back a constant would report every trap missing, and
 * feature detection would take the wrong branch in silence -- so the address
 * has to be a function of the trap number. */
#define TRAPADDR_BASE 0x00F28000u

static unsigned g_ticks;          /* _TickCount, one per call */

/* The Deferred Task Manager.
 *
 * This is what actually makes MacinTalk 2 synthesise.  Its sound callback does
 * not render: it fills in a DeferredTask at storage+$B6 and installs it, and
 * the *task* renders.  Nothing else reaches the sample state machine, so with
 * $A082 stubbed the engine clears its buffers to silence, plays them, and
 * never touches the voice's wave table at all. */
static int      g_dt_pending;
static int      g_in_deferred;
static unsigned g_dt_proc, g_dt_parm;
static int      g_dt_runs;


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
        *a0_out = p; *d0_out = p ? 0 : (unsigned)(-108) /* memFullErr */;
        m68k_write_memory_16(MEM_ERR_ADDR, p ? 0u : 0xFF94u);
        return 1;
    }
    case 0xA122: {                     /* _NewHandle -- size in D0, hdl A0 */
        unsigned h = heap_new_handle(d0);
        *a0_out = h; *d0_out = h ? 0 : (unsigned)(-108);
        m68k_write_memory_16(MEM_ERR_ADDR, h ? 0u : 0xFF94u);
        return 1;
    }
    case 0xA1AD:                       /* _Gestalt -- selector D0, resp A0 */
        /* Reached through the TrapAvailable() pattern at Cecy 1 +$58A4, and
         * the engine carries its own answer table for the case where Gestalt
         * is missing.  We report it present -- the selector it asks for is in
         * the trap log's D0-in column, and the answer below is chosen from
         * that rather than from what a Mac would have said. */
        *a0_out = 0;
        *d0_out = 0;                   /* noErr */
        return 1;
    case 0xA090: {                     /* _SysEnvirons(version, SysEnvRec*) */
        /* Cecy 1 +$100A asks for version 2 and compares systemVersion with
         * $0700 to decide whether it is on System 7.  These resources came
         * off a System 7 disk, so saying so is the truthful answer. */
        unsigned rec = a0;
        m68k_write_memory_16(rec + 0, 2);        /* environsVersion         */
        m68k_write_memory_16(rec + 2, 9);        /* machineType             */
        m68k_write_memory_16(rec + 4, 0x0700);   /* systemVersion, System 7 */
        m68k_write_memory_16(rec + 6, 1);        /* processor: env68000     */
        m68k_write_memory_8 (rec + 8, 0);        /* hasFPU                  */
        m68k_write_memory_8 (rec + 9, 0);        /* hasColorQD              */
        m68k_write_memory_16(rec + 10, 0);       /* keyBoardType            */
        m68k_write_memory_16(rec + 12, 0);       /* atDrvrVersNum           */
        m68k_write_memory_16(rec + 14, 0);       /* sysVRefNum              */
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    /* The Time Manager.  MacinTalk 2's back end installs eight tasks at
     * Cecy 1 +$38F6, over 26-byte records whose filled fields -- long at +0,
     * word at +4, long at +$A, word at +$E -- are TMTask's qLink, qType,
     * tmCount and tmWakeUp.
     *
     * Accepting and ignoring them is right for this host rather than merely
     * convenient: we take every sound buffer the instant it is offered and
     * fire the callback immediately, so nothing here ever waits on a clock.
     * A timer that never fires cannot stall what never waits. */
    case 0xA082: {                     /* _DTInstall -- A0 = DeferredTask  */
        /* Named from the record, not from recall.  A snapshot at the call
         * site (Cecy 1 +$3BA2) reads:
         *
         *   +$00 qLink    00000000
         *   +$04 qType    0007         <- dtQType
         *   +$06 dtFlags  0000
         *   +$08 dtAddr   00063A5A     <- back end +$3A5A
         *   +$0C dtParam  00093BE8     <- the back end's storage
         *   +$10 dtReserved 00000000
         *
         * which is DeferredTask field for field.  The task is entered with
         * A1 = dtParam, and +$3A62 confirms it: `move.l a1,d0 / movea.l d0,a4`
         * with a4 used as storage from there on.
         *
         * Running it here would be re-entering the CPU mid-instruction, so
         * take the record now and run it once the machine is between calls --
         * the same shape the sound callback already uses. */
        g_dt_proc = m68k_read_memory_32(a0 + 8);
        g_dt_parm = m68k_read_memory_32(a0 + 12);
        g_dt_pending = 1;
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA058:                       /* _InsTime                         */
    case 0xA059:                       /* _RmvTime                         */
    case 0xA05A:                       /* _PrimeTime                       */
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA193: {                     /* _Microseconds -- A0 = UnsignedWide */
        /* Only reached because we told SysEnvirons this is System 7.  It must
         * advance, or anything measuring elapsed time sees zero forever; the
         * instruction count is the one monotonic clock this host has. */
        unsigned long long us = (unsigned long long)g_instr_count;
        m68k_write_memory_32(a0 + 0, (unsigned)(us >> 32));
        m68k_write_memory_32(a0 + 4, (unsigned)(us & 0xFFFFFFFFu));
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA146:                       /* _GetTrapAddress -- number in D0  */
        *a0_out = TRAPADDR_BASE + ((d0 & 0x0FFFu) * 4u);
        *d0_out = 0;
        return 1;
    case 0xA029:                       /* _HLock   -- nothing moves here   */
    case 0xA02A:                       /* _HUnlock                         */
    case 0xA023:                       /* _DisposeHandle                   */
    case 0xA01F:                       /* _DisposePtr                      */
    case 0xA049:                       /* _HPurge                          */
    case 0xA04A:                       /* _HNoPurge                        */
    case 0xA036:                       /* _MoreMasters                     */
    /* Both of these only matter to a real, compacting heap.  Serving them
     * explicitly rather than letting them stub keeps g_stub_count meaning
     * "something we have not implemented", which is the alarm it exists to
     * be. */
    case 0xA040:                       /* _ResrvMem                        */
    case 0xA064:                       /* _MoveHHi                         */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
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
/* `bytes`/`len` are the resource exactly as it came off the disk image, kept
 * host-side so a detached resource can be handed back unmodified.  See
 * res_find and _DetachResource below -- this is not belt and braces, it is
 * the fix for MacinTalk 2's voice switching. */
typedef struct { unsigned type; short id; unsigned handle;
                 unsigned char *bytes; int len; int detached; } ResEntry;
static ResEntry g_res[MAX_RES];
static int      g_res_count;
static int      g_res_load = 1;
static short    g_res_err;

#define RES_NOT_FOUND (-192)      /* resNotFound */

/* Every resource the engine asked for, and whether we had it.
 *
 * `.sp` needed this once, to discover it wanted TALK 1 while outSPOKEN stores
 * TALK 1001.  MacinTalk 2 asks for far more, and a voice that will not load
 * reports only resNotFound -- which says nothing about *what* was missing. */
#define MAX_RESLOG 256
typedef struct { unsigned type; short id; int found; } ResReq;
static ResReq g_reslog[MAX_RESLOG];
static int    g_reslog_n;

/* Which voices the Speech Manager knows about.
 *
 * MacinTalk 2 does not find a voice by rummaging: it asks the Speech Manager
 * for the voice's *file*, then opens that file and reads a resource id out of
 * the answer.  We are the Speech Manager, so this table is that answer.  Python
 * fills it from each voice's own `ttvd` -- see tools/voices.py. */
#define MAX_VOICES 32
typedef struct { unsigned creator, id; short res_id; } VoiceReg;
static VoiceReg g_voices[MAX_VOICES];
static int      g_voice_count;

static void log_res(unsigned type, short id, int found)
{
    if (g_reslog_n < MAX_RESLOG) {
        g_reslog[g_reslog_n].type = type;
        g_reslog[g_reslog_n].id = id;
        g_reslog[g_reslog_n].found = found;
        g_reslog_n++;
    }
}

/* Hand back a resource, restoring it first if the engine took it away.
 *
 * A real Resource Manager reads a resource from the file every time it is not
 * already in memory, so a client always gets it as the author wrote it.  We
 * kept one block per resource and returned the same handle forever, which is
 * fine until a client MODIFIES what it is given -- and MacinTalk 2 does.
 *
 * Measured, not deduced: loading a voice rewrites 1,548 of its `ttvi`'s 8,644
 * bytes in place, 17.9% of the resource, while `ttvd` and `ttvw` are left
 * alone.  So `ttvi` is patched as it is loaded.  Select that voice a second
 * time and the engine patched its own output again; the voice then rendered
 * endless buffers of noise, which is what NVDA users heard as buzzing after
 * switching voices and what made an eventual further switch clear it.  Eight
 * *different* voices in a row were always fine -- it took a revisit.
 *
 * The engine says which resources it means to keep: it calls _DetachResource
 * on them, which in a real Resource Manager removes the resource from the map
 * and makes the block the caller's own.  So detaching is the signal to
 * restore, and nothing else changes behaviour.
 *
 * The block is reused rather than reallocated.  Our _DisposeHandle is a no-op
 * and the heap only ever grows, so a fresh block per switch would exhaust it
 * after a few dozen; the same block with the original bytes back in it is
 * what the engine would have got from the file anyway. */
static unsigned res_find(unsigned type, short id)
{
    int i;
    for (i = 0; i < g_res_count; i++)
        if (g_res[i].type == type && g_res[i].id == id) {
            ResEntry *r = &g_res[i];
            if (r->detached && r->bytes) {
                unsigned blk = m68k_read_memory_32(r->handle);
                if (blk) memcpy(g_ram + blk, r->bytes, (size_t)r->len);
                r->detached = 0;
            }
            return r->handle;
        }
    return 0;
}

/* -> the entry a handle belongs to, or NULL.  Only _DetachResource needs it. */
static ResEntry *res_by_handle(unsigned handle)
{
    int i;
    if (!handle) return NULL;
    for (i = 0; i < g_res_count; i++)
        if (g_res[i].handle == handle) return &g_res[i];
    return NULL;
}

/* --------------------------------------------------- the Component Manager -
 *
 * MacinTalk 2 and MacinTalk Pro are Component Manager components, not DRVRs,
 * so the host plays a second role it did not need for `.sp`: the switchboard
 * that `$A82A` calls into.  See docs/macintalk2-components.md for how the
 * frame layouts below were measured; none of it is recalled.
 *
 * The one idea that makes this cheap: **a component call is a tail call.**  A
 * naive implementation would re-enter the CPU (the way run_pending_callback
 * does) and copy the result back, but that cannot recurse, and the front end
 * calling the back end which calls its own handler is three deep immediately.
 * Instead we rewrite the stack so the callee's Pascal frame lands exactly
 * where the caller's `$A82A` result is expected, give the callee the address
 * just past the trap word as its return address, and let it return straight to
 * the caller.  The host never sees the call end, and never needs to.
 *
 * That works because every one of these leaves SP at the caller's result slot:
 *
 *     D0 = -1  handler frame at csp+4-paramSize; the handler pops its return
 *              address, paramSize bytes of arguments and the storage Handle,
 *              landing on csp+12 -- the slot the glue's caller pushed.
 *     D0 =  0  component main's frame at csp+paramSize-4; main pops 8 bytes
 *              (measured at Cecy 3 +$110: unlk / movea.l (a7)+,a0 / addq #8),
 *              landing on csp+8+paramSize.
 *
 * Both overwrite stack the caller still owns, so every field is read out
 * before anything is written.
 */

#define MAX_COMPONENTS 8
#define MAX_INSTANCES  16

/* A ComponentInstance is an opaque token to the engine, so it can be anything
 * non-zero that we recognise on the way back in.  Keeping them in their own
 * magic page means a stale or invented one is obvious rather than plausible. */
#define INSTANCE_BASE  0x00F2C000u
#define INSTANCE_TOK(i) (INSTANCE_BASE + (unsigned)(i) * 4u)

/* A `Component` and a `ComponentInstance` are different things and the engine
 * holds both at once -- FindNextComponent hands back the former, OpenComponent
 * turns it into the latter.  Separate token ranges so mixing them up is caught
 * here rather than becoming a plausible-looking wrong answer. */
#define COMPONENT_BASE 0x00F2C100u
#define COMPONENT_TOK(i) (COMPONENT_BASE + (unsigned)(i) * 4u)

/* Where a `D0 = 0` call's ComponentParameters is copied so the frame we build
 * on top of it cannot clobber the arguments it describes.
 *
 * A ring, not a stack, because tail-calling means the host is never told that
 * a call finished -- slots come back by wrap-around alone.  64 is far past the
 * nesting these two components can reach; g_cp_wraps gives a corruption a
 * signature instead of leaving it to look like an engine bug. */
#define CP_SCRATCH   0x00F22000u
#define CP_SLOTS     64
#define CP_SLOT_SZ   264u          /* 4 + a paramSize that cannot exceed 255 */

/* The host's own top-level call lives for a whole utterance, so it must never
 * come from the ring. */
#define CP_TOPLEVEL  0x00F26400u

typedef struct {
    unsigned type, subtype, manuf;
    unsigned entry;                 /* code resource, already loaded into RAM */
} Component;

typedef struct {
    int      component;             /* index into g_comp; -1 when free */
    unsigned storage;               /* the Handle set through selector $11 */
    unsigned refcon;                /* a long the component parks on itself */
} Instance;

static Component g_comp[MAX_COMPONENTS];
static int       g_comp_count;
static Instance  g_inst[MAX_INSTANCES];
static int       g_inst_count;
static int       g_cp_slot, g_cp_wraps;

/* OpenComponent is the one call a tail call cannot express.
 *
 * Every other selector either answers immediately or hands control to exactly
 * one callee, so the callee can return straight to the original caller.  Open
 * has to do both: run the component's own Open handler, *and then* give the
 * caller an instance rather than whatever that handler returned.  Two returns,
 * one of which is ours.
 *
 * So the handler is sent back to a magic address instead, and the pending
 * record here says what to do when execution reaches it.  A stack rather than
 * a single slot, because a component's Open may open another component --
 * which is exactly what the front end does to the back end. */
#define MAGIC_COPEN_RET 0x00F11100u

#define MAX_PENDING 8
typedef struct {
    unsigned result_slot;   /* the caller's, where the instance belongs */
    unsigned resume_pc;     /* where the caller carries on              */
    unsigned inst_token;    /* handed back if the handler said noErr    */
    unsigned callee_result; /* where the handler left its own result    */
} PendingOpen;
static PendingOpen g_pending[MAX_PENDING];
static int         g_pending_n;
/* The frame arithmetic for each CallComponentFunctionWithStorage.
 *
 * Kept because it earned its place: the SR-clobber above was invisible from
 * the 68000 side -- the trace showed the right instructions in the right order
 * and every register looked plausible -- and one look at csp, paramSize and
 * the computed newsp side by side made it obvious. */
#define MAX_FRAMELOG 16
static unsigned    g_framelog[MAX_FRAMELOG][6];
static int         g_framelog_n;
static int         g_copen_ret;   /* set by instr_hook, acted on outside */

/* Every `$A82A` selector seen, whether or not we knew it.  The first run's log
 * is what settles this engine's surface, exactly as the trap log settled
 * `.sp`'s -- so record the unknown ones with enough stack to decode them. */
#define MAX_CMLOG 512
typedef struct { unsigned d0, pc, csp, w0, w1, w2, w3; int served; } CmRec;
static CmRec g_cmlog[MAX_CMLOG];
static int   g_cmlog_n;

static int comp_of(unsigned token)
{
    unsigned i;
    if (token < COMPONENT_BASE) return -1;
    i = (token - COMPONENT_BASE) / 4u;
    return i < (unsigned)g_comp_count ? (int)i : -1;
}

static int inst_of(unsigned token)
{
    unsigned i;
    if (token < INSTANCE_BASE || token >= COMPONENT_BASE) return -1;
    i = (token - INSTANCE_BASE) / 4u;
    if (i >= (unsigned)g_inst_count || g_inst[i].component < 0) return -1;
    return (int)i;
}

/* Put the exception frame where `rte` will land on `entry` with SP == newsp.
 * The same trick tb_return uses, aimed into a callee instead of past a trap.
 *
 * `sr` is passed in, and must have been read before the caller wrote anything.
 * This is not defensiveness -- it is a bug that already happened.  With twelve
 * bytes of arguments the new frame lands *below* the old one, and the return
 * address written at `newsp` covers `exc_sp` and `exc_sp+1`, which is exactly
 * where the saved SR lives.  Reading SR here, after that write, picked up the
 * low half of a return address instead: supervisor bit clear, so `rte` dropped
 * to user mode, A7 became the (zero) USP, and the callee ran with SP = 0
 * scribbling on low memory.  Every earlier call happened to have four bytes of
 * arguments, which puts the new frame exactly on top of the old one and hides
 * this completely. */
static void enter_callee(unsigned sr, unsigned newsp, unsigned entry)
{
    unsigned frame = newsp - EXC_FRAME;
    m68k_write_memory_16(frame, sr);
    m68k_write_memory_32(frame + 2, entry);
    m68k_set_reg(M68K_REG_SP, frame);
}

/* Returns 1 if we served it.  `resume_pc` is the address just past the trap
 * word, which doubles as the callee's return address. */
static int serve_component_trap(unsigned d0, unsigned exc_sp, unsigned csp,
                                unsigned resume_pc)
{
    unsigned char tmp[CP_SLOT_SZ];
    /* Read before any case writes: several of them build a frame that overlaps
     * the exception frame this comes from.  See enter_callee. */
    unsigned sr = m68k_read_memory_16(exc_sp);
    /* Snapshot the caller's stack *now*.  Every branch below rewrites it to
     * build the callee's frame, so reading it at the end of this function --
     * which is what the log used to do -- described our own output rather
     * than the engine's input, and reported a 'stat' call as `what=0
     * paramSize=6`. */
    unsigned w0 = m68k_read_memory_32(csp + 0);
    unsigned w1 = m68k_read_memory_32(csp + 4);
    unsigned w2 = m68k_read_memory_32(csp + 8);
    unsigned w3 = m68k_read_memory_32(csp + 12);
    unsigned i;
    int served = 0;

    switch (d0) {

    case 0xFFFFFFFFu: {         /* CallComponentFunctionWithStorage           */
        /* `moveq #$FF,d0` sign-extends, so the selector arrives as -1, not
         * $FF.  Comparing against $FF here would silently miss every call. */
        unsigned handler = w0, params = w1, storage = w2;
        unsigned psize   = m68k_read_memory_8(params + 1);
        unsigned newsp;

        if (psize > 255u) psize = 255u;
        if (g_framelog_n < MAX_FRAMELOG) {
            unsigned *f = g_framelog[g_framelog_n++];
            f[0] = csp;   f[1] = params;         f[2] = psize;
            f[3] = handler; f[4] = csp + 4u - psize; f[5] = exc_sp;
        }
        /* Bounce through C: params may itself live on the stack we are about
         * to rewrite, and a straight copy would then read its own output. */
        for (i = 0; i < psize; i++)
            tmp[i] = (unsigned char)m68k_read_memory_8(params + 4 + i);

        newsp = csp + 4u - psize;
        for (i = 0; i < psize; i++)
            m68k_write_memory_8(newsp + 4u + i, tmp[i]);
        /* Storage is the handler's *first* declared argument, so it sits above
         * every unpacked one -- four handlers across both components agree. */
        m68k_write_memory_32(newsp + 4u + psize, storage);
        m68k_write_memory_32(newsp, resume_pc);
        enter_callee(sr, newsp, handler);
        served = 1;
        break;
    }

    case 0x00000000u: {         /* call another component instance            */
        unsigned hdr   = w0;
        unsigned psize = (hdr >> 16) & 0xFFu;
        unsigned token = m68k_read_memory_32(csp + 4u + psize);
        unsigned cp, newsp;
        int ii = inst_of(token);

        if (ii < 0) break;      /* unknown instance -- fall through and halt */

        for (i = 0; i < 4u + psize; i++)
            tmp[i] = (unsigned char)m68k_read_memory_8(csp + i);
        cp = CP_SCRATCH + (unsigned)(g_cp_slot % CP_SLOTS) * CP_SLOT_SZ;
        if (++g_cp_slot % CP_SLOTS == 0) g_cp_wraps++;
        for (i = 0; i < 4u + psize; i++)
            m68k_write_memory_8(cp + i, tmp[i]);

        newsp = csp + psize - 4u;
        m68k_write_memory_32(newsp + 8u, cp);                  /* params     */
        m68k_write_memory_32(newsp + 4u, g_inst[ii].storage);  /* storage    */
        m68k_write_memory_32(newsp, resume_pc);
        enter_callee(sr, newsp, g_comp[g_inst[ii].component].entry);
        served = 1;
        break;
    }

    case 0x0000000Eu: {         /* one argument, one result; see below        */
        /* Measured at Cecy 3 +$134: the front end asks this before allocating
         * storage and uses the answer only to choose between _ResrvMem then
         * _NewHandle, and a plain _NewHandle -- then records the inverse at
         * storage+$21A.  Both branches allocate the same $21C bytes and both
         * are fine against a bump allocator, so 0 is safe.  storage+$21A is
         * the first knob to try if MacinTalk 2 later misbehaves in a way that
         * smells like a heap assumption. */
        tb_return(exc_sp, 4, 0, 4);
        served = 1;
        break;
    }

    case 0x00000004u: {         /* FindNextComponent(prev, desc) -> Component */
        /* Cecy 3 +$17E2 builds a 20-byte ComponentDescription on its own
         * frame -- type 't2be', subType 0, manufacturer 'mtk2', flags 0 -- and
         * returns -240 (`noSynthFound`, straight out of Apple's Speech.h) when
         * this comes back nil.  A zero field is a wildcard, which is why the
         * subType of 0 still has to match our back end's 't2be'. */
        unsigned desc = m68k_read_memory_32(csp + 0);
        unsigned prev = m68k_read_memory_32(csp + 4);
        unsigned wt = m68k_read_memory_32(desc + 0);
        unsigned ws = m68k_read_memory_32(desc + 4);
        unsigned wm = m68k_read_memory_32(desc + 8);
        unsigned found = 0;
        int k, start = 0, pi = comp_of(prev);

        if (pi >= 0) start = pi + 1;        /* "next", so resume past it */
        for (k = start; k < g_comp_count; k++) {
            if (wt && g_comp[k].type != wt) continue;
            if (ws && g_comp[k].subtype != ws) continue;
            if (wm && g_comp[k].manuf != wm) continue;
            found = COMPONENT_TOK(k);
            break;
        }
        tb_return(exc_sp, 8, found, 4);
        served = 1;
        break;
    }

    case 0x00000007u: {         /* OpenComponent(Component) -> instance       */
        /* Cecy 3 +$183E returns -241 (`synthOpenFailed`) when this comes back
         * nil, so a nil answer here is the front end giving up on the back end
         * entirely.  Creating the instance is the easy half; the component's
         * own Open handler has to run before the caller may use it. */
        unsigned ctok = m68k_read_memory_32(csp + 0);
        unsigned tok, cp, newsp;
        int ci = comp_of(ctok), ii;

        if (ci < 0) break;                        /* unknown Component */
        if (g_inst_count >= MAX_INSTANCES) break;
        if (g_pending_n >= MAX_PENDING) break;

        ii = g_inst_count++;
        g_inst[ii].component = ci;
        g_inst[ii].storage = 0;                   /* the handler will set it */
        g_inst[ii].refcon = 0;
        tok = INSTANCE_TOK(ii);

        cp = CP_SCRATCH + (unsigned)(g_cp_slot % CP_SLOTS) * CP_SLOT_SZ;
        if (++g_cp_slot % CP_SLOTS == 0) g_cp_wraps++;
        m68k_write_memory_8(cp + 0, 0);           /* flags                  */
        m68k_write_memory_8(cp + 1, 4);           /* paramSize              */
        m68k_write_memory_16(cp + 2, 0xFFFFu);    /* what = -1, Open        */
        m68k_write_memory_32(cp + 4, tok);        /* params[0] = self       */

        /* Well clear of the caller's frame; the callee grows downward. */
        newsp = csp - 32u;
        m68k_write_memory_32(newsp + 12, 0);      /* the callee's result    */
        m68k_write_memory_32(newsp + 8, cp);      /* params, first declared */
        m68k_write_memory_32(newsp + 4, 0);       /* storage: nil at open   */
        m68k_write_memory_32(newsp + 0, MAGIC_COPEN_RET);

        {
            PendingOpen *p = &g_pending[g_pending_n++];
            p->result_slot = csp + 4;
            p->resume_pc = resume_pc;
            p->inst_token = tok;
            p->callee_result = newsp + 12;
        }
        enter_callee(sr, newsp, g_comp[ci].entry);
        served = 1;
        break;
    }

    case 0x00000015u: {         /* one argument, a *word* result              */
        /* Cecy 3 +$14FC reserves two bytes, passes self, and treats a result
         * <= 0 as failure; on success it immediately fills three pointer
         * slots from three subroutines.  That is a component opening its own
         * resource file and then reading its tables out of it -- Apple most
         * likely calls this OpenComponentResFile.
         *
         * Named here by what it does rather than by what it is probably
         * called, because the evidence supports the behaviour and only
         * suggests the name.  Our _GetResource searches one flat table and
         * ignores files entirely, so any positive refNum will do; the proof
         * that this reading is right is whether the resource calls that
         * follow ask for ttsr/ttsd/ttss. */
        tb_return(exc_sp, 4, 1, 2);
        served = 1;
        break;
    }

    case 0x00000018u: {         /* the close that matches $15                 */
        /* Cecy 3 +$1578 hands back the same *word* refNum $15 returned, once
         * the three tables are loaded and detached, and ignores the result.
         * CloseComponentResFile in all but the name.  Note the argument is a
         * word, not a long -- two bytes of arguments, not four. */
        tb_return(exc_sp, 2, 0, 2);
        served = 1;
        break;
    }

    case 0x0000000Du: {         /* SetComponentInstanceStorage(self, storage) */
        /* $D and $11 have the same shape -- (self, a long), no result -- and
         * both are called once during each component's Open, so the shape
         * cannot tell them apart.  The engine's own later use does.
         *
         * The front end allocates two blocks: $21C, handed to $11 early, and
         * $322, handed to $D at the very end of Open.  It writes the back-end
         * ComponentInstance into **the $322 block at +$4**, and its handlers
         * then reach that instance as `$4(deref(storage))` -- at Cecy 3 +$171E
         * and +$5F8.  Only the $322 block satisfies that, so $D is the one
         * setting storage and $11 is parking something else.
         *
         * The back end agrees: it gives $11 a `_NewPtr` block and $D a real
         * Handle, and its handlers dereference storage once, which is only
         * meaningful for the Handle. */
        unsigned storage = m68k_read_memory_32(csp + 0);
        int ii = inst_of(m68k_read_memory_32(csp + 4));
        if (ii >= 0) g_inst[ii].storage = storage;
        tb_return(exc_sp, 8, 0, 0);
        served = 1;
        break;
    }

    case 0x00000010u: {         /* GetComponentInstanceStorage(self)          */
        int ii = inst_of(m68k_read_memory_32(csp));
        tb_return(exc_sp, 4, ii < 0 ? 0 : g_inst[ii].storage, 4);
        served = 1;
        break;
    }

    case 0x00000011u: {         /* park a long on the instance -- see $D      */
        /* Two arguments and no result slot: the call site at Cecy 3 +$17E
         * never reserves one, so writing four bytes there would land in the
         * caller's locals. */
        unsigned value = m68k_read_memory_32(csp + 0);
        int ii = inst_of(m68k_read_memory_32(csp + 4));
        if (ii >= 0) g_inst[ii].refcon = value;
        tb_return(exc_sp, 8, 0, 0);
        served = 1;
        break;
    }

    default:
        break;
    }

    if (g_cmlog_n < MAX_CMLOG) {
        CmRec *r = &g_cmlog[g_cmlog_n++];
        r->d0 = d0; r->pc = resume_pc - 2u; r->csp = csp; r->served = served;
        r->w0 = w0; r->w1 = w1; r->w2 = w2; r->w3 = w3;
    }

    /* An unknown selector must never be stubbed.  A Toolbox stub leaves the
     * stack unbalanced and the caller returns into rubbish several
     * instructions later, which is a much harder bug than stopping here with
     * the stack still intact and readable. */
    if (!served) {
        g_stop_reason = STOP_EXCEPTION;
        g_stop_vector = 10;
        g_stop_pc = resume_pc - 2u;
        m68k_end_timeslice();
    }
    return 1;
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
#define MAGIC_DT_RET  0x00F11200u   /* a deferred task has returned */
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
static int      g_cb_runs;        /* callbacks actually executed */


/* Whether a sound callback may run while the engine is still mid-call.
 *
 * `.sp` wanted it to: its Prime rendered a whole utterance synchronously, so
 * answering the callback the instant the buffer was offered kept it running
 * flat out.  MacinTalk 2 is double-buffered and asynchronous, and its callback
 * (Cecy 1 +$3B1E) only refills on its *second* invocation -- the first just
 * sets a flag.  Firing early therefore burns that first callback before the
 * engine has queued anything for it to be about.
 *
 * So this is per-engine policy, set from Python, not a global truth. */
static int      g_defer_cb;

/* Every Sound Manager command, in order.  MacinTalk 2 drives audio
 * asynchronously, so "why did it stop after one buffer" is a question about
 * the *sequence* of commands, which no single counter can answer. */
#define MAX_SNDLOG 512
static unsigned short g_sndlog[MAX_SNDLOG];
static int            g_sndlog_n;

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

        if (g_sndlog_n < MAX_SNDLOG) g_sndlog[g_sndlog_n++] = (unsigned short)cmd;
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
    case 0xA807: {                      /* _SndNewChannel                 */
        /* SndNewChannel(SndChannelPtr *chan, short synth, long init,
         *               SndCallBackUPP userRoutine)
         *
         * Read off the call site at Cecy 1 +$A64: two bytes of result space,
         * then chan, synth (a *word*), init, userRoutine -- 14 bytes.
         *
         * `.sp` never needed this because it was handed a channel; MacinTalk 2
         * makes its own, and the callback it registers here is the one the
         * existing sound model already looks for at SndChannel+8.  So filling
         * that field in is what wires MacinTalk 2's audio into the path `.sp`
         * already uses. */
        unsigned user_routine = m68k_read_memory_32(csp + 0);
        unsigned chanpp = m68k_read_memory_32(csp + 10);
        unsigned chan = heap_alloc(1064);       /* SndChannel + its queue */
        if (chan) {
            m68k_write_memory_32(chan + 8, user_routine);   /* callBack */
            m68k_write_memory_32(chanpp, chan);
        }
        tb_return(exc_sp, 14, chan ? 0u : 0xFF94u, 2);
        return 1;
    }
    case 0xA801:                        /* _SndDisposeChannel(chan, quiet) */
        /* Nothing to tear down: the bump allocator does not free, and we take
         * every buffer the moment it is offered, so no queue can be left. */
        tb_return(exc_sp, 6, 0, 2);
        return 1;
    case 0xA800: {                      /* _SoundDispatch, selector in D0 */
        unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
        unsigned selector = d0 & 0xFFFFu;

        /* The Speech Manager rides on the Sound Manager's trap, so this is not
         * a sound call at all:
         *
         *     GetVoiceInfo(VoiceSpec *voice, OSType 'fref', void *info)
         *
         * read off Cecy 1 +$7FC, where D0 is $0614000C -- twelve bytes of
         * arguments in the low word -- and the selector pushed is 'fref',
         * which Apple's Speech.h calls soVoiceFile.
         *
         * `info` is a VoiceFileInfo: an FSSpec (vRefNum, parID, Str63 name =
         * 70 bytes) followed by **resID at +70**.  That resID is the whole
         * point: the engine opens the file and immediately does
         * _Get1Resource('ttvd', resID).  Our Resource Manager keeps one flat
         * table and no files, so the FSSpec can be nominal, but the resID has
         * to be right. */
        if (d0 == 0x0614000Cu) {
            unsigned info    = m68k_read_memory_32(csp + 0);
            unsigned vspec   = m68k_read_memory_32(csp + 8);
            unsigned creator = m68k_read_memory_32(vspec + 0);
            unsigned vid     = m68k_read_memory_32(vspec + 4);
            int k, found = 0;
            short res_id = 0;
            for (k = 0; k < g_voice_count; k++) {
                if (g_voices[k].creator == creator && g_voices[k].id == vid) {
                    res_id = g_voices[k].res_id; found = 1; break;
                }
            }
            if (found) {
                m68k_write_memory_16(info + 0, 0);      /* FSSpec.vRefNum   */
                m68k_write_memory_32(info + 2, 0);      /* FSSpec.parID     */
                m68k_write_memory_8 (info + 6, 0);      /* FSSpec.name, ""  */
                m68k_write_memory_16(info + 70,
                                     (unsigned)(unsigned short)res_id);
            }
            /* -244 is voiceNotFound, which is what the caller expects when a
             * VoiceSpec names something that is not installed. */
            tb_return(exc_sp, 12, found ? 0u : 0xFF0Cu, 2);
            return 1;
        }
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
static int serve_toolbox_trap(unsigned short word, unsigned exc_sp,
                              unsigned d0, unsigned resume_pc)
{
    unsigned csp = exc_sp + EXC_FRAME;
    /* A Toolbox trap number is ten bits; only bit 10 is a flag (auto-pop).
     * Masking bit 9 as well -- the OS-trap rule -- turns $A9A0 into $A8A0 and
     * every resource call falls through to "stubbed". */
    unsigned base = word & 0xFBFFu;

    /* _ComponentDispatch carries its selector in D0 rather than on the stack,
     * which is why this one trap needs a register the others do not. */
    if (base == 0xA82Au)
        return serve_component_trap(d0, exc_sp, csp, resume_pc);

    if (serve_sound_trap(base, exc_sp, csp)) return 1;

    switch (base) {
    /* _Get1Resource searches only the current file where _GetResource walks
     * the whole chain.  We keep one flat table and no files at all, so the
     * distinction cannot arise -- but the trap still has to be *served*, since
     * a Toolbox stub leaves six bytes on the stack and the caller carries on
     * with a corrupted frame.  That is precisely what happened on the first
     * run: the _DetachResource after it reported D0 = 0x00017474. */
    case 0xA81F:                         /* _Get1Resource(type, id)          */
    case 0xA9A0: {                       /* _GetResource(type, id) -> Handle */
        short id = (short)m68k_read_memory_16(csp);
        unsigned type = m68k_read_memory_32(csp + 2);
        unsigned h = res_find(type, id);
        log_res(type, id, h != 0);
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 6, h, 4);
        return 1;
    }
    case 0xA992: {                       /* _DetachResource(h)               */
        /* The resource leaves the map and the block becomes the caller's, so
         * whatever it does to those bytes from here on is its own business --
         * and MacinTalk 2 patches its `ttvi` as it loads a voice.  Mark it, so
         * that asking for it again gets the original back.  See res_find. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        if (r) r->detached = 1;
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, 0, 0);
        return 1;
    }
    case 0xA9A2:                         /* _LoadResource(h) -- already in   */
    case 0xA9A3:                         /* _ReleaseResource(h)              */
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
    case 0xA81A:                         /* _HOpenResFile -> short refNum    */
        /* HOpenResFile(short vRefNum, long dirID, ConstStr255Param fileName,
         *              SignedByte permission)
         *
         * Twelve bytes of arguments: the byte permission still costs two,
         * because `move.b <ea>,-(a7)` on the 68000 keeps A7 even.
         *
         * The engine only wants a file to read the voice out of, and every
         * resource we have is already in one flat table, so any positive
         * refNum will do -- what matters is that it is not -1, which is the
         * value Cecy 1 +$830 tests for. */
        tb_return(exc_sp, 12, 1, 2);
        return 1;
    case 0xA99A:                         /* _CloseResFile(short)             */
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    case 0xA9AF:                         /* _ResError -> short               */
        /* Only ever reached on a failure path -- MacinTalk 2 calls it to turn
         * "the Handle came back nil" into an error code to return. */
        tb_return(exc_sp, 0, (unsigned)(unsigned short)g_res_err, 2);
        return 1;
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
        served = serve_toolbox_trap(word, sp, d0, trap_pc + 2);
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
        t->d0_in = m68k_get_reg(NULL, M68K_REG_D0);
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
    g_cb_runs++;

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

/* The second half of OpenComponent, run with the machine stopped.
 *
 * The component's Open handler has just returned to the magic address.  Its
 * own result says whether it succeeded; the caller wants an instance, or nil
 * if it did not.  Then put the caller back exactly where the tail-call
 * convention would have left it: SP on its result slot, PC past the trap. */
static void finish_open_component(void)
{
    PendingOpen *p;
    unsigned res;

    g_copen_ret = 0;
    if (g_pending_n <= 0) return;
    p = &g_pending[--g_pending_n];

    res = m68k_read_memory_32(p->callee_result);
    m68k_write_memory_32(p->result_slot, res == 0 ? p->inst_token : 0);
    m68k_set_reg(M68K_REG_SP, p->result_slot);
    m68k_set_reg(M68K_REG_PC, p->resume_pc);
}

/* Run one deferred task, outside m68k_execute.
 *
 * Same construction as run_pending_callback, with two differences: the task
 * takes its argument in A1 rather than on the stack, and it ends in `rts`, so
 * a magic return address on the stack is all that is needed to catch it. */
static void run_pending_deferred(void)
{
    unsigned proc = g_dt_proc, parm = g_dt_parm;
    unsigned save_pc, save_sp, save_a1, sp;

    g_dt_pending = 0;
    if (!proc) return;
    g_dt_runs++;

    save_pc = m68k_get_reg(NULL, M68K_REG_PC);
    save_sp = m68k_get_reg(NULL, M68K_REG_SP);
    save_a1 = m68k_get_reg(NULL, M68K_REG_A1);

    sp = save_sp - 4;
    m68k_write_memory_32(sp, MAGIC_DT_RET);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_A1, parm);
    m68k_set_reg(M68K_REG_PC, proc);

    g_in_deferred = 1;
    while (g_in_deferred && g_stop_reason == STOP_RUNNING)
        m68k_execute(100000);
    g_in_deferred = 0;

    m68k_set_reg(M68K_REG_PC, save_pc);
    m68k_set_reg(M68K_REG_SP, save_sp);
    m68k_set_reg(M68K_REG_A1, save_a1);
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
    /* A component's Open handler has returned.  Finish the OpenComponent that
     * started it -- outside m68k_execute, the way the sound callback does it,
     * rather than redirecting PC from inside the hook. */
    if (pc == MAGIC_DT_RET) {
        g_in_deferred = 0;
        m68k_end_timeslice();
        return;
    }
    if (pc == MAGIC_COPEN_RET) {
        g_copen_ret = 1;
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
    /* osp_init doubles as "start over", and the driver rebuilds the emulator
     * whenever the user crosses between engines, so the pristine copies from
     * the previous life have to go or every switch leaks a voice's worth. */
    for (v = 0; v < g_res_count; v++) {
        if (g_res[v].bytes) free(g_res[v].bytes);
        g_res[v].bytes = NULL; g_res[v].len = 0; g_res[v].detached = 0;
    }
    g_res_count = 0; g_reslog_n = 0; g_voice_count = 0; g_res_load = 1; g_res_err = 0; g_ticks = 0;
    g_comp_count = 0; g_inst_count = 0;
    g_cmlog_n = 0; g_cp_slot = 0; g_cp_wraps = 0;
    g_pending_n = 0; g_copen_ret = 0; g_framelog_n = 0;
    g_pcm_len = 0; g_buffers_taken = 0; g_pcm_overflow = 0; g_short_buffers = 0;
    g_buflog_n = 0;
    g_cb_pending = 0; g_in_callback = 0; g_sample_rate = 0;
    g_cb_runs = 0; g_sndlog_n = 0; g_defer_cb = 0;
    g_dt_pending = 0; g_in_deferred = 0; g_dt_runs = 0;
    g_dt_proc = g_dt_parm = 0;
    g_heap_base = g_heap_end = g_heap_next = 0;
    g_mem_traps = 0;
    return 0;
}

OSP_API void osp_shutdown(void)
{
    int i;
    /* The DLL is unloaded after this, so these would leak into NVDA's heap
     * for the rest of its life -- a voice's worth per engine switch. */
    for (i = 0; i < g_res_count; i++) {
        if (g_res[i].bytes) free(g_res[i].bytes);
        g_res[i].bytes = NULL; g_res[i].len = 0; g_res[i].detached = 0;
    }
    g_res_count = 0;
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
    /* Keep the resource as it arrived. A client that modifies what it is given
     * -- MacinTalk 2 patches a voice's `ttvi` -- must still get the original
     * the next time it asks. See res_find. Registration failing for want of a
     * few kilobytes is not worth failing the load over, so a null copy simply
     * means "cannot restore this one".*/
    g_res[g_res_count].bytes = (unsigned char *)malloc((size_t)len);
    g_res[g_res_count].len = len;
    g_res[g_res_count].detached = 0;
    if (g_res[g_res_count].bytes)
        memcpy(g_res[g_res_count].bytes, data, (size_t)len);
    g_res_count++;
    return mp;
}

/* --- the Component Manager, from Python ------------------------------- */

/* Defined further down; declared here so the call below keeps its linkage. */
OSP_API int osp_call(unsigned entry, unsigned sentinel, long long max_instr);

OSP_API int osp_add_component(unsigned type, unsigned subtype, unsigned manuf,
                              unsigned entry)
{
    Component *c;
    if (g_comp_count >= MAX_COMPONENTS) return -1;
    c = &g_comp[g_comp_count];
    c->type = type; c->subtype = subtype; c->manuf = manuf; c->entry = entry;
    return g_comp_count++;
}

OSP_API unsigned osp_open_instance(int component)
{
    int i;
    if (component < 0 || component >= g_comp_count) return 0;
    if (g_inst_count >= MAX_INSTANCES) return 0;
    i = g_inst_count++;
    g_inst[i].component = component;
    g_inst[i].storage = 0;
    g_inst[i].refcon = 0;
    return INSTANCE_TOK(i);
}

OSP_API unsigned osp_instance_storage(unsigned token)
{
    int i = inst_of(token);
    return i < 0 ? 0u : g_inst[i].storage;
}

/* Play Speech Manager: build a ComponentParameters and call the component's
 * own entry point, the way the real Component Manager would.
 *
 * `args` is in **declared** order -- (self) for Open, and so on.  They are
 * written reversed, because params[0] is the last declared argument: that is
 * how the block sits on the stack at a measured `D0 = 0` call site, and the
 * glue copies it into a handler frame verbatim rather than re-ordering it.
 */
OSP_API int osp_component_call(unsigned token, int what,
                               const unsigned *args, int nargs,
                               long long max_instr, unsigned *result)
{
    unsigned cp = CP_TOPLEVEL, sp, res_slot;
    int ii = inst_of(token), i, r;

    if (ii < 0) return -2;
    if (nargs < 0 || nargs * 4 > 255) return -3;

    m68k_write_memory_8(cp + 0, 0);                      /* flags     */
    m68k_write_memory_8(cp + 1, (unsigned)(nargs * 4));  /* paramSize */
    m68k_write_memory_16(cp + 2, (unsigned)what & 0xFFFFu);
    for (i = 0; i < nargs; i++)
        m68k_write_memory_32(cp + 4u + (unsigned)i * 4u, args[nargs - 1 - i]);

    /* pascal ComponentResult main(ComponentParameters *p, Handle storage) --
     * left to right, so params goes deeper than storage. */
    sp = m68k_get_reg(NULL, M68K_REG_SP);
    sp -= 4; res_slot = sp; m68k_write_memory_32(res_slot, 0);
    sp -= 4; m68k_write_memory_32(sp, cp);
    sp -= 4; m68k_write_memory_32(sp, g_inst[ii].storage);
    m68k_set_reg(M68K_REG_SP, sp);

    r = osp_call(g_comp[g_inst[ii].component].entry, MAGIC_SENTINEL, max_instr);
    if (result) *result = m68k_read_memory_32(res_slot);
    return r;
}

OSP_API int osp_cm_log_n(void) { return g_cmlog_n; }
OSP_API int osp_cm_log_get(int i, unsigned *d0, unsigned *pc, unsigned *csp,
                           unsigned *words, int *served)
{
    CmRec *r;
    if (i < 0 || i >= g_cmlog_n) return 0;
    r = &g_cmlog[i];
    *d0 = r->d0; *pc = r->pc; *csp = r->csp; *served = r->served;
    words[0] = r->w0; words[1] = r->w1; words[2] = r->w2; words[3] = r->w3;
    return 1;
}
OSP_API int osp_cp_wraps(void) { return g_cp_wraps; }
/* Tell the Speech Manager -- us -- that a voice exists, and which `ttvd` id it
 * lives under.  Both halves come from the voice's own ttvd. */
OSP_API int osp_add_voice(unsigned creator, unsigned id, int res_id)
{
    if (g_voice_count >= MAX_VOICES) return -1;
    g_voices[g_voice_count].creator = creator;
    g_voices[g_voice_count].id = id;
    g_voices[g_voice_count].res_id = (short)res_id;
    return g_voice_count++;
}

OSP_API int osp_reslog_n(void) { return g_reslog_n; }
OSP_API int osp_reslog_get(int i, unsigned *type, int *id, int *found)
{
    if (i < 0 || i >= g_reslog_n) return 0;
    *type = g_reslog[i].type; *id = g_reslog[i].id;
    *found = g_reslog[i].found;
    return 1;
}

OSP_API int osp_framelog_n(void) { return g_framelog_n; }
OSP_API unsigned osp_framelog_get(int i, int k)
{
    return (i >= 0 && i < g_framelog_n && k >= 0 && k < 6)
        ? g_framelog[i][k] : 0u;
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
        if (g_cb_pending && !g_defer_cb && g_stop_reason == STOP_RUNNING)
            run_pending_callback();
        if (g_dt_pending && g_stop_reason == STOP_RUNNING)
            run_pending_deferred();
        if (g_copen_ret && g_stop_reason == STOP_RUNNING)
            finish_open_component();
    }

    return g_stop_reason;
}

/* Call a routine that takes Pascal arguments already pushed by the caller.
 * MACSTARTSOUND is reached this way, through the export table at driver+$001E. */
/* Continue after a snapshot breakpoint, leaving PC, SP and every register
 * exactly as the break left them.  Needed to test what NVDA does constantly:
 * stop an utterance half way through and then start another one. */
/* Keep answering the sound callback until the engine stops asking.
 *
 * MacinTalk 2's SpeakBuffer is asynchronous: it renders one buffer, queues it
 * behind a callBackCmd and returns, and everything after that is driven by the
 * Sound Manager calling back.  `.sp` never needed this because its Prime
 * rendered a whole utterance synchronously -- here the host has to keep being
 * the Sound Manager after the speak call has returned.
 *
 * Returns how many callbacks ran.  Hitting `max_rounds` is a stall, not a
 * finish, and the caller should say so rather than treat the audio as
 * complete. */
OSP_API int osp_run_callbacks(int max_rounds, long long max_instr)
{
    int n = 0;
    while ((g_cb_pending || g_dt_pending) && n < max_rounds) {
        g_stop_reason = STOP_RUNNING;
        g_stop_vector = -1;
        g_instr_count = 0;
        g_instr_budget = max_instr;
        /* The callback installs the deferred task, and the deferred task is
         * what renders, so both have to be drained or the chain stops half
         * way with a buffer of silence already queued. */
        if (g_cb_pending) run_pending_callback();
        else              run_pending_deferred();
        n++;
        if (g_stop_reason != STOP_RUNNING) break;
    }
    return n;
}

OSP_API int osp_resume(long long max_instr)
{
    if (g_stop_reason != STOP_BREAK) return g_stop_reason;
    g_stop_reason = STOP_RUNNING;
    g_instr_budget = max_instr;
    g_instr_count = 0;
    while (g_stop_reason == STOP_RUNNING) {
        m68k_execute(100000);
        if (g_cb_pending && g_stop_reason == STOP_RUNNING)
            run_pending_callback();
    }
    return g_stop_reason;
}

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

OSP_API void osp_defer_callbacks(int on) { g_defer_cb = on ? 1 : 0; }
OSP_API int osp_cb_runs(void) { return g_cb_runs; }
OSP_API int osp_dt_runs(void) { return g_dt_runs; }
OSP_API int osp_sndlog_n(void) { return g_sndlog_n; }
OSP_API int osp_sndlog_get(int i)
{
    return (i >= 0 && i < g_sndlog_n) ? (int)g_sndlog[i] : -1;
}

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

OSP_API unsigned osp_trap_d0in(int i)
{
    return (i >= 0 && i < g_trap_count) ? g_traps[i].d0_in : 0u;
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
