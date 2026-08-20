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

/* Ticks, the 60 Hz counter.  **MacinTalk Pro reads low memory $016A
 * directly** rather than calling _TickCount -- `move.l $16a.w,d0` at
 * 0x1B20C0, compared against a deadline it stored -- so a counter that
 * only advances when the trap is called leaves it waiting forever.
 *
 * Advanced from the instruction count: a 68020 Mac managed a few million
 * instructions a second and a tick is a sixtieth of one, so tens of
 * thousands of instructions per tick is the right order. What matters is
 * that it is monotonic and that a spin loop cannot outrun it. */
#define TICKS_ADDR       0x016Au
#define INSTR_PER_TICK   40000u
static unsigned g_ticks;          /* _TickCount, and low memory $016A  */
static long long g_tick_instr;    /* when the last tick was issued     */
/* Off by default, and that is not caution: **`.sp` is time-sensitive.**
 * A clock that advances by itself makes the same sentence render
 * differently twice, which three engine tests catch immediately. Only
 * MacinTalk Pro waits on Ticks, so only MacinTalk Pro turns this on. */
static int      g_tick_auto;
/* Experiment only: seed one synthetic sound callback so Pro's own
 * callBackCmds become observable. Off by default. */
static int      g_seed_cb;

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

/* File Manager completion routines -- the other half of an asynchronous call.
 *
 * MacinTalk Pro reads its lexicon with `_Read` and the async bit set ($A402),
 * parks the module that asked it, and waits for the File Manager to call the
 * routine in ioCompletion.  Copying the bytes and returning is only half the
 * service: with no completion the engine sleeps forever, which is exactly what
 * it did.  A trap answered synchronously when it was asked asynchronously is
 * the same lie as a stubbed one -- every field comes back correct and only the
 * callback is missing, so nothing downstream can tell.
 *
 * Same construction as the deferred task above, and for the same reason:
 * running emulated code from inside the trap handler would re-enter the CPU.
 * A queue rather than one slot, because a caller may have several requests
 * outstanding, and they must complete in the order they were made. */
#define IOC_CAP 16
static struct { unsigned proc, pb, result; } g_ioc[IOC_CAP];
static int      g_ioc_n;          /* queued and not yet run                 */
static int      g_in_ioc;
static int      g_ioc_runs;
static int      g_ioc_dropped;    /* a full queue is a fault, never silent  */


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


