/* osp_roots.c -- where the engine data may be, as the add-on decides it.
 *
 * A port of `rom.search_roots()` minus `migrate()`, which moves an old
 * folder into place and is NVDA's business.  The list is the same and in the
 * same order, because the first mention of a folder is the one that decides
 * which copy of a file wins, and it is shown to people, so it is deduplicated
 * the way the Python deduplicates it.  `tools/roots_oracle.py` diffs the two.
 *
 * The catalogue never discovers roots -- a caller hands them in -- and this
 * is the one caller that computes them for the others: the serve host, which
 * is launched with the NVDA configuration path the Python serve was launched
 * with and has to look in every place the driver looks.
 */

#define ROOTS_MAX 24

typedef struct { char list[ROOTS_MAX][ENG_PATH]; int n; } RootList;

static void roots_add(RootList *r, const char *path)
{
    if (r->n >= ROOTS_MAX || !path || !*path) return;
    strncpy(r->list[r->n], path, ENG_PATH - 1);
    r->list[r->n][ENG_PATH - 1] = 0;
    r->n++;
}

static void roots_join(RootList *r, const char *a, const char *b)
{
    char p[ENG_PATH];
    vc_join(p, sizeof p, a, b);
    if (p[0]) roots_add(r, p);
}

/* The roots for a configuration folder (NVDA's `configPath`, or whatever the
 * serve was pointed at) and an add-on folder (which holds a `rom` of its own
 * in the repository and the SAPI staging), one per line, deduplicated.
 * Size-needed contract. */
OSP_API int osp_roots_default(const char *config_path, const char *addon_root, char *out, int cap)
{
    RootList raw, uniq;
    char base[ENG_PATH], p[ENG_PATH], env[ENG_PATH];
    int i, j;
    NumBuf b;
    char keys[ROOTS_MAX][ENG_PATH];

    memset(&raw, 0, sizeof raw);
    memset(&uniq, 0, sizeof uniq);

    /* config_dir: <config>/macintalk/outspoken; config_base is <config>. */
    if (config_path && *config_path) strncpy(base, config_path, ENG_PATH - 1); else base[0] = 0;
    base[ENG_PATH - 1] = 0;
    vc_join(p, sizeof p, base, "macintalk");
    if (p[0]) roots_join(&raw, p, "outspoken");
    roots_join(&raw, base, "outspoken-roms");                 /* legacy_dir */
    /* The pointer file: a text file naming the folder, for anyone keeping it
     * elsewhere. */
    vc_join(p, sizeof p, base, "outspoken-roms.txt");
    if (p[0]) {
        unsigned char *data; int len;
        if (eng_slurp(p, &data, &len) == 0) {
            const unsigned char *s = data; int n = len;
            eng_strip(&s, &n);
            if (n > 0 && n < ENG_PATH) { char named[ENG_PATH]; memcpy(named, s, (size_t)n); named[n] = 0; roots_add(&raw, named); }
            free(data);
        }
    }
    if (addon_root && *addon_root) roots_join(&raw, addon_root, "rom");
    /* The SAPI driver's world: the folder its register step remembered in
     * HKCU, then the machine-wide DataPath from BOTH registry views. */
    if (osp_plat_registry_string(0, 0, "Software\\outSPOKEN SAPI", "DataPath", env, sizeof env)) roots_add(&raw, env);
    if (osp_plat_registry_string(1, 64, "Software\\outSPOKEN SAPI", "DataPath", env, sizeof env)) roots_add(&raw, env);
    if (osp_plat_registry_string(1, 32, "Software\\outSPOKEN SAPI", "DataPath", env, sizeof env)) roots_add(&raw, env);
    /* %ProgramData%\macintalk\outspoken, the machine-wide twin. */
    if (osp_plat_env("ProgramData", env, sizeof env) || osp_plat_env("ALLUSERSPROFILE", env, sizeof env)) {
        vc_join(p, sizeof p, env, "macintalk");
        if (p[0]) roots_join(&raw, p, "outspoken");
    }
    if (osp_plat_env("APPDATA", env, sizeof env)) {
        vc_join(p, sizeof p, env, "macintalk");
        if (p[0]) roots_join(&raw, p, "outspoken");
        roots_join(&raw, env, "outspoken-data");
    }

    /* Deduplicated, because this list is shown to people; order preserved. */
    for (i = 0; i < raw.n; i++) {
        char key[ENG_PATH];
        int dup = 0;
        if (!osp_plat_normkey(raw.list[i], key, sizeof key)) strncpy(key, raw.list[i], sizeof key - 1);
        for (j = 0; j < uniq.n; j++) if (!strcmp(keys[j], key)) { dup = 1; break; }
        if (dup) continue;
        strncpy(keys[uniq.n], key, ENG_PATH - 1);
        roots_add(&uniq, raw.list[i]);
    }

    nb_init(&b);
    for (i = 0; i < uniq.n; i++) { nb_puts(&b, uniq.list[i]); nb_putc(&b, '\n'); }
    return vc_deliver(&b, out, cap);
}
