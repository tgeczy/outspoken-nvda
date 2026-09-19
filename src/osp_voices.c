/* osp_voices.c -- the voice catalogue: what the user has, and how to open it.
 *
 * A port of `voices.py` (the `ttvd` as Apple's VoiceDescription, the gating
 * that keeps a voice off the list when its engine or its own parts are
 * missing) together with the engine-finding halves of `rom.py`,
 * `macintalk2.find`, `macintalk3.engine_dir` and `macintalkpro.engine_dir`,
 * and the shape of `outspoken.py`'s `_catalogue`: the ids NVDA persists and
 * the labels it shows.  `tools/catalogue_oracle.py` diffs all of it against
 * the Python.
 *
 * Given the search roots -- NVDA's are decided in Python, where NVDA's
 * configuration folder is NVDA's business; the CLI and Android hand in
 * theirs -- this answers three questions every front end asks: which voices
 * can actually speak, what to show for each, and the manifest that opens
 * its engine (see osp_engine.c).  Built, never hardcoded: which engines a
 * user has depends entirely on what they extracted from their own disk.
 *
 * Two deliberate departures from the Python, both about order and neither
 * about outcome: a folder is walked in sorted order where `os.walk` uses
 * the operating system's, so two copies of one file inside one root resolve
 * the same way on every platform; and a name is lower-cased for sorting on
 * its ASCII letters only, which is every voice name Apple shipped.
 */

#define VC_MAX_VOICES   64
#define VC_MAX_FILES    12
#define VC_MAX_SKIPPED  64
#define VC_MAX_ROOTS    16
#define VC_VD_LEN       362      /* sizeof(VoiceDescription) */
#define VC_MTK3_WAVE_OFF 648
#define VC_MTK3_NO_WAVE  1

typedef struct {
    char kind[5];               /* "sp", "mtk2", "mtk3", "gala" -- the driver's */
    char creator[5];            /* VoiceSpec.creator, MacRoman */
    int id;
    int hz;                     /* .sp only: the base pitch the voice is named for */
    unsigned char name[64];     /* Pascal-string text, MacRoman */
    int name_len;
    int gender;                 /* VoiceDescription.gender */
    int language;               /* VoiceDescription.language */
    int ttvi_id, ttvw_id;       /* -1 when the voice names none */
    int has_ttvi, has_ttvw;
    char folder[ENG_PATH];
    EngFile files[VC_MAX_FILES]; /* resource type -> path */
    int nfiles;
    int has_rsrcfork, has_index; /* the voice folder's non-resource parts */
} VcVoice;

typedef struct { char folder[256]; char reason[256]; } VcSkipped;

static struct {
    char roots[VC_MAX_ROOTS][ENG_PATH];
    int nroots;
    VcVoice voices[VC_MAX_VOICES];
    int nvoices;
    VcSkipped skipped[VC_MAX_SKIPPED];
    int nskipped;
    /* The 1984 engine's files, first found wins, and MacinTalk 2's. */
    char sp_files[4][ENG_PATH];             /* DRVR, TALK, RULZ, DICT */
    char mtk2_files[8][ENG_PATH];           /* Cecy_1, Cecy_3, the six tables */
    char mtk3_dir[ENG_PATH];
    char gala_dir[ENG_PATH];
    char cami_dir[ENG_PATH];
    int scanned;
} g_vc;

static const char *const VC_SP_NAMES[4] = {
    "DRVR_1030.bin", "TALK_1001.bin", "RULZ_1129.bin", "DICT_-4048.bin"
};
static const char *const VC_MTK2_NAMES[8] = {
    "Cecy_1.bin", "Cecy_3.bin",
    "ttsr_1.bin", "ttsd_1.bin", "ttsd_2.bin", "ttss_0.bin", "ttph_1.bin", "ttop_1.bin"
};
/* What has to be in ONE folder before an engine's voices can be spoken. */
static const char *const VC_ENGINE_FILES_MTK2[] = { "Cecy_1.bin", "Cecy_3.bin", NULL };
static const char *const VC_ENGINE_FILES_GALA[] = { "gtse_1.bin", "datafork.bin", "rsrcfork.bin", NULL };
static const char *const VC_ENGINE_FILES_MTK3[] = { "ttvi_10.bin", "ttvi_8.bin", "ttvi_9.bin", "ttss_0.bin", NULL };
static const char *const VC_ENGINE_FILES_CAMI[] = { "gtse_99.bin", "rsrcfork.bin", NULL };

#if defined(_WIN32)
#  define VC_SEP '\\'
#else
#  define VC_SEP '/'
#endif

static void vc_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la + 1 + strlen(b) + 1 > cap) { out[0] = 0; return; }
    memcpy(out, a, la);
    /* os.path.join: no second separator after one already there */
    if (la && a[la - 1] != '/' && a[la - 1] != '\\') out[la++] = VC_SEP;
    strcpy(out + la, b);
}

static int eng_isdir(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    wchar_t wide[ENG_PATH];
    if (eng_utf16(path, wide, ENG_PATH) != 0) return 0;
    return _wstat(wide, &st) == 0 && (st.st_mode & _S_IFDIR);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int eng_isfile(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    wchar_t wide[ENG_PATH];
    if (eng_utf16(path, wide, ENG_PATH) != 0) return 0;
    return _wstat(wide, &st) == 0 && (st.st_mode & _S_IFREG);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* ---- walking --------------------------------------------------------------- */

/* os.walk, top-down, in sorted order.  `fn` sees each folder with its
 * sorted listing and returns nonzero to stop the whole walk. */
static int vc_walk(const char *dir, int depth,
                   int (*fn)(const char *dir, const EngDir *listing, void *ctx), void *ctx)
{
    EngDir d;
    int i, stop = 0;
    if (depth > 16) return 0;
    if (eng_listdir(dir, &d) != 0) return 0;
    stop = fn(dir, &d, ctx);
    for (i = 0; i < d.n && !stop; i++) {
        char sub[ENG_PATH];
        vc_join(sub, sizeof sub, dir, d.names[i]);
        if (sub[0] && eng_isdir(sub)) stop = vc_walk(sub, depth + 1, fn, ctx);
    }
    eng_dir_free(&d);
    return stop;
}

/* First path for each wanted name across the roots, `rom.find` /
 * `macintalk2.find` style. */
typedef struct { const char *const *names; int n; char (*paths)[ENG_PATH]; } VcFirst;

static int vc_first_fn(const char *dir, const EngDir *listing, void *ctx)
{
    VcFirst *f = (VcFirst *)ctx;
    int i, k;
    for (i = 0; i < listing->n; i++)
        for (k = 0; k < f->n; k++)
            if (!f->paths[k][0] && strcmp(listing->names[i], f->names[k]) == 0)
                vc_join(f->paths[k], ENG_PATH, dir, listing->names[i]);
    return 0;
}

/* engine_installed: some folder under some root holds every one of `need`.
 * All of them in ONE folder -- a Pro voice folder has a rsrcfork.bin of its
 * own, so accumulating across the tree declared half an engine present. */
typedef struct { const char *const *need; int found; } VcHave;

static int vc_have_fn(const char *dir, const EngDir *listing, void *ctx)
{
    VcHave *h = (VcHave *)ctx;
    int k, i;
    (void)dir;
    for (k = 0; h->need[k]; k++) {
        int got = 0;
        for (i = 0; i < listing->n; i++) if (strcmp(listing->names[i], h->need[k]) == 0) { got = 1; break; }
        if (!got) return 0;
    }
    h->found = 1;
    return 1;
}

static int vc_engine_installed(const char *creator)
{
    VcHave h;
    int r;
    h.found = 0;
    if (!strcmp(creator, "mtk2")) h.need = VC_ENGINE_FILES_MTK2;
    else if (!strcmp(creator, "gala")) h.need = VC_ENGINE_FILES_GALA;
    else if (!strcmp(creator, "mtk3")) h.need = VC_ENGINE_FILES_MTK3;
    else if (!strcmp(creator, "cami")) h.need = VC_ENGINE_FILES_CAMI;
    else return 0;
    for (r = 0; r < g_vc.nroots && !h.found; r++) vc_walk(g_vc.roots[r], 0, vc_have_fn, &h);
    return h.found;
}

/* ---- the VoiceDescription ---------------------------------------------------- */

static unsigned vc_u32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static int vc_s16(const unsigned char *p) { return (int)(short)(((unsigned)p[0] << 8) | p[1]); }

/* Parse a ttvd into `v`. -> 0, or -1 with `why` for the skipped list. */
static int vc_describe(const unsigned char *d, int n, VcVoice *v, char *why, size_t whycap)
{
    unsigned length;
    int gender;
    if (n < VC_VD_LEN) { snprintf(why, whycap, "ttvd is %d bytes, need at least %d", n, VC_VD_LEN); return -1; }
    length = vc_u32(d);
    if (length != VC_VD_LEN) { snprintf(why, whycap, "VoiceDescription.length is %u, expected %d", length, VC_VD_LEN); return -1; }
    memcpy(v->creator, d + 4, 4); v->creator[4] = 0;
    v->id = (int)vc_u32(d + 8);
    v->name_len = d[16] < 63 ? d[16] : 63;
    memcpy(v->name, d + 17, (size_t)v->name_len);
    v->name[v->name_len] = 0;
    gender = vc_s16(d + 336);
    v->gender = gender;
    v->language = vc_s16(d + 342);
    v->ttvi_id = v->ttvw_id = -1; v->has_ttvi = v->has_ttvw = 0;
    if (!strcmp(v->creator, "mtk2") && n - VC_VD_LEN >= 0x14) {
        /* The extension names the other two resources. */
        v->ttvi_id = vc_s16(d + VC_VD_LEN + 8);  v->has_ttvi = 1;
        v->ttvw_id = vc_s16(d + VC_VD_LEN + 0x12); v->has_ttvw = 1;
    } else if (!strcmp(v->creator, "mtk3") && n > VC_MTK3_WAVE_OFF + 2) {
        /* Nine of the nineteen name their wave here; the ten that need none
         * read 1. */
        int wave = vc_s16(d + VC_MTK3_WAVE_OFF);
        if (wave != VC_MTK3_NO_WAVE) { v->ttvw_id = wave; v->has_ttvw = 1; }
    }
    return 0;
}

static void vc_skip(const char *folder, const char *reason)
{
    if (g_vc.nskipped >= VC_MAX_SKIPPED) return;
    strncpy(g_vc.skipped[g_vc.nskipped].folder, folder, 255);
    strncpy(g_vc.skipped[g_vc.nskipped].reason, reason, 255);
    g_vc.nskipped++;
}

static const char *vc_engine_label(const char *creator)
{
    if (!strcmp(creator, "mtk2")) return "MacinTalk 2";
    if (!strcmp(creator, "mtk3")) return "MacinTalk 3";
    if (!strcmp(creator, "gala")) return "MacinTalk Pro";
    if (!strcmp(creator, "cami")) return "MacinTalk Pro (Spanish)";
    return "unknown";
}

static const char *vc_file(const VcVoice *v, const char *kind)
{
    int i;
    for (i = 0; i < v->nfiles; i++) if (!strcmp(v->files[i].name, kind)) return v->files[i].path;
    return NULL;
}

/* What a voice folder must hold before the voice can speak, as VOICE_PARTS
 * -- and for MacinTalk 3 the voice itself says whether it needs a wave.
 * -> 0 when complete, else `short` filled with what is missing. */
static int vc_incomplete(const VcVoice *v, char *shortlist, size_t cap)
{
    const char *need_files[3] = { NULL, NULL, NULL };
    const char *need_types[4] = { NULL, NULL, NULL, NULL };
    int i, n = 0;
    shortlist[0] = 0;
    if (!strcmp(v->creator, "gala") || !strcmp(v->creator, "cami")) {
        need_files[0] = "rsrcfork.bin"; need_files[1] = "resources.tsv";
        need_types[0] = "ttvd"; need_types[1] = "gtsv";
    } else if (!strcmp(v->creator, "mtk2")) {
        need_types[0] = "ttvd"; need_types[1] = "ttvi"; need_types[2] = "ttvw";
    } else if (!strcmp(v->creator, "mtk3")) {
        need_types[0] = "ttvd";
    }
#define VC_SHORT(s) do { if (n) strncat(shortlist, ", ", cap - strlen(shortlist) - 1); \
                         strncat(shortlist, (s), cap - strlen(shortlist) - 1); n++; } while (0)
    for (i = 0; need_files[i]; i++) {
        int have = !strcmp(need_files[i], "rsrcfork.bin") ? v->has_rsrcfork : v->has_index;
        if (!have) VC_SHORT(need_files[i]);
    }
    for (i = 0; need_types[i]; i++) if (!vc_file(v, need_types[i])) VC_SHORT(need_types[i]);
    if (!strcmp(v->creator, "mtk3") && v->has_ttvw) {
        char want[32]; const char *have = vc_file(v, "ttvw");
        size_t wl;
        sprintf(want, "ttvw_%d.bin", v->ttvw_id);
        wl = strlen(want);
        if (!have || strlen(have) < wl || strcmp(have + strlen(have) - wl, want) != 0) VC_SHORT(want);
    }
#undef VC_SHORT
    return n ? -1 : 0;
}

/* ---- the scan ------------------------------------------------------------------ */

static void vc_lower_ascii(const unsigned char *s, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++) out[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : (char)s[i];
    out[n] = 0;
}

static int vc_cmp_voice(const void *a, const void *b)
{
    const VcVoice *x = (const VcVoice *)a, *y = (const VcVoice *)b;
    char la[64], lb[64];
    vc_lower_ascii(x->name, x->name_len, la);
    vc_lower_ascii(y->name, y->name_len, lb);
    return strcmp(la, lb);
}

/* `installed(creator, speakable=True)`: every voice folder under
 * <root>/voices whose ttvd names `creator`, in name order, dropping the
 * ones whose engine is absent or whose own parts are.  Appends to
 * g_vc.voices and returns how many. */
static int vc_installed(const char *creator, int engine_ok)
{
    int r, first = g_vc.nvoices, i;
    char seen[VC_MAX_VOICES * 2][80];
    int nseen = 0;

    for (r = 0; r < g_vc.nroots; r++) {
        char base[ENG_PATH];
        EngDir d;
        vc_join(base, sizeof base, g_vc.roots[r], "voices");
        if (!base[0] || !eng_isdir(base) || eng_listdir(base, &d) != 0) continue;
        for (i = 0; i < d.n; i++) {
            char p[ENG_PATH], why[256];
            EngDir fl;
            VcVoice v;
            int k, dup = 0;
            unsigned char *data; int len;
            const char *ttvd;
            vc_join(p, sizeof p, base, d.names[i]);
            if (!p[0] || !eng_isdir(p)) continue;
            /* First root wins, the same precedence paths.find uses. */
            for (k = 0; k < nseen; k++) if (!strcmp(seen[k], d.names[i])) { dup = 1; break; }
            if (dup) continue;
            if (nseen < VC_MAX_VOICES * 2) strncpy(seen[nseen++], d.names[i], 79);

            memset(&v, 0, sizeof v);
            strncpy(v.folder, p, ENG_PATH - 1);
            if (eng_listdir(p, &fl) != 0) continue;
            for (k = 0; k < fl.n; k++) {
                char rtype[16]; int rid, j, slot = -1;
                const char *f = fl.names[k];
                if (eng_split_name(f, rtype, sizeof rtype, &rid) != 0) {
                    /* Only `<type>_<id>.bin` is a resource; the rest is `extra`. */
                    if (!strcmp(f, "rsrcfork.bin")) v.has_rsrcfork = 1;
                    if (!strcmp(f, "resources.tsv")) v.has_index = 1;
                    continue;
                }
                for (j = 0; j < v.nfiles; j++) if (!strcmp(v.files[j].name, rtype)) slot = j;
                if (slot < 0) { if (v.nfiles >= VC_MAX_FILES) continue; slot = v.nfiles++; }
                strncpy(v.files[slot].name, rtype, 63);
                vc_join(v.files[slot].path, ENG_PATH, p, f);   /* the last of a type wins */
            }
            eng_dir_free(&fl);
            ttvd = vc_file(&v, "ttvd");
            if (!ttvd) { vc_skip(d.names[i], "no ttvd"); continue; }
            if (eng_slurp(ttvd, &data, &len) != 0) { vc_skip(d.names[i], "cannot read ttvd"); continue; }
            if (vc_describe(data, len, &v, why, sizeof why) != 0) { free(data); vc_skip(d.names[i], why); continue; }
            free(data);
            if (strcmp(v.creator, creator) != 0) continue;
            if (!engine_ok) {
                snprintf(why, sizeof why, "%s is not installed", vc_engine_label(creator));
                vc_skip(d.names[i], why);
                continue;
            }
            {
                char shortlist[200];
                if (vc_incomplete(&v, shortlist, sizeof shortlist) != 0) {
                    snprintf(why, sizeof why, "incomplete extraction, missing %s", shortlist);
                    vc_skip(d.names[i], why);
                    continue;
                }
            }
            if (g_vc.nvoices >= VC_MAX_VOICES) continue;
            strcpy(v.kind, !strcmp(creator, "mtk2") ? "mtk2" : !strcmp(creator, "mtk3") ? "mtk3" : "gala");
            g_vc.voices[g_vc.nvoices++] = v;
        }
        eng_dir_free(&d);
    }
    if (g_vc.nvoices - first > 1)
        qsort(g_vc.voices + first, (size_t)(g_vc.nvoices - first), sizeof(VcVoice), vc_cmp_voice);
    return g_vc.nvoices - first;
}

static void vc_engine_dir(const char *sub, const char *code, char *out)
{
    int r;
    out[0] = 0;
    for (r = 0; r < g_vc.nroots; r++) {
        char d[ENG_PATH], f[ENG_PATH];
        vc_join(d, sizeof d, g_vc.roots[r], sub);
        if (!d[0] || !eng_isdir(d)) continue;
        vc_join(f, sizeof f, d, code);
        if (f[0] && eng_isfile(f)) { strcpy(out, d); return; }
    }
}

/* Scan the roots (one per line) and build the list. -> entries, or -1. */
OSP_API int osp_catalogue_scan(const char *roots)
{
    const char *p = roots;
    VcFirst f;
    int i;

    memset(&g_vc, 0, sizeof g_vc);
    while (p && *p && g_vc.nroots < VC_MAX_ROOTS) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && n < ENG_PATH) {
            memcpy(g_vc.roots[g_vc.nroots], p, n);
            g_vc.roots[g_vc.nroots][n] = 0;
            if (eng_isdir(g_vc.roots[g_vc.nroots])) g_vc.nroots++;
            else g_vc.roots[g_vc.nroots][0] = 0;
        }
        p = nl ? nl + 1 : NULL;
    }

    /* The 1984 engine: three files anywhere under the roots, first wins;
     * the dictionary optional.  Listed only when the rules are there too --
     * without them it speaks only phonemes, which no screen reader sends. */
    f.names = VC_SP_NAMES; f.n = 4; f.paths = g_vc.sp_files;
    for (i = 0; i < g_vc.nroots; i++) vc_walk(g_vc.roots[i], 0, vc_first_fn, &f);
    if (g_vc.sp_files[0][0] && g_vc.sp_files[1][0] && g_vc.sp_files[2][0]) {
        VcVoice *v;
        static const struct { const char *name; int hz; } sp[2] = { { "Male", 110 }, { "Female", 250 } };
        for (i = 0; i < 2 && g_vc.nvoices < VC_MAX_VOICES; i++) {
            v = &g_vc.voices[g_vc.nvoices++];
            memset(v, 0, sizeof *v);
            strcpy(v->kind, "sp"); strcpy(v->creator, "sp");
            v->id = sp[i].hz; v->hz = sp[i].hz;
            strcpy((char *)v->name, sp[i].name); v->name_len = (int)strlen(sp[i].name);
            v->gender = i == 0 ? 1 : 2;
        }
    }

    /* MacinTalk 2: both halves of Cecy and the tables, first found wins. */
    f.names = VC_MTK2_NAMES; f.n = 8; f.paths = g_vc.mtk2_files;
    for (i = 0; i < g_vc.nroots; i++) vc_walk(g_vc.roots[i], 0, vc_first_fn, &f);
    if (g_vc.mtk2_files[0][0] && g_vc.mtk2_files[1][0])
        vc_installed("mtk2", vc_engine_installed("mtk2"));

    /* MacinTalk 3: its folder, by name, holding its code. */
    vc_engine_dir("macintalk3", "ttvi_10.bin", g_vc.mtk3_dir);
    if (g_vc.mtk3_dir[0]) vc_installed("mtk3", vc_engine_installed("mtk3"));

    /* MacinTalk Pro, English then Spanish, each in its own folder. */
    vc_engine_dir("macintalkpro", "gtse_1.bin", g_vc.gala_dir);
    if (g_vc.gala_dir[0]) vc_installed("gala", vc_engine_installed("gala"));
    vc_engine_dir("macintalkespanol", "gtse_99.bin", g_vc.cami_dir);
    if (g_vc.cami_dir[0]) vc_installed("cami", vc_engine_installed("cami"));

    g_vc.scanned = 1;
    return g_vc.nvoices;
}

OSP_API int osp_catalogue_count(void) { return g_vc.scanned ? g_vc.nvoices : -1; }
OSP_API int osp_catalogue_skipped_count(void) { return g_vc.scanned ? g_vc.nskipped : -1; }

/* MacRoman to UTF-8, for names shown to people. */
static void vc_put_utf8(NumBuf *b, const unsigned char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned cp = s[i] < 0x80 ? s[i] : ENG_MACROMAN_HI[s[i] - 0x80];
        char u[4];
        if (cp < 0x80) { u[0] = (char)cp; nb_put(b, u, 1); }
        else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); nb_put(b, u, 2); }
        else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); nb_put(b, u, 3); }
    }
}

static int vc_deliver(NumBuf *b, char *out, int cap)
{
    int need;
    if (b->oom) { nb_free(b); return -1; }
    need = (int)b->len;
    if (out && cap > 0) {
        size_t n = b->len < (size_t)cap - 1 ? b->len : (size_t)cap - 1;
        memcpy(out, b->p, n);
        out[n] = 0;
    }
    nb_free(b);
    return need;
}

/* One entry as NVDA's list wants it, tab-separated, UTF-8:
 *   id \t label \t kind \t creator \t voice id \t name \t language \t gender \t folder
 * where `id` is what NVDA persists (`male`, `mtk2:Ben`, `cami:Carlos`) and
 * `language` is `es` for the Spanish Pro voices and `en` otherwise.  Returns
 * the size needed, as the text calls do. */
OSP_API int osp_catalogue_entry(int i, char *out, int cap)
{
    NumBuf b;
    const VcVoice *v;
    char num[32];
    if (!g_vc.scanned || i < 0 || i >= g_vc.nvoices) return -2;
    v = &g_vc.voices[i];
    nb_init(&b);
    if (!strcmp(v->kind, "sp")) {
        nb_puts(&b, v->hz == 110 ? "male" : "female");
        nb_putc(&b, '\t');
        vc_put_utf8(&b, v->name, v->name_len);
        nb_puts(&b, " (MacinTalk 1)");
    } else {
        nb_puts(&b, v->creator); nb_putc(&b, ':');
        vc_put_utf8(&b, v->name, v->name_len);
        nb_putc(&b, '\t');
        vc_put_utf8(&b, v->name, v->name_len);
        nb_puts(&b, !strcmp(v->kind, "mtk2") ? " (MacinTalk 2)"
                  : !strcmp(v->kind, "mtk3") ? " (MacinTalk 3)" : " (MacinTalk Pro)");
    }
    nb_putc(&b, '\t'); nb_puts(&b, v->kind);
    nb_putc(&b, '\t'); nb_puts(&b, v->creator);
    sprintf(num, "\t%d\t", v->id); nb_puts(&b, num);
    vc_put_utf8(&b, v->name, v->name_len);
    nb_putc(&b, '\t'); nb_puts(&b, !strcmp(v->creator, "cami") ? "es" : "en");
    sprintf(num, "\t%d\t", v->gender); nb_puts(&b, num);
    nb_puts(&b, v->folder);
    return vc_deliver(&b, out, cap);
}

OSP_API int osp_catalogue_skipped(int i, char *out, int cap)
{
    NumBuf b;
    if (!g_vc.scanned || i < 0 || i >= g_vc.nskipped) return -2;
    nb_init(&b);
    nb_puts(&b, g_vc.skipped[i].folder);
    nb_putc(&b, '\t');
    nb_puts(&b, g_vc.skipped[i].reason);
    return vc_deliver(&b, out, cap);
}

static void vc_put_voice(NumBuf *b, const VcVoice *v)
{
    char num[32];
    int k, j;
    const EngFile *sorted[VC_MAX_FILES];
    nb_puts(b, "voice="); nb_puts(b, v->creator);
    sprintf(num, "\t%d\t", v->id); nb_puts(b, num);
    vc_put_utf8(b, v->name, v->name_len);
    nb_putc(b, '\t'); nb_puts(b, v->folder); nb_putc(b, '\n');
    /* sorted(v.files.items()): by resource type */
    for (k = 0; k < v->nfiles; k++) sorted[k] = &v->files[k];
    for (k = 1; k < v->nfiles; k++) {
        const EngFile *t = sorted[k];
        for (j = k; j > 0 && strcmp(sorted[j - 1]->name, t->name) > 0; j--) sorted[j] = sorted[j - 1];
        sorted[j] = t;
    }
    for (k = 0; k < v->nfiles; k++) {
        nb_puts(b, "vfile:"); nb_puts(b, sorted[k]->name); nb_putc(b, '=');
        nb_puts(b, sorted[k]->path); nb_putc(b, '\n');
    }
}

/* The manifest that opens entry `i`'s engine with that voice selected --
 * the same text the Python oracle builds, so the two can be diffed. */
OSP_API int osp_catalogue_manifest(int i, char *out, int cap)
{
    NumBuf b;
    const VcVoice *v;
    char num[32];
    int k;
    if (!g_vc.scanned || i < 0 || i >= g_vc.nvoices) return -2;
    v = &g_vc.voices[i];
    nb_init(&b);
    if (!strcmp(v->kind, "sp")) {
        /* file: lines sorted by name: DICT, DRVR, RULZ, TALK */
        static const int order[4] = { 3, 0, 2, 1 };
        nb_puts(&b, "engine=sp\n");
        for (k = 0; k < 4; k++) {
            int idx = order[k];
            if (!g_vc.sp_files[idx][0]) continue;
            nb_puts(&b, "file:"); nb_puts(&b, VC_SP_NAMES[idx]); nb_putc(&b, '=');
            nb_puts(&b, g_vc.sp_files[idx]); nb_putc(&b, '\n');
        }
        nb_puts(&b, "voice=sp\t110\tMale\t\nvoice=sp\t250\tFemale\t\n");
        sprintf(num, "select=sp\t%d\n", v->hz); nb_puts(&b, num);
    } else if (!strcmp(v->kind, "mtk2")) {
        /* sorted by name: Cecy_1, Cecy_3, ttop_1, ttph_1, ttsd_1, ttsd_2, ttsr_1, ttss_0 */
        static const int order[8] = { 0, 1, 7, 6, 3, 4, 2, 5 };
        nb_puts(&b, "engine=mtk2\n");
        for (k = 0; k < 8; k++) {
            int idx = order[k];
            if (!g_vc.mtk2_files[idx][0]) continue;
            nb_puts(&b, "file:"); nb_puts(&b, VC_MTK2_NAMES[idx]); nb_putc(&b, '=');
            nb_puts(&b, g_vc.mtk2_files[idx]); nb_putc(&b, '\n');
        }
        for (k = 0; k < g_vc.nvoices; k++)
            if (!strcmp(g_vc.voices[k].kind, "mtk2")) vc_put_voice(&b, &g_vc.voices[k]);
        nb_puts(&b, "select="); nb_puts(&b, v->creator);
        sprintf(num, "\t%d\n", v->id); nb_puts(&b, num);
    } else if (!strcmp(v->kind, "mtk3")) {
        nb_puts(&b, "engine=mtk3\nfolder="); nb_puts(&b, g_vc.mtk3_dir); nb_putc(&b, '\n');
        for (k = 0; k < g_vc.nvoices; k++)
            if (!strcmp(g_vc.voices[k].kind, "mtk3")) vc_put_voice(&b, &g_vc.voices[k]);
        nb_puts(&b, "select="); nb_puts(&b, v->creator);
        sprintf(num, "\t%d\n", v->id); nb_puts(&b, num);
    } else {
        nb_puts(&b, "engine=pro\nfolder=");
        nb_puts(&b, !strcmp(v->creator, "cami") ? g_vc.cami_dir : g_vc.gala_dir); nb_putc(&b, '\n');
        vc_put_voice(&b, v);
        nb_puts(&b, "select="); nb_puts(&b, v->creator);
        sprintf(num, "\t%d\n", v->id); nb_puts(&b, num);
    }
    return vc_deliver(&b, out, cap);
}
