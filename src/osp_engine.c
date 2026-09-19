/* osp_engine.c -- the engines as the host drives them: text in, PCM out.
 *
 * Until 2.0 the Python modules drove every engine over ctypes -- loading its
 * resources, calling its component, pumping its callbacks, tidying its PCM --
 * and the host was a CPU with a Toolbox.  This file and its `osp_engine_*.c`
 * siblings are those modules ported, one file per engine as the Python had
 * one module per engine, behind one surface that mirrors the Python's exactly:
 *
 *     open(manifest)  select(voice)  set_rate  set_pitch  set_voice_hz
 *     set_inflection  set_numbers    translate speak      stop  close
 *
 * Mirrors it *exactly* because that is the boundary `tools/render_oracle.py`
 * compares at: the same script through the Python and through this, byte for
 * byte.  Nothing here is allowed to be better than its specification until
 * the specification changes too.
 *
 * **One engine at a time**, as before.  The host is one CPU with global
 * state and `osp_init` is "start over", so opening a second engine ends the
 * first -- the Python's `_LIVE` list, made structural.
 *
 * **The manifest.**  An engine is opened from a small text description, one
 * `key=value` per line, naming every file by explicit path:
 *
 *     engine=sp | mtk2 | mtk3 | pro
 *     folder=<engine folder>                 (mtk3, pro)
 *     file:<name>=<path>                     (engine-level files)
 *     voice=<creator>\t<id>\t<name>\t<folder>
 *     vfile:<kind>=<path>                    (after its voice line)
 *     select=<creator>\t<id>
 *
 * Explicit paths, deliberately: the oracle fills them from the Python
 * modules' own `find()` and the catalogue in C fills them later, and either
 * way no directory is walked from inside an engine.  So an engine-port
 * failure can never be a catalogue failure, and the surface does not change
 * when the catalogue lands.
 */

#define ENG_NOT_PORTED   (-100)   /* the kind exists, its C does not yet */
#define ENG_ERR_MANIFEST (-1)
#define ENG_ERR_FILE     (-2)
#define ENG_ERR_OPEN     (-3)
#define ENG_ERR_STATE    (-4)

#define ENG_NUM_OFF    0
#define ENG_NUM_WORDS  1
#define ENG_NUM_DIGITS 2

#define ENG_MAX_FILES  64
#define ENG_MAX_VOICES 40
#define ENG_MAX_VFILES 8
#define ENG_PATH       1024

typedef struct { char name[64]; char path[ENG_PATH]; } EngFile;
typedef struct {
    char creator[5];
    int id;
    char name[64];
    char folder[ENG_PATH];
    EngFile files[ENG_MAX_VFILES];
    int nfiles;
} EngVoice;
typedef struct {
    int kind;                       /* ENG_KIND_* */
    char folder[ENG_PATH];
    EngFile files[ENG_MAX_FILES];
    int nfiles;
    EngVoice voices[ENG_MAX_VOICES];
    int nvoices;
    char sel_creator[5];
    int sel_id;
} EngManifest;

enum { ENG_KIND_NONE = 0, ENG_KIND_SP, ENG_KIND_MTK2, ENG_KIND_MTK3, ENG_KIND_PRO };

/* What one engine implements.  A NULL slot means "this engine has no such
 * setting", and the call is a harmless no-op -- exactly as engine.py's
 * set_inflection does nothing, present so the driver need not ask. */
typedef struct {
    int  (*open)(const EngManifest *m);
    void (*close)(void);
    int  (*select)(const char *creator, int id);      /* -> 1 took it, 0 refused */
    void (*set_rate)(int wpm);
    void (*set_pitch)(int tenths);
    void (*set_voice_hz)(double hz);
    void (*set_inflection)(int percent);
    void (*translate)(const unsigned char *text, int len, NumBuf *out);
    void (*speak)(const unsigned char *prepared, int len, NumBuf *out);
    void (*stop)(void);
} EngOps;

static struct {
    int kind;
    const EngOps *ops;
    int numbers;                    /* ENG_NUM_*; the Python's number_mode */
    double sp_hz;                   /* the 1984 voice's base pitch, by its id */
    char error[256];
    NumBuf pcm;                     /* the last utterance */
} g_eng;

static void eng_fail(const char *what)
{
    strncpy(g_eng.error, what, sizeof g_eng.error - 1);
    g_eng.error[sizeof g_eng.error - 1] = 0;
}

/* ---- files -------------------------------------------------------------------- */

#if defined(_WIN32)
/* UTF-8 to UTF-16, for _wfopen.  Hand-rolled rather than MultiByteToWideChar
 * because <windows.h> declares names this host already uses for its own
 * types.  -> 0, or -1 for malformed input or a path too long. */
static int eng_utf16(const char *s, wchar_t *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        unsigned cp, need;
        if (*p < 0x80) { cp = *p++; need = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p++ & 0x1F; need = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p++ & 0x0F; need = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p++ & 0x07; need = 3; }
        else return -1;
        while (need--) {
            if ((*p & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp >= 0x10000) {
            if (n + 3 > cap) return -1;
            cp -= 0x10000;
            out[n++] = (wchar_t)(0xD800 + (cp >> 10));
            out[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 2 > cap) return -1;
            out[n++] = (wchar_t)cp;
        }
    }
    out[n] = 0;
    return 0;
}
#endif

/* Read a whole file.  Paths arrive as UTF-8; on Windows fopen wants the
 * ANSI code page, which loses any name outside it, so go through the wide
 * API there.  -> 0, or -1 with *data NULL. */
static int eng_slurp(const char *path, unsigned char **data, int *len)
{
    FILE *f;
    long n;
    *data = NULL; *len = 0;
#if defined(_WIN32)
    {
        wchar_t wide[ENG_PATH];
        if (eng_utf16(path, wide, ENG_PATH) != 0) return -1;
        f = _wfopen(wide, L"rb");
    }
#else
    f = fopen(path, "rb");
#endif
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    *data = (unsigned char *)malloc((size_t)n + 1);
    if (!*data) { fclose(f); return -1; }
    if (n > 0 && fread(*data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(*data); *data = NULL; return -1;
    }
    (*data)[n] = 0;
    *len = (int)n;
    fclose(f);
    return 0;
}

#if defined(_WIN32)
/* UTF-16 back to UTF-8, for the names a folder listing hands back. */
static int eng_utf8_from_utf16(const wchar_t *s, char *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        unsigned cp = *s++;
        if (cp >= 0xD800 && cp < 0xDC00 && *s >= 0xDC00 && *s < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*s++ - 0xDC00);
        if (cp < 0x80) { if (n + 2 > cap) return -1; out[n++] = (char)cp; }
        else if (cp < 0x800) { if (n + 3 > cap) return -1; out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { if (n + 4 > cap) return -1; out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else { if (n + 5 > cap) return -1; out[n++] = (char)(0xF0 | (cp >> 18)); out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[n] = 0;
    return 0;
}
#endif

/* A folder's entries, sorted as Python's sorted(os.listdir()) sorts them:
 * by code point, which for UTF-8 is byte order.  `.` and `..` left out. */
typedef struct { char **names; int n, cap; } EngDir;

static int eng_dir_add(EngDir *d, const char *name)
{
    char *copy;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (d->n >= d->cap) {
        int want = d->cap ? d->cap * 2 : 64;
        char **q = (char **)realloc(d->names, sizeof(char *) * (size_t)want);
        if (!q) return -1;
        d->names = q; d->cap = want;
    }
    copy = (char *)malloc(strlen(name) + 1);
    if (!copy) return -1;
    strcpy(copy, name);
    d->names[d->n++] = copy;
    return 0;
}
static int eng_dir_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static void eng_dir_free(EngDir *d)
{
    int i;
    for (i = 0; i < d->n; i++) free(d->names[i]);
    free(d->names);
    memset(d, 0, sizeof *d);
}
static int eng_listdir(const char *path, EngDir *d)
{
    memset(d, 0, sizeof *d);
#if defined(_WIN32)
    {
        wchar_t wide[ENG_PATH + 4];
        struct _wfinddata_t f;
        intptr_t h;
        char name[ENG_PATH];
        size_t n;
        if (eng_utf16(path, wide, ENG_PATH) != 0) return -1;
        n = wcslen(wide);
        if (n + 3 >= ENG_PATH + 4) return -1;
        wide[n] = L'\\'; wide[n + 1] = L'*'; wide[n + 2] = 0;
        h = _wfindfirst(wide, &f);
        if (h == -1) return -1;
        do {
            if (eng_utf8_from_utf16(f.name, name, sizeof name) == 0)
                if (eng_dir_add(d, name)) { _findclose(h); eng_dir_free(d); return -1; }
        } while (_wfindnext(h, &f) == 0);
        _findclose(h);
    }
#else
    {
        DIR *dir = opendir(path);
        struct dirent *e;
        if (!dir) return -1;
        while ((e = readdir(dir)) != NULL)
            if (eng_dir_add(d, e->d_name)) { closedir(dir); eng_dir_free(d); return -1; }
        closedir(dir);
    }
#endif
    if (d->n > 1) qsort(d->names, (size_t)d->n, sizeof(char *), eng_dir_cmp);
    return 0;
}

static const char *eng_file(const EngManifest *m, const char *name)
{
    int i;
    for (i = 0; i < m->nfiles; i++)
        if (strcmp(m->files[i].name, name) == 0) return m->files[i].path;
    return NULL;
}

static const char *eng_vfile(const EngVoice *v, const char *kind)
{
    int i;
    for (i = 0; i < v->nfiles; i++)
        if (strcmp(v->files[i].name, kind) == 0) return v->files[i].path;
    return NULL;
}

/* Copy a NUL-terminated field, refusing to overflow. -> 0 or -1 */
static int eng_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) return -1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

/* ---- the manifest -------------------------------------------------------------- */

static int eng_parse_voice(EngVoice *v, const char *val, size_t n)
{
    /* creator \t id \t name \t folder */
    const char *end = val + n, *p = val, *t;
    memset(v, 0, sizeof *v);
    t = memchr(p, '\t', (size_t)(end - p)); if (!t) return -1;
    if (eng_field(v->creator, sizeof v->creator, p, (size_t)(t - p))) return -1;
    p = t + 1;
    t = memchr(p, '\t', (size_t)(end - p)); if (!t) return -1;
    v->id = atoi(p);
    p = t + 1;
    t = memchr(p, '\t', (size_t)(end - p));
    if (!t) {
        if (eng_field(v->name, sizeof v->name, p, (size_t)(end - p))) return -1;
        return 0;
    }
    if (eng_field(v->name, sizeof v->name, p, (size_t)(t - p))) return -1;
    p = t + 1;
    return eng_field(v->folder, sizeof v->folder, p, (size_t)(end - p));
}

static int eng_parse(EngManifest *m, const char *text)
{
    const char *p = text;
    memset(m, 0, sizeof *m);
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        size_t klen, vlen;
        const char *key, *val;
        if (end > p && end[-1] == '\r') end--;
        if (end == p) { p = nl ? nl + 1 : end; continue; }
        if (!eq || eq > end) return -1;
        key = p; klen = (size_t)(eq - p);
        val = eq + 1; vlen = (size_t)(end - val);

        if (klen == 6 && memcmp(key, "engine", 6) == 0) {
            if (vlen == 2 && memcmp(val, "sp", 2) == 0) m->kind = ENG_KIND_SP;
            else if (vlen == 4 && memcmp(val, "mtk2", 4) == 0) m->kind = ENG_KIND_MTK2;
            else if (vlen == 4 && memcmp(val, "mtk3", 4) == 0) m->kind = ENG_KIND_MTK3;
            else if (vlen == 3 && memcmp(val, "pro", 3) == 0) m->kind = ENG_KIND_PRO;
            else return -1;
        } else if (klen == 6 && memcmp(key, "folder", 6) == 0) {
            if (eng_field(m->folder, sizeof m->folder, val, vlen)) return -1;
        } else if (klen > 5 && memcmp(key, "file:", 5) == 0) {
            EngFile *f;
            if (m->nfiles >= ENG_MAX_FILES) return -1;
            f = &m->files[m->nfiles++];
            if (eng_field(f->name, sizeof f->name, key + 5, klen - 5)) return -1;
            if (eng_field(f->path, sizeof f->path, val, vlen)) return -1;
        } else if (klen == 5 && memcmp(key, "voice", 5) == 0) {
            if (m->nvoices >= ENG_MAX_VOICES) return -1;
            if (eng_parse_voice(&m->voices[m->nvoices], val, vlen)) return -1;
            m->nvoices++;
        } else if (klen > 6 && memcmp(key, "vfile:", 6) == 0) {
            EngVoice *v; EngFile *f;
            if (m->nvoices == 0) return -1;
            v = &m->voices[m->nvoices - 1];
            if (v->nfiles >= ENG_MAX_VFILES) return -1;
            f = &v->files[v->nfiles++];
            if (eng_field(f->name, sizeof f->name, key + 6, klen - 6)) return -1;
            if (eng_field(f->path, sizeof f->path, val, vlen)) return -1;
        } else if (klen == 6 && memcmp(key, "select", 6) == 0) {
            const char *t = memchr(val, '\t', vlen);
            if (!t) return -1;
            if (eng_field(m->sel_creator, sizeof m->sel_creator, val, (size_t)(t - val))) return -1;
            m->sel_id = atoi(t + 1);
        } else {
            return -1;
        }
        p = nl ? nl + 1 : end;
    }
    return m->kind ? 0 : -1;
}

/* ---- shared helpers the engines use ------------------------------------------- */

/* str.strip() as the Python modules apply it to text on its way to an
 * engine: ASCII whitespace, the four separators 0x1C-0x1F, and MacRoman
 * 0xCA, which is the non-breaking space the Unicode strip also removes. */
static int eng_is_space(unsigned char c)
{
    return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || c == 0xCA;
}
static void eng_strip(const unsigned char **s, int *n)
{
    while (*n > 0 && eng_is_space((*s)[0])) { (*s)++; (*n)--; }
    while (*n > 0 && eng_is_space((*s)[*n - 1])) (*n)--;
}

/* str.isalpha() per MacRoman byte.  Over 0x80-0xFF it is the same table as
 * `\w` (there are no digits up there), and that is a fact about MacRoman
 * rather than a shortcut, so it is checked by the number oracle's table
 * rather than assumed here. */
static int eng_is_alpha(unsigned char c)
{
    if (c >= 0x80) return NUM_WORD_HI[c - 0x80];
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* numwords.normalise as the engines call it: `number_mode` words or digits
 * rewrites, anything else leaves the text alone. */
static void eng_numbers(const unsigned char *text, int len, int spanish, NumBuf *out)
{
    int need;
    if (g_eng.numbers != ENG_NUM_WORDS && g_eng.numbers != ENG_NUM_DIGITS) {
        nb_put(out, text, (size_t)len);
        return;
    }
    need = osp_numbers(text, len, g_eng.numbers == ENG_NUM_DIGITS, spanish, NULL, 0);
    if (need < 0) { out->oom = 1; return; }
    if (nb_reserve(out, (size_t)need)) return;
    osp_numbers(text, len, g_eng.numbers == ENG_NUM_DIGITS, spanish,
                out->p + out->len, (int)(out->cap - out->len));
    out->len += (size_t)need;
}

/* Run one of the size-needed text calls straight into `out`. -> 0 or -1 */
static int eng_text_into(int (*fn)(const unsigned char *, int, unsigned char *, int),
                         const unsigned char *text, int len, NumBuf *out)
{
    int need = fn(text, len, NULL, 0);
    if (need < 0) { if (need == -1) out->oom = 1; return -1; }
    if (nb_reserve(out, (size_t)need)) return -1;
    fn(text, len, out->p + out->len, (int)(out->cap - out->len));
    out->len += (size_t)need;
    return 0;
}

/* The engine modules' `translate` for the three Speech Manager engines: the
 * number pass, then every character in SPOKEN_PUNCTUATION replaced by a
 * space -- a space and not nothing, or the words either side run together.
 * MacinTalk 2, 3 and Pro share the string and the rule. */
static const char ENG_SPOKEN_PUNCTUATION[] = "()[]{}<>@#$%^&*+=/\\|~`\"_";

static void eng_translate_sm(const unsigned char *text, int len, int spanish, NumBuf *out)
{
    size_t i, start = out->len;
    eng_numbers(text, len, spanish, out);
    for (i = start; i < out->len; i++)
        if (out->p[i] < 0x80 && strchr(ENG_SPOKEN_PUNCTUATION, (char)out->p[i]))
            out->p[i] = ' ';
}

/* ---- MacRoman, for names that reach the engine ---------------------------------- */

/* MacRoman 0x80-0xFF as Unicode, generated from Python's codec. */
static const unsigned short ENG_MACROMAN_HI[128] = {
    /* 80 */ 0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    /* 88 */ 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    /* 90 */ 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    /* 98 */ 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    /* A0 */ 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    /* A8 */ 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    /* B0 */ 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    /* B8 */ 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    /* C0 */ 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    /* C8 */ 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    /* D0 */ 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    /* D8 */ 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    /* E0 */ 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    /* E8 */ 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    /* F0 */ 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    /* F8 */ 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

/* UTF-8 to MacRoman, as `str.encode("mac-roman", "replace")`: a character
 * the encoding cannot carry becomes `?`.  Malformed UTF-8 bytes become `?`
 * one at a time.  -> bytes written, at most `cap`. */
static int eng_macroman(const char *s, unsigned char *out, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < cap) {
        unsigned cp, need, k;
        const unsigned char *start = p;
        if (*p < 0x80) { cp = *p++; need = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p++ & 0x1F; need = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p++ & 0x0F; need = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p++ & 0x07; need = 3; }
        else { p++; out[n++] = '?'; continue; }
        for (k = 0; k < need; k++) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFFFFFFu; break; }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp == 0xFFFFFFFFu) { p = start + 1; out[n++] = '?'; continue; }
        if (cp < 0x80) { out[n++] = (unsigned char)cp; continue; }
        for (k = 0; k < 128; k++) if (ENG_MACROMAN_HI[k] == cp) break;
        out[n++] = k < 128 ? (unsigned char)(0x80 + k) : '?';
    }
    return n;
}

/* ---- the engines --------------------------------------------------------------- */

#include "osp_engine_sp.c"
#include "osp_engine_mtk2.c"
#include "osp_engine_mtk3.c"
#include "osp_engine_pro.c"

static const EngOps *eng_ops_for(int kind)
{
    switch (kind) {
        case ENG_KIND_SP:   return &ENG_SP_OPS;
        case ENG_KIND_MTK2: return &ENG_MTK2_OPS;
        case ENG_KIND_MTK3: return &ENG_MTK3_OPS;
        case ENG_KIND_PRO:  return &ENG_PRO_OPS;
        default:            return NULL;
    }
}

/* ---- the API -------------------------------------------------------------------- */

OSP_API void osp_engine_close(void)
{
    if (g_eng.ops && g_eng.ops->close) g_eng.ops->close();
    g_eng.ops = NULL;
    g_eng.kind = ENG_KIND_NONE;
    nb_free(&g_eng.pcm);
    osp_nrl_unload();
    osp_shutdown();
}

OSP_API const char *osp_engine_error(void) { return g_eng.error; }

/* Open an engine from a manifest (see the file comment).  -> 0; -100 when the
 * engine is not yet ported; another negative with osp_engine_error() set. */
OSP_API int osp_engine_open(const char *manifest)
{
    EngManifest *m;
    const EngOps *ops;
    int r;

    osp_engine_close();
    g_eng.error[0] = 0;
    g_eng.numbers = ENG_NUM_WORDS;              /* the Python's default */

    m = (EngManifest *)calloc(1, sizeof *m);
    if (!m) { eng_fail("no memory"); return ENG_ERR_OPEN; }
    if (eng_parse(m, manifest) != 0) {
        free(m); eng_fail("manifest does not parse"); return ENG_ERR_MANIFEST;
    }
    ops = eng_ops_for(m->kind);
    if (!ops) { free(m); eng_fail("engine not ported yet"); return ENG_NOT_PORTED; }

    /* A fresh machine, as osp.Host() makes one. */
    if (osp_init(0x01000000u) != 0) { free(m); eng_fail("osp_init failed"); return ENG_ERR_OPEN; }
    r = ops->open(m);
    g_eng.kind = m->kind;
    /* The 1984 driver's two voices are named by their pitch, 110 and 250 Hz,
     * and that number is their id; the settings layer offsets from it. */
    g_eng.sp_hz = m->kind == ENG_KIND_SP ? (double)m->sel_id : 0.0;
    free(m);
    if (r != 0) {
        osp_shutdown();
        g_eng.kind = ENG_KIND_NONE;
        if (!g_eng.error[0]) eng_fail("engine would not open");
        return r;
    }
    g_eng.ops = ops;
    return 0;
}

OSP_API int osp_engine_select(const char *creator, int id)
{
    if (!g_eng.ops) return 0;
    if (g_eng.kind == ENG_KIND_SP) {
        /* Selecting a 1984 voice is choosing its base pitch. */
        g_eng.sp_hz = (double)id;
        osp_engine_set_voice_hz(g_eng.sp_hz);
        return 1;
    }
    return g_eng.ops->select ? g_eng.ops->select(creator, id) : 1;
}
OSP_API void osp_engine_set_rate(int wpm)
{
    if (g_eng.ops && g_eng.ops->set_rate) g_eng.ops->set_rate(wpm);
}
OSP_API void osp_engine_set_pitch(int tenths)
{
    if (g_eng.ops && g_eng.ops->set_pitch) g_eng.ops->set_pitch(tenths);
}
OSP_API void osp_engine_set_voice_hz(double hz)
{
    if (g_eng.ops && g_eng.ops->set_voice_hz) g_eng.ops->set_voice_hz(hz);
}
OSP_API void osp_engine_set_inflection(int percent)
{
    if (g_eng.ops && g_eng.ops->set_inflection) g_eng.ops->set_inflection(percent);
}
OSP_API void osp_engine_set_numbers(int mode)
{
    g_eng.numbers = mode;
}

/* The engine's own `translate`: what `speak` is then handed.  MacRoman in
 * and out, size-needed contract as osp_numbers. */
OSP_API int osp_engine_translate(const unsigned char *text, int len,
                                 unsigned char *out, int cap)
{
    NumBuf b;
    int need;
    if (!g_eng.ops) return ENG_ERR_STATE;
    nb_init(&b);
    g_eng.ops->translate(text, len < 0 ? 0 : len, &b);
    if (b.oom) { nb_free(&b); return -1; }
    need = (int)b.len;
    if (out && cap > 0) memcpy(out, b.p, b.len < (size_t)cap ? b.len : (size_t)cap);
    nb_free(&b);
    return need;
}

/* Render `prepared` -- the output of osp_engine_translate -- and hold the
 * PCM for osp_engine_pcm.  -> bytes of 8-bit unsigned PCM at the engine's
 * native rate, after the module's own tidying; negative on failure. */
OSP_API int osp_engine_speak(const unsigned char *prepared, int len)
{
    if (!g_eng.ops) return ENG_ERR_STATE;
    nb_free(&g_eng.pcm);
    nb_init(&g_eng.pcm);
    g_eng.ops->speak(prepared, len < 0 ? 0 : len, &g_eng.pcm);
    if (g_eng.pcm.oom) { nb_free(&g_eng.pcm); return -1; }
    return (int)g_eng.pcm.len;
}

OSP_API int osp_engine_pcm(unsigned char *out, int cap)
{
    size_t n = g_eng.pcm.len < (size_t)(cap < 0 ? 0 : cap) ? g_eng.pcm.len : (size_t)(cap < 0 ? 0 : cap);
    if (out && n) memcpy(out, g_eng.pcm.p, n);
    return (int)n;
}

OSP_API void osp_engine_stop(void)
{
    if (g_eng.ops && g_eng.ops->stop) g_eng.ops->stop();
}

/* The driver's 0-100 scales on top of the engines' own. */
#include "osp_settings.c"
