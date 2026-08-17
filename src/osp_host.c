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
#include <math.h>
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
/* What Gestalt('proc') answers, and it must agree with the CPU Musashi is
 * actually running: 1 = 68000, 2 = 68010, 3 = 68020, 4 = 68030.  See
 * osp_set_cpu and the _Gestalt case. */
static int            g_gestalt_proc = 1;
/* The last error a component reported through SetComponentInstanceError.
 * Only diagnosis; it is what the engine was about to return anyway. */
static short          g_instance_error;
/* Bytes the CPU pushes for an exception.  The 68000 pushes SR and PC and
 * nothing else; **the 68010 and up add a format/vector word**, so every frame
 * is 8 bytes and every `rte` pops 8.  Getting this wrong is not subtle and is
 * not immediate: the trap is served, the `rte` returns two bytes off, and the
 * engine runs millions of instructions of nonsense before dying somewhere
 * unrelated.  Set by osp_set_cpu; see tb_return and enter_callee. */
static unsigned       g_exc_frame = 6u;
/* Format 0, vector 10 (line 1010) at offset 40.  Only used when we build a
 * frame ourselves, where the value is never inspected -- but the format nibble
 * decides how many bytes `rte` pops, so it has to be format 0. */
#define EXC_FORMAT_ALINE 0x0028u

/* Exception vectors are pointed at this page, one slot of 8 bytes each, so a
 * stop can name the vector that caused it.  Vector 10 is the A-line trap and
 * is the only one we expect; it holds a real `rte`, the rest hold nothing
 * because we stop before executing them. */
#define MAGIC_EXC_BASE  0x00F00000u
#define MAGIC_EXC_SLOT  8u
#define MAGIC_SENTINEL  0x00F10000u

/* The one Memory Manager zone.  Our heap is a single flat bump allocator, so
 * there genuinely is only one -- and saying so consistently is what MacinTalk
 * Pro checks for.  It allocates its instance storage, asks _HandleZone which
 * zone that handle is in and _GetZone which zone is current, and if the two
 * disagree it disposes the storage and returns synthOpenFailed (-241) without
 * having read a single resource.  Stubbing those two traps returned garbage,
 * and garbage is not equal to garbage.
 *
 * Sits above the trap-address page and the component tables; see the map by
 * TRAPADDR_BASE.  Filled in by osp_heap_init, because the fields worth
 * answering honestly are the heap's own limits. */
#define MAGIC_ZONE      0x00F2D000u

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
/* TopMapHndl: a Handle to the resource MAP of the most recently opened
 * resource file. **MacinTalk Pro reads it directly** -- `gtse 1 +$59D6` is
 * `move.l $a50.w,(a0)` -- and then does its own resource lookups against
 * that map: it adds RsrcMapEntry's offset to it, pulls the 24-bit data
 * offset out of the reference entry, adds the data-area base from the
 * map's own header copy, and reads the bytes straight out of the file.
 *
 * Left at zero it read address 0, which holds the initial SSP, and every
 * offset after that was nonsense -- the symptom being a voice config full
 * of fragments of other resources' names. */
/* Defined with the heap further down; the one-file File Manager below
 * materialises a resource map and so needs both before then. */
static unsigned heap_alloc(unsigned size);
static void note_handle(unsigned mp, unsigned size);

#define TOP_MAP_HNDL  0x0A50u
#define CUR_MAP_ADDR  0x0A5Au     /* CurMap, the current file's refNum */

/* ------------------------------------------------ one file, and only one -
 *
 * MacinTalk Pro's Open goes looking for the file it is running from.  It calls
 * OpenComponentResFile for a refNum, PBGetFCBInfo to turn that into a volume,
 * a directory and a name, and FSMakeFSSpec to pack the three into an FSSpec it
 * keeps at +$1C of its globals -- see gtse 1 +$6E8 and +$F42.  A non-zero
 * result from any of it is fatal to Open.
 *
 * It wants that FSSpec because its 572,928-byte lexicon lives in the file's
 * DATA fork, which no resource type covers and the Resource Manager cannot
 * reach.
 *
 * So the host has to be a File Manager.  Only just, though: there is exactly
 * one file, it is the one the engine already believes it is running from, and
 * nothing is ever written.  No volume, no directory, no second file, no
 * catalogue -- the same posture as the resource registry, which is one flat
 * table and no files at all.
 */
#define FAKE_VREFNUM  0xFFFFu     /* -1: the default volume, on every Mac    */
#define FAKE_DIRID    2u          /* the root directory id on any HFS volume */
/* refNum 1 is taken: OpenComponentResFile hands it out for "the flat resource
 * table", which is what the older engines and Pro's own Get1NamedResource use.
 * Real opens are numbered from here so the two can never be confused. */
#define FIRST_REFNUM  2

#define MAX_FILES     4
#define MAX_OPEN      8
#define FORK_DATA     0
#define FORK_RSRC     1

/* A file is its name and its two forks.  **Both matter, and they are not
 * interchangeable**: MacinTalk Pro's lexicon is in its own DATA fork, while a
 * voice's 800 KB of units is in that voice file's RESOURCE fork, which Pro
 * reads by walking the map and seeking -- so serving one where the other was
 * asked for is silent corruption. */
typedef struct {
    unsigned char name[64];              /* Str63, as the Mac holds it */
    unsigned char *fork[2];              /* [FORK_DATA], [FORK_RSRC]   */
    int            len[2];
    unsigned       map;                  /* Handle to the map, or 0    */
} FileEntry;
static FileEntry g_files[MAX_FILES];
static int       g_file_count;

typedef struct { int used, file, fork, pos; } OpenFile;
static OpenFile  g_open[MAX_OPEN];       /* refNum = index + FIRST_REFNUM */

/* -> index of the file with this Str63 name, or -1.
 *
 * Case-insensitive, like the file system it stands in for.  A nil or empty
 * name means "the first file", which is the engine's own -- the caller that
 * has not said which file it wants is the one asking about itself. */
/* The last name any open was asked for, so a miss can be reported rather than
 * guessed at.  Diagnosis only. */
static char g_last_file_req[80];

static int file_by_name(unsigned ptr)
{
    int i, j, n;
    if (!ptr) { strcpy(g_last_file_req, "(nil)"); return g_file_count ? 0 : -1; }
    n = (int)m68k_read_memory_8(ptr);
    if (n <= 0) { strcpy(g_last_file_req, "(empty)"); return g_file_count ? 0 : -1; }
    if (n > 63) n = 63;
    for (i = 0; i < n; i++)
        g_last_file_req[i] = (char)m68k_read_memory_8(ptr + 1u + (unsigned)i);
    g_last_file_req[n] = 0;
    for (i = 0; i < g_file_count; i++) {
        if (g_files[i].name[0] != (unsigned char)n) continue;
        for (j = 1; j <= n; j++) {
            int x = g_files[i].name[j];
            int y = (int)m68k_read_memory_8(ptr + (unsigned)j);
            if (x >= 'a' && x <= 'z') x -= 32;
            if (y >= 'a' && y <= 'z') y -= 32;
            if (x != y) break;
        }
        if (j > n) return i;
    }
    return -1;
}

/* Write a file's Str63 name wherever a caller asked for it. */
static void file_put_name(int idx, unsigned ptr)
{
    int i, n;
    if (!ptr || idx < 0 || idx >= g_file_count) return;
    n = g_files[idx].name[0];
    for (i = 0; i <= n; i++)
        m68k_write_memory_8(ptr + (unsigned)i, g_files[idx].name[i]);
}

/* Put a copy of this file's resource MAP into emulated memory and answer
 * with a Handle to it, caching per file.
 *
 * A resource fork begins with four longs -- data offset, map offset, data
 * length, map length -- and the map begins with a copy of that same header,
 * which is why the engine can read the data-area base out of the map alone.
 * We hand back the real bytes, so every offset the engine computes against
 * them lands where it should in the fork we are also serving through _Read.
 *
 * -> 0 if this file has no resource fork, or the heap is full. */
static unsigned file_map_handle(int idx)
{
    FileEntry *f;
    const unsigned char *fk;
    unsigned mOff, mLen, blk, mp;
    if (idx < 0 || idx >= g_file_count) return 0;
    f = &g_files[idx];
    if (f->map) return f->map;
    fk = f->fork[FORK_RSRC];
    if (!fk || f->len[FORK_RSRC] < 16) return 0;
    mOff = ((unsigned)fk[4] << 24) | ((unsigned)fk[5] << 16)
         | ((unsigned)fk[6] << 8)  |  (unsigned)fk[7];
    mLen = ((unsigned)fk[12] << 24) | ((unsigned)fk[13] << 16)
         | ((unsigned)fk[14] << 8)  |  (unsigned)fk[15];
    if (!mLen || mOff + mLen > (unsigned)f->len[FORK_RSRC]) return 0;
    blk = heap_alloc(mLen);
    if (!blk) return 0;
    memcpy(g_ram + blk, fk + mOff, (size_t)mLen);
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    note_handle(mp, mLen);
    f->map = mp;
    return mp;
}

/* -> refNum, or 0 if there is no room or no such file. */
static int file_open(int idx, int fork)
{
    int i;
    if (idx < 0 || idx >= g_file_count) return 0;
    for (i = 0; i < MAX_OPEN; i++)
        if (!g_open[i].used) {
            g_open[i].used = 1;
            g_open[i].file = idx;
            g_open[i].fork = fork;
            g_open[i].pos = 0;
            return i + FIRST_REFNUM;
        }
    return 0;
}

/* True when no file has been registered at all.
 *
 * `.sp` and MacinTalk 2 open and close a resource file during voice
 * loading and never read a byte of it -- for them the call is a formality
 * and any positive refNum has always done. Only MacinTalk Pro supplies
 * files, so failing an open for want of one broke both older engines the
 * moment the File Manager became real: every voice went silent, which the
 * tests caught before it reached anybody. */
static int no_files(void) { return g_file_count == 0; }

static OpenFile *file_by_ref(int refnum)
{
    int i = refnum - FIRST_REFNUM;
    if (i < 0 || i >= MAX_OPEN || !g_open[i].used) return NULL;
    return &g_open[i];
}

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
/* How big each Handle is.
 *
 * The bump allocator never needed to know -- nothing asked -- until MacinTalk
 * Pro called _GetHandleSize fifteen times in one utterance. A stubbed answer
 * is not a small error here: the engine sizes buffers from it, and a stub does
 * not write the caller's reserved long at all, so it proceeds on whatever was
 * on the stack. */
#define MAX_HANDLES 2048
typedef struct { unsigned mp, size; } HandleRec;
static HandleRec g_handles[MAX_HANDLES];
static int       g_handle_count;

static void note_handle(unsigned mp, unsigned size)
{
    if (!mp || g_handle_count >= MAX_HANDLES) return;
    g_handles[g_handle_count].mp = mp;
    g_handles[g_handle_count].size = size;
    g_handle_count++;
}

/* -> the size, or 0 for a Handle we did not make. */
static unsigned handle_size(unsigned mp)
{
    int i;
    for (i = g_handle_count - 1; i >= 0; i--)   /* newest first */
        if (g_handles[i].mp == mp) return g_handles[i].size;
    return 0;
}

static unsigned heap_new_handle(unsigned size)
{
    unsigned blk = heap_alloc(size);
    unsigned mp;
    if (!blk) return 0;
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    note_handle(mp, size);
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


/* ------------------------------------------- Fixed point and extended 80 -
 *
 * MacinTalk Pro does its synthesis arithmetic in Apple's numeric types, and
 * calls the Toolbox to do it: FixMul and FixDiv by the hundred, and
 * conversions to and from the 80-bit extended format SANE works in.
 *
 * `Fixed` is signed 16.16, `Fract` is signed 2.30.  An extended is ten bytes:
 * a sign bit, fifteen exponent bits biased by 16383, and a 64-bit mantissa
 * **with an explicit integer bit** -- unlike IEEE double, nothing is implied.
 * We already write this layout into AIFF sample rates; this reads it too.
 */
static double ext80_read(unsigned addr)
{
    unsigned hi = m68k_read_memory_16(addr);
    unsigned m0 = m68k_read_memory_32(addr + 2);
    unsigned m1 = m68k_read_memory_32(addr + 6);
    double mant = (double)m0 * 4294967296.0 + (double)m1;
    int exp = (int)(hi & 0x7FFFu);
    double v;
    if (exp == 0 && mant == 0.0) return 0.0;
    v = ldexp(mant, exp - 16383 - 63);
    return (hi & 0x8000u) ? -v : v;
}

static void ext80_write(unsigned addr, double v)
{
    int sign = 0, e2;
    double m;
    unsigned hi, m0, m1;
    unsigned long long mant;
    if (v < 0.0) { sign = 1; v = -v; }
    if (!(v > 0.0)) {                        /* zero, and NaN lands here too */
        m68k_write_memory_16(addr, (unsigned)(sign << 15));
        m68k_write_memory_32(addr + 2, 0);
        m68k_write_memory_32(addr + 6, 0);
        return;
    }
    m = frexp(v, &e2);                       /* v = m * 2^e2, 0.5 <= m < 1   */
    mant = (unsigned long long)ldexp(m, 64); /* integer bit lands in bit 63  */
    hi = (unsigned)((sign << 15) | ((unsigned)(e2 - 1 + 16383) & 0x7FFFu));
    m0 = (unsigned)(mant >> 32);
    m1 = (unsigned)(mant & 0xFFFFFFFFu);
    m68k_write_memory_16(addr, hi);
    m68k_write_memory_32(addr + 2, m0);
    m68k_write_memory_32(addr + 6, m1);
}

static int fx_signed(unsigned v)
{
    return (v & 0x80000000u) ? (int)(v - 0x100000000ull) : (int)v;
}

/* Round-to-nearest, which is what the Toolbox does. */
static unsigned fx_mul(unsigned a, unsigned b)
{
    long long r = ((long long)fx_signed(a) * (long long)fx_signed(b));
    return (unsigned)((r + 0x8000) >> 16);
}

static unsigned fx_div(unsigned a, unsigned b)
{
    long long num = ((long long)fx_signed(a)) << 16;
    int den = fx_signed(b);
    if (den == 0) return (fx_signed(a) < 0) ? 0x80000000u : 0x7FFFFFFFu;
    return (unsigned)(num / den);
}

static void tb_return(unsigned exc_sp, unsigned param_bytes,
                      unsigned result, int result_size);

static unsigned short g_sane_env;

static double sane_read_float(unsigned addr, unsigned fmt)
{
    if (fmt == 0x0000u) {
        return ext80_read(addr);
    } else if (fmt == 0x0800u) {
        unsigned long long bits = ((unsigned long long)m68k_read_memory_32(addr) << 32)
                                | (unsigned long long)m68k_read_memory_32(addr + 4);
        double v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    } else if (fmt == 0x1000u) {
        unsigned bits = m68k_read_memory_32(addr);
        float v;
        memcpy(&v, &bits, sizeof(v));
        return (double)v;
    } else if (fmt == 0x2000u) {
        return (double)(short)m68k_read_memory_16(addr);
    } else if (fmt == 0x2800u) {
        return (double)fx_signed(m68k_read_memory_32(addr));
    } else if (fmt == 0x3000u) {
        unsigned long long bits = ((unsigned long long)m68k_read_memory_32(addr) << 32)
                                | (unsigned long long)m68k_read_memory_32(addr + 4);
        long long signed_bits = (long long)bits;
        return (double)signed_bits;
    }
    return 0.0;
}

static void sane_write_float(unsigned addr, unsigned fmt, double v)
{
    if (fmt == 0x0000u) {
        /* MSVC long double is just IEEE double, so this SANE subset trades the
         * last 11 mantissa bits of extended precision for a working synthesis
         * engine.  The audio path does not need bit-perfect 80-bit arithmetic. */
        ext80_write(addr, v);
    } else if (fmt == 0x0800u) {
        unsigned long long bits;
        memcpy(&bits, &v, sizeof(bits));
        m68k_write_memory_32(addr, (unsigned)(bits >> 32));
        m68k_write_memory_32(addr + 4, (unsigned)bits);
    } else if (fmt == 0x1000u) {
        float f = (float)v;
        unsigned bits;
        memcpy(&bits, &f, sizeof(bits));
        m68k_write_memory_32(addr, bits);
    } else if (fmt == 0x2000u) {
        long r = (long)(v < 0.0 ? v - 0.5 : v + 0.5);
        if (r > 32767L) r = 32767L;
        else if (r < -32768L) r = -32768L;
        m68k_write_memory_16(addr, (unsigned)(unsigned short)r);
    } else if (fmt == 0x2800u) {
        double r = v < 0.0 ? v - 0.5 : v + 0.5;
        if (r > 2147483647.0) r = 2147483647.0;
        else if (r < -2147483648.0) r = -2147483648.0;
        m68k_write_memory_32(addr, (unsigned)(long)r);
    } else if (fmt == 0x3000u) {
        double r = v < 0.0 ? v - 0.5 : v + 0.5;
        unsigned long long bits = (unsigned long long)(long long)r;
        m68k_write_memory_32(addr, (unsigned)(bits >> 32));
        m68k_write_memory_32(addr + 4, (unsigned)bits);
    }
}

static void sane_set_relation(unsigned exc_sp, double dst, double src)
{
    unsigned sr = m68k_read_memory_16(exc_sp);
    sr &= ~0x001Fu;                         /* X N Z V C */
    if (dst < src) sr |= 0x08u | 0x01u;     /* N and C */
    else if (dst == src) sr |= 0x04u;       /* Z */
    m68k_write_memory_16(exc_sp, sr);
}

static int sane_fail(unsigned short trap, unsigned short opword, unsigned pc)
{
    fprintf(stderr, "unimplemented SANE trap $%04X opword $%04X at 0x%X\n",
            trap, opword, pc);
    g_stop_reason = STOP_EXCEPTION;
    g_stop_vector = 10;
    g_stop_pc = pc;
    m68k_end_timeslice();
    return 1;
}

static int sane_return(unsigned exc_sp, unsigned param_bytes)
{
    tb_return(exc_sp, param_bytes, 0, 0);
    return 1;
}

static int serve_sane_trap(unsigned short trap, unsigned exc_sp,
                           unsigned csp, unsigned resume_pc)
{
    unsigned short opword = (unsigned short)m68k_read_memory_16(csp);
    unsigned fmt = (unsigned)(opword & 0x7800u);
    unsigned op = (unsigned)(opword & 0x07FFu);
    unsigned dst = m68k_read_memory_32(csp + 2);
    unsigned src = m68k_read_memory_32(csp + 6);
    double d, s, r;

    if (trap == 0xA9ECu) {                  /* Pack5 / Elems68K */
        if (opword == 0x8010u || opword == 0x8012u) {
            d = ext80_read(dst);
            s = ext80_read(src);
            ext80_write(dst, pow(d, s));
            return sane_return(exc_sp, 10);
        }
        if (fmt != 0x0000u) return sane_fail(trap, opword, resume_pc - 2u);
        d = ext80_read(dst);
        switch (op) {
        case 0x0000u: r = log(d); break;           /* FLNX    */
        case 0x0002u: r = log(d) / log(2.0); break;/* FLOG2X  */
        case 0x0004u: r = log1p(d); break;         /* FLN1X   */
        case 0x0006u: r = log1p(d) / log(2.0); break;
        case 0x0008u: r = exp(d); break;           /* FEXPX   */
        case 0x000Au: r = pow(2.0, d); break;      /* FEXP2X  */
        case 0x000Cu: r = exp(d) - 1.0; break;     /* FEXP1X  */
        case 0x000Eu: r = pow(2.0, d) - 1.0; break;
        case 0x0018u: r = sin(d); break;
        case 0x001Au: r = cos(d); break;
        case 0x001Cu: r = tan(d); break;
        case 0x001Eu: r = atan(d); break;
        default: return sane_fail(trap, opword, resume_pc - 2u);
        }
        ext80_write(dst, r);
        return sane_return(exc_sp, 6);
    }

    if (trap != 0xA9EBu) return 0;

    switch (op) {
    case 0x0000u: case 0x0002u: case 0x0004u: case 0x0006u:
    case 0x000Cu: case 0x0018u: {
        d = ext80_read(dst);
        s = sane_read_float(src, fmt);
        if (op == 0x0000u) r = d + s;
        else if (op == 0x0002u) r = d - s;
        else if (op == 0x0004u) r = d * s;
        else if (op == 0x0006u) r = d / s;
        else if (op == 0x000Cu) r = fmod(d, s);
        else r = ldexp(d, (int)s);
        ext80_write(dst, r);
        return sane_return(exc_sp, 10);
    }
    case 0x0008u: case 0x000Au:
        sane_set_relation(exc_sp, ext80_read(dst), sane_read_float(src, fmt));
        return sane_return(exc_sp, 10);
    case 0x000Eu:
        ext80_write(dst, sane_read_float(src, fmt));
        return sane_return(exc_sp, 10);
    case 0x0010u:
        sane_write_float(dst, fmt, ext80_read(src));
        return sane_return(exc_sp, 10);
    case 0x0012u:
        ext80_write(dst, sqrt(ext80_read(dst)));
        return sane_return(exc_sp, 6);
    case 0x0014u:
        ext80_write(dst, floor(ext80_read(dst) + 0.5));
        return sane_return(exc_sp, 6);
    case 0x0016u:
        d = ext80_read(dst);
        ext80_write(dst, d < 0.0 ? ceil(d) : floor(d));
        return sane_return(exc_sp, 6);
    case 0x001Au:
        ext80_write(dst, floor(log(fabs(ext80_read(dst))) / log(2.0)));
        return sane_return(exc_sp, 6);
    case 0x001Cu:
        m68k_write_memory_16(dst, ext80_read(src) == 0.0 ? 4u : 5u);
        return sane_return(exc_sp, 10);
    case 0x000Du:
        ext80_write(dst, -ext80_read(dst));
        return sane_return(exc_sp, 6);
    case 0x000Fu:
        ext80_write(dst, fabs(ext80_read(dst)));
        return sane_return(exc_sp, 6);
    case 0x0001u:                         /* FSETENV */
        g_sane_env = m68k_read_memory_16(dst);
        return sane_return(exc_sp, 6);
    case 0x0003u:                         /* FGETENV */
        m68k_write_memory_16(dst, g_sane_env);
        return sane_return(exc_sp, 6);
    case 0x0017u:                         /* FPROCENTRY */
        m68k_write_memory_16(dst, g_sane_env);
        g_sane_env = 0;
        return sane_return(exc_sp, 6);
    case 0x0019u:                         /* FPROCEXIT */
        g_sane_env = m68k_read_memory_16(dst);
        return sane_return(exc_sp, 6);
    default:
        return sane_fail(trap, opword, resume_pc - 2u);
    }
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
    /* Every one of these answers with the single zone.  A client that wants
     * to know where a block lives, or which zone is current, or where the
     * system and application zones are, gets the same truthful answer: there
     * is one heap here.  MacinTalk Pro's Open compares two of them. */
    case 0xA11A:                       /* _GetZone         -> THz in A0     */
    case 0xA126:                       /* _HandleZone(h)   -> THz in A0     */
    case 0xA148:                       /* _PtrZone(p)      -> THz in A0     */
    case 0xA02C:                       /* _ApplicationZone -> THz in A0     */
        *a0_out = MAGIC_ZONE; *d0_out = 0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA01B:                       /* _SetZone(THz) -- there is only one */
        *a0_out = a0; *d0_out = 0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    /* ---- the File Manager, for one file ------------------------------- *
     *
     * All of these take a ParamBlockRec in A0 and return the error in D0, and
     * the HFS spellings are the flat ones with bit 9 set: $A20A is _HOpenRF,
     * $A214 is _HGetVol, $A215 is _HSetVol.  `base` has already masked that
     * off, so one case serves both.
     *
     * IOParam offsets used below: ioResult 16, ioNamePtr 18, ioVRefNum 22,
     * ioRefNum 24, ioMisc 28, ioBuffer 32, ioReqCount 36, ioActCount 40,
     * ioPosMode 44, ioPosOffset 46. */
    case 0xA000:                       /* _Open   -- the DATA fork          */
    case 0xA00A: {                     /* _OpenRF -- the RESOURCE fork      */
        /* Which fork is the whole point. Pro's lexicon is in its own data
         * fork; a voice's 800 KB of units is in that voice file's resource
         * fork, which it reads by walking the map and seeking. Serving one
         * where the other was asked for is silent corruption, not an error. */
        int idx, ref;
        if (no_files()) {                                  /* nominal open */
            m68k_write_memory_16(a0 + 24, (unsigned)FIRST_REFNUM);
            m68k_write_memory_16(a0 + 16, 0);
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        idx = file_by_name(m68k_read_memory_32(a0 + 18));
        ref = file_open(idx, base == 0xA00Au ? FORK_RSRC : FORK_DATA);
        if (!ref) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);        /* fnfErr, -43 */
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        m68k_write_memory_16(a0 + 24, (unsigned)ref);      /* ioRefNum    */
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA001: {                     /* _Close                            */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        if (f) f->used = 0;
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA002: {                     /* _Read                             */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        unsigned buf = m68k_read_memory_32(a0 + 32);
        int req  = (int)m68k_read_memory_32(a0 + 36);
        int mode = (int)(short)m68k_read_memory_16(a0 + 44);
        int off  = (int)m68k_read_memory_32(a0 + 46);
        const unsigned char *bytes;
        int len, pos, got, i, err = 0;
        if (!f) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);        /* fnfErr, -43 */
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        bytes = g_files[f->file].fork[f->fork];
        len = g_files[f->file].len[f->fork];
        if (!bytes) {
            m68k_write_memory_16(a0 + 16, 0xFFDCu);        /* ioErr, -36  */
            *d0_out = (unsigned)(-36); *a0_out = a0;
            return 1;
        }
        switch (mode & 3) {
            case 1:  pos = off; break;                     /* fsFromStart */
            case 2:  pos = len + off; break;               /* fsFromLEOF  */
            case 3:  pos = f->pos + off; break;            /* fsFromMark  */
            default: pos = f->pos; break;                  /* fsAtMark    */
        }
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        got = req < 0 ? 0 : req;
        if (got > len - pos) { got = len - pos; err = -39; }
        for (i = 0; i < got; i++)
            m68k_write_memory_8(buf + (unsigned)i, bytes[pos + i]);
        f->pos = pos + got;
        m68k_write_memory_32(a0 + 40, (unsigned)got);      /* ioActCount  */
        m68k_write_memory_16(a0 + 16, (unsigned)(unsigned short)err);
        *d0_out = (unsigned)err;                           /* eofErr, -39 */
        *a0_out = a0;
        return 1;
    }
    case 0xA044: {                     /* _SetFPos                          */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        int mode = (int)(short)m68k_read_memory_16(a0 + 44);
        int off  = (int)m68k_read_memory_32(a0 + 46);
        int len, pos;
        if (!f) {
            m68k_write_memory_16(a0 + 16, 0xFFD8u);
            *d0_out = (unsigned)(-43); *a0_out = a0;
            return 1;
        }
        len = g_files[f->file].len[f->fork];
        switch (mode & 3) {
            case 1:  pos = off; break;
            case 2:  pos = len + off; break;
            case 3:  pos = f->pos + off; break;
            default: pos = f->pos; break;
        }
        if (pos < 0) pos = 0;
        /* Seeking past the end is posErr, and saying so matters: the caller
         * that gets noErr for a bad seek reads the wrong bytes instead. */
        if (pos > len) {
            f->pos = len;
            m68k_write_memory_16(a0 + 16, 0xFFC0u);        /* posErr, -64 */
            *d0_out = (unsigned)(-64); *a0_out = a0;
            return 1;
        }
        f->pos = pos;
        m68k_write_memory_32(a0 + 46, (unsigned)pos);
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA018: {                     /* _GetFPos                          */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        m68k_write_memory_32(a0 + 36, 0);
        m68k_write_memory_32(a0 + 46, (unsigned)(f ? f->pos : 0));
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA011: {                     /* _GetEOF -- length in ioMisc       */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(a0 + 24));
        int len = f ? g_files[f->file].len[f->fork] : 0;
        m68k_write_memory_32(a0 + 28, (unsigned)len);
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    }
    case 0xA014:                       /* _GetVol / _HGetVol                */
        file_put_name(0, m68k_read_memory_32(a0 + 18));
        m68k_write_memory_16(a0 + 22, FAKE_VREFNUM);
        m68k_write_memory_32(a0 + 48, FAKE_DIRID);         /* ioWDDirID     */
        m68k_write_memory_16(a0 + 16, 0);
    case 0xA015:                       /* _SetVol / _HSetVol -- only one    */
        m68k_write_memory_16(a0 + 16, 0);
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA060:                       /* _HFSDispatch -- selector in D0    */
        /* Selector 8 is PBGetFCBInfo, and the param block proves it rather
         * than a table doing: MacinTalk Pro fills ioCompletion at +12,
         * ioNamePtr at +18, ioVRefNum at +22, ioRefNum at +24 and ioFCBIndx
         * at +28, then reads ioFCBVRefNum back from +52 and ioFCBParID from
         * +58 -- which is FCBPBRec, field for field.
         *
         * "Which file is this open refNum?" has one answer here. */
        if (d0 == 7u) {
            /* PBGetWDInfo(WDPBRec): ioWDVRefNum at +32 and ioWDDirID at +48,
             * which is where _GetVol above already puts them.  Pro asks for
             * this between setting the volume and opening a file, and stubbed
             * it concluded the file was not there -- resFNotFound, -193. */
            m68k_write_memory_16(a0 + 16, 0);            /* ioResult        */
            file_put_name(0, m68k_read_memory_32(a0 + 18)); /* ioNamePtr    */
            m68k_write_memory_16(a0 + 32, FAKE_VREFNUM); /* ioWDVRefNum     */
            m68k_write_memory_32(a0 + 48, FAKE_DIRID);   /* ioWDDirID       */
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        if (d0 == 8u) {
            m68k_write_memory_16(a0 + 16, 0);            /* ioResult        */
            /* Whichever file that refNum has open, or the engine's own
             * when the refNum is the flat table's. */
            {
                OpenFile *f = file_by_ref(
                    (int)(short)m68k_read_memory_16(a0 + 24));
                int idx = f ? f->file : 0;
                int len = f ? g_files[idx].len[f->fork]
                            : (g_file_count ? g_files[0].len[FORK_DATA] : 0);
                file_put_name(idx, m68k_read_memory_32(a0 + 18));
                m68k_write_memory_32(a0 + 40, (unsigned)len); /* ioFCBEOF  */
                m68k_write_memory_32(a0 + 44, (unsigned)len); /* ioFCBPLen */
            }
            m68k_write_memory_16(a0 + 52, FAKE_VREFNUM); /* ioFCBVRefNum    */
            m68k_write_memory_32(a0 + 58, FAKE_DIRID);   /* ioFCBParID      */
            *d0_out = 0; *a0_out = a0;
            return 1;
        }
        return 0;                      /* anything else is news; let it stub */
    case 0xA198:                       /* thin wrapper, result in D0        */
        /* gtse 1 +$82CE is `movea.l (a7)+,a1 / moveq #1,d0 / A198 / jmp (a1)`
         * -- no stack arguments, the result is D0, and it sits among the file
         * wrappers at +$82xx, called from the module loader at +$5E42 and
         * +$6286.  Stubbed it left D0 as the 1 that was moved in, and no Mac
         * OSErr is ever 1, so the loader read that as a failure.
         *
         * Answered as noErr, which is provisional: the trap is not identified
         * from a call site the way the rest here are, only its shape and its
         * neighbours.  If MacinTalk Pro ever behaves oddly around loading a
         * module, doubt this first. */
        *d0_out = 0; *a0_out = a0;
        return 1;
    case 0xA01C:                       /* _FreeMem -> free bytes in D0      */
        /* One contiguous free block, so free and largest-free are the same
         * number here. gtse 1 +$2E92 asks before deciding how much to keep. */
        *d0_out = (g_heap_end > g_heap_next) ? (g_heap_end - g_heap_next) : 0;
        *a0_out = a0;
        return 1;
    case 0xA025:                       /* _GetHandleSize(h) -> long in D0   */
        /* Answerable exactly now that every Handle records its size. Pro asks
         * fifteen times per utterance and sizes buffers from the answer. */
        *d0_out = handle_size(a0);
        *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA069:                       /* _HGetState(h) -> flags in D0      */
        /* Nothing here is locked, purgeable or a resource in the Memory
         * Manager's sense -- our blocks never move -- so the honest answer is
         * no flags set. `$A06A` used to be served as _SystemZone, which was
         * wrong: it is _HSetState, the other half of this pair, and MacinTalk
         * Pro calls them around anything it wants to hold still. */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA06A:                       /* _HSetState(h, flags)              */
        *d0_out = 0; *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA04C:                       /* _CompactMem(size) -> largest free */
        /* A bump allocator has one free block and it is contiguous, so the
         * largest it could give out is everything left. */
        *d0_out = (g_heap_end > g_heap_next) ? (g_heap_end - g_heap_next) : 0;
        *a0_out = a0;
        m68k_write_memory_16(MEM_ERR_ADDR, 0);
        return 1;
    case 0xA1AD:                       /* _Gestalt -- selector D0, resp A0 */
        /* Reached through the TrapAvailable() pattern at Cecy 1 +$58A4, and
         * the engine carries its own answer table for the case where Gestalt
         * is missing.  We report it present -- the selector it asks for is in
         * the trap log's D0-in column, and the answer below is chosen from
         * that rather than from what a Mac would have said.
         *
         * Zero for almost everything, which reads as "the feature is there and
         * it is off", and is what MacinTalk 2 has always been told.  Exactly
         * two selectors are answered, because exactly two are gates:
         *
         * MacinTalk Pro's Open builds a requirements record at gtse 1 +$1044
         * and tests it at +$282.  `sysv` below $0700 fails, and `proc` equal
         * to 1 or 2 -- a 68000 or a 68010 -- fails.  Either way it writes
         * #$ff0f into the error and disposes its own storage:
         * **synthOpenFailed, -241, before it reads a single resource.**
         *
         * So Pro genuinely requires a 68020. `proc` reports whatever CPU is
         * actually configured rather than a flattering constant -- claiming an
         * 020 while running the 68000 core would invite 020-only instructions
         * into an emulator that cannot execute them. */
        if (d0 == 0x73797376u)         /* 'sysv' */
            *a0_out = 0x0700u;         /* System 7, as _SysEnvirons says too */
        else if (d0 == 0x70726F63u)    /* 'proc' */
            *a0_out = g_gestalt_proc;
        else if (d0 == 0x66707520u)    /* 'fpu ' */
            /* 3 is the 68040's built-in FPU. The modules MacinTalk Pro loads
             * for synthesis contain F-line coprocessor instructions, so this
             * has to agree with the CPU the same way 'proc' does -- and on a
             * 68040 the answer is yes. */
            *a0_out = (g_gestalt_proc >= 5) ? 3u : 0u;
        else
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
/* Six on a 68000, eight on everything later. See g_exc_frame. */
#define EXC_FRAME g_exc_frame

static void tb_return(unsigned exc_sp, unsigned param_bytes,
                      unsigned result, int result_size)
{
    unsigned caller_sp = exc_sp + EXC_FRAME;
    unsigned result_slot = caller_sp + param_bytes;
    unsigned new_sp, sr, pc, fmt = 0;

    if (result_size == 4)      m68k_write_memory_32(result_slot, result);
    else if (result_size == 2) m68k_write_memory_16(result_slot, result);
    else                       result_slot = caller_sp + param_bytes;

    new_sp = result_slot - EXC_FRAME;
    sr = m68k_read_memory_16(exc_sp);          /* read them all before any  */
    pc = m68k_read_memory_32(exc_sp + 2);      /* write, the ranges overlap */
    if (EXC_FRAME > 6u) fmt = m68k_read_memory_16(exc_sp + 6);
    m68k_write_memory_16(new_sp, sr);
    m68k_write_memory_32(new_sp + 2, pc);
    /* The format word moves with the frame. `rte` reads it to decide how much
     * to pop, so leaving the old one behind desynchronises the stack. */
    if (EXC_FRAME > 6u) m68k_write_memory_16(new_sp + 6, fmt);
    m68k_set_reg(M68K_REG_SP, new_sp);
}

/* ------------------------------------------------------ resource manager - */

#define MAX_RES 64
/* `bytes`/`len` are the resource exactly as it came off the disk image, kept
 * host-side so a detached resource can be handed back unmodified.  See
 * res_find and _DetachResource below -- this is not belt and braces, it is
 * the fix for MacinTalk 2's voice switching. */
/* `name` is the Mac resource name, which MacinTalk Pro looks resources up BY:
 * its own code is `gtse 1` called `*TTS`, and Bruce's 789 KB unit database is
 * `gtss 3` called `EnglMBruceData`.  Empty for the engines that only ever ask
 * by id. */
/* `map_entry` is the offset from the start of the resource MAP to this
 * resource's reference entry, which is what RsrcMapEntry answers.  Zero
 * for the engines that never ask -- only MacinTalk Pro does, because it
 * reads its 800 KB unit database out of the file rather than loading it. */
typedef struct { unsigned type; short id; unsigned handle;
                 unsigned char *bytes; int len; int detached;
                 char name[64]; int map_entry; int file; } ResEntry;
static ResEntry g_res[MAX_RES];
static int      g_res_count;
static int      g_res_load = 1;
/* Which registered file the Resource Manager is currently searching, or
 * -1 for "the one flat table", which is all `.sp` and MacinTalk 2 have
 * ever had.  Set by _UseResFile and by opening a resource file. */
static int      g_cur_res_file = -1;
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
/* `file` is which registered file this voice lives in, or -1 for none.
 * MacinTalk 2 never needed it -- it is handed its voice's resources
 * directly -- but **MacinTalk Pro opens the voice file itself**, so the
 * FSSpec it is given has to name something real. */
typedef struct { unsigned creator, id; short res_id; int file; } VoiceReg;
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
/* -> index of a matching entry, or -1.
 *
 * **The same type and id can name different resources in different files**,
 * and MacinTalk Pro relies on it: `gtsg 0` is 1,032 bytes in the engine and
 * 110 in a voice. A flat table hands out whichever was registered first, so
 * Pro opened a voice, asked for ITS `gtsg 0`, got the engine's, and reported
 * resFNotFound from its own lookup two calls later.
 *
 * `one_file` is the Get1* family, which the Resource Manager restricts to the
 * current file; the plain family walks the whole chain but still prefers the
 * current file, which is what "most recently opened first" amounts to here.
 * An entry belonging to no file (-1) matches either way, because that is every
 * resource the older engines register. */
static int res_index(unsigned type, short id, int one_file)
{
    int i;
    if (g_cur_res_file >= 0)
        for (i = 0; i < g_res_count; i++)
            if (g_res[i].type == type && g_res[i].id == id
                    && g_res[i].file == g_cur_res_file)
                return i;
    for (i = 0; i < g_res_count; i++) {
        if (g_res[i].type != type || g_res[i].id != id) continue;
        if (one_file && g_cur_res_file >= 0 && g_res[i].file >= 0
                && g_res[i].file != g_cur_res_file)
            continue;                       /* belongs to another file */
        return i;
    }
    return -1;
}

/* Put a detached resource's original bytes back. See the long note above. */
static void res_restore(ResEntry *r)
{
    if (r->detached && r->bytes) {
        unsigned blk = m68k_read_memory_32(r->handle);
        if (blk) memcpy(g_ram + blk, r->bytes, (size_t)r->len);
        r->detached = 0;
    }
}

/* Is entry `i` inside the file being searched?  Counting and walking by
 * index have to agree with looking up by id, or Count1Resources promises
 * more than Get1IndResource can deliver -- which is how a stubbed count
 * once produced 26,245 faults. */
static int res_in_file(int i, int one_file)
{
    if (g_cur_res_file < 0) return 1;
    if (g_res[i].file == g_cur_res_file) return 1;
    return one_file ? (g_res[i].file < 0) : 1;
}

static unsigned res_find_scoped(unsigned type, short id, int one_file)
{
    int i = res_index(type, id, one_file);
    if (i < 0) return 0;
    res_restore(&g_res[i]);
    return g_res[i].handle;
}

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

/* The Resource Manager compares names without regard to case, so we do too. */
static int res_name_eq(const char *a, const unsigned char *b, int blen)
{
    int i;
    for (i = 0; i < blen; i++) {
        int x = a[i], y = b[i];
        if (!x) return 0;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return a[blen] == 0;
}

/* -> the handle of a named resource, restoring it exactly as res_find does. */
static unsigned res_find_named(unsigned type, const unsigned char *name,
                               int namelen)
{
    int i, pass;
    /* Current file first, then anywhere -- names collide across files exactly
     * as ids do. */
    for (pass = (g_cur_res_file >= 0 ? 0 : 1); pass < 2; pass++)
        for (i = 0; i < g_res_count; i++) {
            if (g_res[i].type != type) continue;
            if (pass == 0 && g_res[i].file != g_cur_res_file) continue;
            if (!res_name_eq(g_res[i].name, name, namelen)) continue;
            res_restore(&g_res[i]);
            return g_res[i].handle;
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
    /* Built by hand rather than by the CPU, so the format word has to be
     * supplied too -- `rte` pops by format, not by CPU type. */
    if (EXC_FRAME > 6u) m68k_write_memory_16(frame + 6, EXC_FORMAT_ALINE);
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

    case 0x0000000Bu: {         /* SetComponentInstanceError(self, OSErr)     */
        /* gtse 1 +$1E0 reaches this only on the way out of a FAILED selector:
         * it tests its error, checks it has storage, then pushes the instance
         * and the error as a word.  Six bytes of arguments, no result.
         *
         * Worth serving for the diagnosis alone -- this is the engine saying
         * out loud which error it is about to return, and osp_instance_error
         * hands it to the probe.  Unserved it halted, which turned "the rate
         * call failed" into "unhandled exception" and hid the reason. */
        g_instance_error = (short)m68k_read_memory_16(csp);
        tb_return(exc_sp, 6, 0, 0);
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
        /* **D0 is (selector << 16) | argument byte count**, which the
         * GetVoiceInfo case below already depends on: $0614000C is selector
         * $0614 with twelve bytes, and GetVoiceInfo does take twelve.
         *
         * Reading the low word as the selector -- as this did -- makes the
         * two indistinguishable, and MacinTalk Pro calls selector $000C with
         * eight bytes, whose low word is 8. That ran the SndChannelStatus
         * branch against a completely different argument layout, took a
         * caller-supplied long as a pointer, and wrote through 0x1C380028,
         * which is not even inside the emulator's 16 MB. It was the wild
         * write that sent Pro branching into a wave table. */
        unsigned bytes = d0 & 0xFFFFu;
        unsigned selector = d0 >> 16;

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
            int k, found = 0, vfile = -1;
            short res_id = 0;
            for (k = 0; k < g_voice_count; k++) {
                if (g_voices[k].creator == creator && g_voices[k].id == vid) {
                    res_id = g_voices[k].res_id;
                    vfile = g_voices[k].file;
                    found = 1; break;
                }
            }
            if (found) {
                /* **The name is not decoration for MacinTalk Pro.** It takes
                 * this FSSpec and calls _OpenRFPerm on it, then walks that
                 * fork's resource map. An empty name sent it to the engine's
                 * own file instead of the voice's, where the units it wanted
                 * were not -- reported as resFNotFound, -193, from Pro's own
                 * lookup rather than from any trap.
                 *
                 * A voice registered without a file keeps the nominal FSSpec
                 * MacinTalk 2 has always been given. */
                if (vfile >= 0 && vfile < g_file_count) {
                    m68k_write_memory_16(info + 0, FAKE_VREFNUM);
                    m68k_write_memory_32(info + 2, FAKE_DIRID);
                    file_put_name(vfile, info + 6);
                } else {
                    m68k_write_memory_16(info + 0, 0);  /* FSSpec.vRefNum   */
                    m68k_write_memory_32(info + 2, 0);  /* FSSpec.parID     */
                    m68k_write_memory_8 (info + 6, 0);  /* FSSpec.name, ""  */
                }
                m68k_write_memory_16(info + 70,
                                     (unsigned)(unsigned short)res_id);
            }
            /* -244 is voiceNotFound, which is what the caller expects when a
             * VoiceSpec names something that is not installed. */
            tb_return(exc_sp, 12, found ? 0u : 0xFF0Cu, 2);
            return 1;
        }
        if (selector == 8 && bytes == 10) {  /* SndChannelStatus(chan,len,stat) */
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
        if (d0 == 0x000C0008u) {
            /* SndSoundManagerVersion: **no arguments at all**, and a
             * four-byte NumVersion result. Read off the call site in the
             * loaded module -- `subq.l #$4,a7` reserves the long, nothing is
             * pushed, then `move.l (a7)+` pops it and `cmpi.b #$3` tests the
             * major version.
             *
             * So D0's low word is NOT an argument byte count here, and
             * deriving one from it removes eight bytes that were never
             * pushed. Answer 2.0.0: below 3, which selects the classic
             * channel-and-bufferCmd path this host actually models. */
            tb_return(exc_sp, 0, 0x02008000u, 4);
            return 1;
        }
        /* Anything else: answer noErr but **remove exactly what was pushed**.
         * The byte count is in D0 and guessing it is how a stack gets one
         * argument out of step, which does not fail here -- it fails later,
         * somewhere unrelated, as a wild pointer. */
        tb_return(exc_sp, bytes <= 64 ? bytes : 10, 0, 2);
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
        unsigned h = res_find_scoped(type, id, base == 0xA81Fu);
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
    case 0xA99C:                         /* _CountResources(type)  -> short   */
    case 0xA80D: {                       /* _Count1Resources(type) -> short   */
        /* The engine enumerates by type -- count them, then walk them by
         * index -- to find which voices and modules are present.
         *
         * Stubbed, the count was whatever the caller's reserved word already
         * held: it came back as thousands, and the walk that followed made
         * 3,954 _ReleaseResource calls and took 26,245 memory faults reading
         * from addresses like 0x70726F6B.  Nothing about that looks like a
         * missing trap until you count the calls. */
        unsigned type = m68k_read_memory_32(csp);
        int one = (base == 0xA80Du), i, n = 0;
        for (i = 0; i < g_res_count; i++)
            if (g_res[i].type == type && res_in_file(i, one)) n++;
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 4, (unsigned)n, 2);
        return 1;
    }
    case 0xA99D:                         /* _GetIndResource(type, i)          */
    case 0xA80E: {                       /* _Get1IndResource(type, i) -> Hdl  */
        /* One-based, as the Resource Manager is, and in the order they were
         * registered -- which is the order they came out of the file. */
        int idx = (int)(short)m68k_read_memory_16(csp);
        unsigned type = m68k_read_memory_32(csp + 2);
        int one = (base == 0xA80Eu), i, seen = 0;
        unsigned h = 0;
        for (i = 0; i < g_res_count && !h; i++)
            if (g_res[i].type == type && res_in_file(i, one) && ++seen == idx) {
                res_restore(&g_res[i]);
                h = g_res[i].handle;
            }
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 6, h, 4);
        return 1;
    }
    case 0xA9A1:                         /* _GetNamedResource(type, name)     */
    case 0xA820: {                       /* _Get1NamedResource(type, name)    */
        /* **This is how MacinTalk Pro finds everything.** It is a modular
         * engine addressed by name rather than by id: `*TTS`, `*Wave`, `*Snd`,
         * `*Lex`, `*Cmd`, `*XPh`, `*XAl`, `*PhX`, `*AlX`, `*WvX`, plus
         * `EnglPhon` and `EnglAllo`; a voice is `EnglMBruceData`,
         * `EnglMBruceCode`, `EnglMBruce` and `EnglMBruceWave`.
         *
         * Serving it matters even when the answer is nil: the caller reserves
         * four bytes for the Handle, and a stub never writes them, so the
         * engine carries on with whatever was on the stack. That is how a
         * stack address once reached _SizeResource as a Handle -- non-zero, so
         * it passed the nil test, and the failure surfaced two calls later
         * as memFullErr. */
        unsigned name = m68k_read_memory_32(csp);
        unsigned type = m68k_read_memory_32(csp + 4);
        unsigned char buf[64];
        unsigned h = 0;
        int n = 0, i;
        if (name) {
            n = (int)m68k_read_memory_8(name);
            if (n > 63) n = 63;
            for (i = 0; i < n; i++)
                buf[i] = (unsigned char)m68k_read_memory_8(name + 1u + (unsigned)i);
            h = res_find_named(type, buf, n);
        }
        log_res(type, (short)(h ? 0 : -1), h != 0);
        g_res_err = h ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 8, h, 4);
        return 1;
    }
    case 0xA9C5: {                       /* _RsrcMapEntry(h) -> long         */
        /* Where this resource's twelve-byte reference entry sits, measured
         * from the start of the resource MAP.  Named from its call site at
         * gtse 1 +$5B16: one Handle in, a long out, zero treated as failure
         * with _ResError straight after.
         *
         * What Pro does with it is the reason the whole File Manager exists
         * here.  It adds the offset to its in-memory copy of the map, pulls
         * the 24-bit data offset out of the entry with a 68020 bitfield
         * instruction, adds the data-area base from the map's own header copy,
         * seeks to that plus four, and reads SizeResource bytes.  It never
         * loads the 800 KB database into a Handle at all -- which is how this
         * ran on a Mac with 8 MB of RAM. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        g_res_err = (r && r->map_entry) ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 4, (r && r->map_entry) ? (unsigned)r->map_entry : 0, 4);
        return 1;
    }
    /* ---- Fixed point, named from their call sites --------------------- *
     *
     * `$A84D` is handed #$FF0000 and #$10000 -- 255.0 and 1.0 -- and returns
     * one long, and `$A844` is handed a pointer to ten bytes the caller just
     * filled with a word and two longs, which is an extended, and returns a
     * long.  They sit in the documented contiguous block with Long2Fix,
     * Fix2X, X2Frac, FracMul, FixMul and FixRound, and every unserved trap in
     * this engine falls inside it.
     *
     * Toolbox convention: the caller reserves the result, then pushes
     * arguments left to right, so the LAST argument is on top. */
    case 0xA83F:                         /* _Long2Fix(long) -> Fixed          */
        tb_return(exc_sp, 4,
                  (unsigned)(m68k_read_memory_32(csp) << 16), 4);
        return 1;
    case 0xA840:                         /* _Fix2Long(Fixed) -> long          */
        tb_return(exc_sp, 4,
                  (unsigned)((fx_signed(m68k_read_memory_32(csp)) + 0x8000)
                             >> 16), 4);
        return 1;
    case 0xA868:                         /* _FixMul(Fixed, Fixed) -> Fixed    */
        tb_return(exc_sp, 8, fx_mul(m68k_read_memory_32(csp + 4),
                                    m68k_read_memory_32(csp)), 4);
        return 1;
    case 0xA84D:                         /* _FixDiv(Fixed, Fixed) -> Fixed    */
        tb_return(exc_sp, 8, fx_div(m68k_read_memory_32(csp + 4),
                                    m68k_read_memory_32(csp)), 4);
        return 1;
    case 0xA86C:                         /* _FixRound(Fixed) -> short         */
        tb_return(exc_sp, 4,
                  (unsigned)((fx_signed(m68k_read_memory_32(csp)) + 0x8000)
                             >> 16), 2);
        return 1;
    case 0xA84A: {                       /* _FracMul(Fract, Fract) -> Fract   */
        /* Fract is 2.30, so the product shifts by 30 rather than 16. */
        long long r = (long long)fx_signed(m68k_read_memory_32(csp + 4))
                    * (long long)fx_signed(m68k_read_memory_32(csp));
        tb_return(exc_sp, 8, (unsigned)((r + (1 << 29)) >> 30), 4);
        return 1;
    }
    case 0xA843: {                       /* _Fix2X(extended *, Fixed)         */
        /* The pointer is a ten-byte local -- `-$a(a6)` at gtse 1 +$7C88 --
         * which is an extended exactly.  The reserved long is discarded by
         * the caller, so answering with the value costs nothing and satisfies
         * either reading of the result. */
        unsigned val = m68k_read_memory_32(csp);
        unsigned dst = m68k_read_memory_32(csp + 4);
        if (dst) ext80_write(dst, fx_signed(val) / 65536.0);
        tb_return(exc_sp, 8, val, 4);
        return 1;
    }
    case 0xA844: {                       /* _X2Fix(extended *) -> Fixed       */
        unsigned src = m68k_read_memory_32(csp);
        double v = src ? ext80_read(src) : 0.0;
        double f = v * 65536.0;
        long long r;
        if (f > 2147483647.0) r = 2147483647;
        else if (f < -2147483648.0) r = -2147483648LL;
        else r = (long long)(f < 0 ? f - 0.5 : f + 0.5);
        tb_return(exc_sp, 4, (unsigned)r, 4);
        return 1;
    }
    case 0xA846: {                       /* _X2Frac(extended *) -> Fract      */
        unsigned src = m68k_read_memory_32(csp);
        double v = src ? ext80_read(src) : 0.0;
        double f = v * 1073741824.0;               /* 2^30 */
        long long r;
        if (f > 2147483647.0) r = 2147483647;
        else if (f < -2147483648.0) r = -2147483648LL;
        else r = (long long)(f < 0 ? f - 0.5 : f + 0.5);
        tb_return(exc_sp, 4, (unsigned)r, 4);
        return 1;
    }
    case 0xA9EB:                         /* _Pack4 / FP68K                   */
    case 0xA9EC:                         /* _Pack5 / Elems68K                */
        return serve_sane_trap((unsigned short)base, exc_sp, csp, resume_pc);
    case 0xA9A5: {                       /* _SizeResource(h) -> long          */
        /* How big is this resource.  Answerable exactly, because the pristine
         * copy kept for _DetachResource carries the length -- and a wrong
         * answer here is expensive: the engine sizes an allocation from it,
         * and stubbed it read whatever was on the stack and asked for that
         * many bytes.  Open failed with memFullErr and an almost empty heap. */
        ResEntry *r = res_by_handle(m68k_read_memory_32(csp));
        g_res_err = r ? 0 : RES_NOT_FOUND;
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 4, r ? (unsigned)r->len : 0xFFFFFFFFu, 4);
        return 1;                        /* -1 is the documented failure     */
    }
    case 0xA9A6:                         /* _GetResAttrs(h) -> short          */
        /* Nothing is purgeable, preloaded, protected or changed here: the
         * resources come from a folder and never go back. */
        tb_return(exc_sp, 4, 0, 2);
        return 1;
    case 0xA906: {                       /* _NewString(str) -> StringHandle   */
        /* gtse 1 +$65EA reserves a long, pushes a pointer to a Pascal string
         * inside a table it is walking, and stores the result as a Handle --
         * treating nil as MemErr and failing.  47 of them during Open.
         *
         * Worth serving even though Open already returned noErr without it:
         * stubbed, the reserved long was never written, and the engine kept
         * whatever was on the stack.  It was non-zero often enough to pass the
         * nil test, so Open succeeded holding 47 handles to nothing. */
        unsigned src = m68k_read_memory_32(csp);
        unsigned n = src ? m68k_read_memory_8(src) : 0;
        unsigned h = heap_new_handle(n + 1);
        if (h) {
            unsigned blk = m68k_read_memory_32(h), i;
            for (i = 0; i <= n; i++)
                m68k_write_memory_8(blk + i, m68k_read_memory_8(src + i));
        }
        m68k_write_memory_16(MEM_ERR_ADDR, h ? 0u : 0xFF94u);
        tb_return(exc_sp, 4, h, 4);
        return 1;
    }
    case 0xAA52:                         /* _HighLevelFSDispatch, sel in D0   */
        /* Selector 1 is FSMakeFSSpec, read off its arguments rather than
         * recalled: gtse 1 +$F76 reserves a word for the result and pushes a
         * word, a long, a pointer and a pointer -- (vRefNum, dirID, fileName,
         * spec) -- and the three inputs are exactly what it just read out of
         * PBGetFCBInfo.  The 70-byte FSSpec it fills lands at +$1C of the
         * engine's globals, and +$1C plus 70 is +$62, which is the next field
         * the code writes. */
        if (d0 == 1u) {
            unsigned spec = m68k_read_memory_32(csp);
            unsigned name = m68k_read_memory_32(csp + 4);
            unsigned dir  = m68k_read_memory_32(csp + 8);
            unsigned vref = m68k_read_memory_16(csp + 12);
            m68k_write_memory_16(spec + 0, vref);
            m68k_write_memory_32(spec + 2, dir);
            /* The caller may pass the name it just got back, or nil to mean
             * "the file itself"; either way there is one file here. */
            (void)name;
            file_put_name(0, spec + 6);
            tb_return(exc_sp, 14, 0, 2);
            return 1;
        }
        return 0;
    case 0xA994:                         /* _CurResFile -> short             */
        tb_return(exc_sp, 0, 1, 2);
        return 1;
    case 0xA9A4:                         /* _HomeResFile(h) -> short         */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    case 0xA998: {                       /* _UseResFile(short)               */
        /* Selects which file Get1* searches. MacinTalk Pro switches between
         * its own file and a voice's, and they carry resources with the SAME
         * type and id -- `gtsg 0` is 1,032 bytes in the engine and 110 in a
         * voice -- so ignoring this hands back the wrong one. refNum 1 is the
         * flat table, which means "no particular file". */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(csp));
        g_cur_res_file = f ? f->file : -1;
        if (f) {
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(f->file));
            m68k_write_memory_16(CUR_MAP_ADDR,
                                 (unsigned)m68k_read_memory_16(csp));
        }
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    }
    case 0xA997: {                       /* _OpenResFile(name) -> short      */
        tb_return(exc_sp, 4, 1, 2);
        return 1;
    }
    case 0xA81A: {                       /* _HOpenResFile -> short refNum    */
        /* HOpenResFile(short vRefNum, long dirID, ConstStr255Param fileName,
         *              SignedByte permission)
         *
         * Twelve bytes of arguments, so the name is the long at csp+2.  Opens
         * the resource fork for real, same as _OpenRFPerm -- what mattered
         * before was only that the refNum was not -1, which is the value
         * Cecy 1 +$830 tests for, and that is still true. */
        int idx = no_files() ? -1 : file_by_name(m68k_read_memory_32(csp + 2));
        int ref = no_files() ? 1 : file_open(idx, FORK_RSRC);
        if (ref && idx >= 0) {
            g_cur_res_file = idx;
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(idx));
            m68k_write_memory_16(CUR_MAP_ADDR, (unsigned)ref);
        }
        g_res_err = ref ? 0 : (short)(-43);
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 12, (unsigned)(ref ? ref : 1), 2);
        return 1;
    }
    case 0xA9C4: {                       /* _OpenRFPerm -> short refNum      */
        /* OpenRFPerm(ConstStr255Param fileName, short vRefNum,
         *            SignedByte permission)
         *
         * Eight bytes of arguments -- the byte permission still costs two,
         * because `move.b <ea>,-(a7)` keeps A7 even -- so the name is the long
         * at csp+4, and the word result is kept as a refNum.
         *
         * A REAL open: MacinTalk Pro does not stop at the Resource Manager, it
         * walks the map and seeks to byte offsets in the fork, so the refNum
         * it gets back has to name a stream it can actually read. */
        int idx = no_files() ? -1 : file_by_name(m68k_read_memory_32(csp + 4));
        int ref = no_files() ? 1 : file_open(idx, FORK_RSRC);
        /* The Resource Manager makes a newly opened file the current one, and
         * that is what scopes every Get1* that follows. */
        if (ref && idx >= 0) {
            g_cur_res_file = idx;
            /* The Resource Manager points TopMapHndl at the file just opened,
             * and Pro reads it directly rather than asking. */
            m68k_write_memory_32(TOP_MAP_HNDL, file_map_handle(idx));
            m68k_write_memory_16(CUR_MAP_ADDR, (unsigned)ref);
        }
        g_res_err = ref ? 0 : (short)(-43);              /* fnfErr */
        m68k_write_memory_16(RES_ERR_ADDR, (unsigned)(unsigned short)g_res_err);
        tb_return(exc_sp, 8, (unsigned)(ref ? ref : -1), 2);
        return 1;
    }
    case 0xA99A: {                       /* _CloseResFile(short)             */
        OpenFile *f = file_by_ref((int)(short)m68k_read_memory_16(csp));
        if (f) {
            if (g_cur_res_file == f->file) g_cur_res_file = -1;
            f->used = 0;
        }
        g_res_err = 0;
        m68k_write_memory_16(RES_ERR_ADDR, 0);
        tb_return(exc_sp, 2, 0, 0);
        return 1;
    }
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

    /* 68000 unless an engine asks for more; see osp_set_cpu.  `.sp` and
     * MacinTalk 2 are 68000 code and stay that way. */
    g_gestalt_proc = 1;
    g_exc_frame = 6u;
    g_instance_error = 0;
    g_sane_env = 0;
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
    { int fi; for (fi = 0; fi < g_file_count; fi++) {
        if (g_files[fi].fork[0]) free(g_files[fi].fork[0]);
        if (g_files[fi].fork[1]) free(g_files[fi].fork[1]);
    } }
    memset(g_files, 0, sizeof(g_files));
    memset(g_open, 0, sizeof(g_open));
    g_file_count = 0;
    g_cur_res_file = -1;
    g_handle_count = 0;
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
    { int fi; for (fi = 0; fi < g_file_count; fi++) {
        if (g_files[fi].fork[0]) free(g_files[fi].fork[0]);
        if (g_files[fi].fork[1]) free(g_files[fi].fork[1]);
    } }
    memset(g_files, 0, sizeof(g_files));
    memset(g_open, 0, sizeof(g_open));
    g_file_count = 0;
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

/* Choose the CPU, because not every engine runs on the same one.
 *
 * MacinTalk Pro checks Gestalt('proc') during Open and refuses anything below
 * a 68020 -- see the _Gestalt case -- so "Pro" is a hardware requirement and
 * not just a name.  `.sp` and MacinTalk 2 are 68000 code and are left alone;
 * only one engine is live at a time, so this is per-engine rather than global.
 *
 * `proc` is the Gestalt processor value, 1..5, which keeps the CPU and the
 * answer we give about it in one place and impossible to disagree.  Call it
 * straight after osp_init, before loading any code. */
OSP_API int osp_instance_error(void) { return g_instance_error; }
OSP_API const char *osp_last_file_request(void) { return g_last_file_req; }

OSP_API int osp_set_cpu(int proc)
{
    unsigned type;
    switch (proc) {
        case 1: type = M68K_CPU_TYPE_68000; break;
        case 2: type = M68K_CPU_TYPE_68010; break;
        case 3: type = M68K_CPU_TYPE_68020; break;
        case 4: type = M68K_CPU_TYPE_68030; break;
        case 5: type = M68K_CPU_TYPE_68040; break;
        default: return -1;
    }
    g_gestalt_proc = proc;
    /* The 68010 introduced the format/vector word and everything after it kept
     * it, so only the 68000 has a six-byte frame. */
    g_exc_frame = (proc == 1) ? 6u : 8u;
    m68k_set_cpu_type(type);
    m68k_pulse_reset();
    return 0;
}

OSP_API void osp_heap_init(unsigned base, unsigned size)
{
    g_heap_base = g_heap_next = base;
    g_heap_end = base + size;
    /* Fill in the one zone (see MAGIC_ZONE).  Only the fields a client can
     * reasonably read are set; a caller that walks the free list would need
     * far more, and the trap log will say so if one ever does.  Written here
     * rather than at osp_init because the honest answers are the heap's. */
    m68k_write_memory_32(MAGIC_ZONE +  0, g_heap_end);        /* bkLim      */
    m68k_write_memory_32(MAGIC_ZONE +  4, 0);                 /* purgePtr   */
    m68k_write_memory_32(MAGIC_ZONE +  8, 0);                 /* hFstFree   */
    m68k_write_memory_32(MAGIC_ZONE + 12, size);              /* zcbFree    */
    m68k_write_memory_32(MAGIC_ZONE + 16, 0);                 /* gzProc     */
    m68k_write_memory_16(MAGIC_ZONE + 20, 64);                /* moreMast   */
    m68k_write_memory_32(MAGIC_ZONE + 48, base);              /* allocPtr   */
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
                                  const unsigned char *data, int len,
                                  int file_index)
{
    unsigned blk, mp;
    if (g_res_count >= MAX_RES) return 0;
    blk = heap_alloc((unsigned)len);
    if (!blk) return 0;
    memcpy(g_ram + blk, data, (size_t)len);
    mp = heap_alloc(4);
    if (!mp) return 0;
    m68k_write_memory_32(mp, blk);
    note_handle(mp, (unsigned)len);
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
    g_res[g_res_count].map_entry = 0;
    g_res[g_res_count].file = file_index;
    memset(g_res[g_res_count].name, 0, sizeof(g_res[g_res_count].name));
    if (g_res[g_res_count].bytes)
        memcpy(g_res[g_res_count].bytes, data, (size_t)len);
    g_res_count++;
    return mp;
}

/* Give a registered resource its Mac name, so Get1NamedResource can find it.
 *
 * Separate from osp_add_resource rather than another argument to it, because
 * the engines that came first never needed names and their callers should not
 * have to say so.  -> 0, or -1 if that resource was never registered. */
OSP_API int osp_name_resource(unsigned handle, const char *name, int len)
{
    ResEntry *r = res_by_handle(handle);
    if (!r || len < 0 || len > 63) return -1;
    memset(r->name, 0, sizeof(r->name));
    memcpy(r->name, name, (size_t)len);
    return 0;
}

/* Tell the host where a resource's entry sits in its file's resource map.
 *
 * Computed on the Python side, where the fork is already parsed -- see
 * rsrc.Resource.map_entry -- rather than parsing the map again in C.
 * -> 0, or -1 if that resource was never registered. */
OSP_API int osp_map_entry(unsigned handle, int entry)
{
    ResEntry *r = res_by_handle(handle);
    if (!r) return -1;
    r->map_entry = entry;
    return 0;
}

/* Register the one file: its name, and its data fork.
 *
 * Kept host-side rather than copied into emulated RAM. MacinTalk Pro's lexicon
 * is 572,928 bytes and it reads it a piece at a time through the File Manager,
 * so there is no reason for all of it to sit in the 68000's address space --
 * unlike a resource, which the engine gets a Handle to and dereferences. */
OSP_API int osp_add_file(const unsigned char *name, int namelen,
                         const unsigned char *data, int dlen,
                         const unsigned char *rsrc, int rlen)
{
    FileEntry *f;
    if (g_file_count >= MAX_FILES) return -1;
    if (namelen < 0 || namelen > 63 || dlen < 0 || rlen < 0) return -1;
    f = &g_files[g_file_count];
    memset(f, 0, sizeof(*f));
    memset(f->name, 0, sizeof(f->name));
    f->name[0] = (unsigned char)namelen;
    memcpy(f->name + 1, name, (size_t)namelen);
    if (data && dlen) {
        f->fork[FORK_DATA] = (unsigned char *)malloc((size_t)dlen);
        if (!f->fork[FORK_DATA]) return -1;
        memcpy(f->fork[FORK_DATA], data, (size_t)dlen);
        f->len[FORK_DATA] = dlen;
    }
    if (rsrc && rlen) {
        f->fork[FORK_RSRC] = (unsigned char *)malloc((size_t)rlen);
        if (!f->fork[FORK_RSRC]) return -1;
        memcpy(f->fork[FORK_RSRC], rsrc, (size_t)rlen);
        f->len[FORK_RSRC] = rlen;
    }
    return g_file_count++;
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
OSP_API int osp_add_voice(unsigned creator, unsigned id, int res_id,
                          int file_index)
{
    if (g_voice_count >= MAX_VOICES) return -1;
    g_voices[g_voice_count].creator = creator;
    g_voices[g_voice_count].id = id;
    g_voices[g_voice_count].res_id = (short)res_id;
    g_voices[g_voice_count].file = file_index;
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
