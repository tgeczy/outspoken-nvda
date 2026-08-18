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

