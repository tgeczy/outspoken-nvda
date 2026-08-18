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
