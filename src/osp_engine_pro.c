/* osp_engine_pro.c -- MacinTalk Pro (Apple, 1993), English and Mexican
 * Spanish, driven from C.
 *
 * A port of `macintalkpro.py`.  Pro is a `ttsc` component like MacinTalk 2
 * with the identical selector map, so the glue carries over; what does not
 * is everything about how it finds its data, and each of these was earned:
 * it requires a 68020 and runs here as a 68040; it is addressed by NAME, so
 * an extraction without names is an engine that cannot start; it reads its
 * own files -- the lexicon from the data fork, a voice's units from the
 * voice file's resource fork, by walking the map and seeking -- so both
 * forks are registered; and the same type and id mean different things in
 * different files, so every resource is tagged with the file it came from.
 * It also waits on the clock, so Ticks advance on their own.
 *
 * Two synthesisers, one engine here: English Pro (`gala`, `gtse 1`) and the
 * Spanish engine (`cami`, `gtse 99`, no data fork, numbers in Spanish),
 * told apart by the voice's creator.  One voice per instance: the host
 * holds 64 resources and Pro is 50 with a voice another ten.
 *
 * Every address, selector, budget and floor here is the Python's.
 */

#define PRO_CODE        0x00040000u
#define PRO_HEAP        0x00080000u
#define PRO_HEAP_SIZE   0x00A00000u          /* 10 MB, ends at 0x00A80000 */
#define PRO_STACK       0x00C00000u
#define PRO_TEXT_BUF    0x00C10000u
#define PRO_VOICE_SPEC  0x00C20000u
#define PRO_STATUS_BUF  0x00C20100u
#define PRO_PARAM_BUF   0x00C20200u

#define PRO_CPUFLAG 0x012Fu
#define PRO_RESERR  0x0A60u
#define PRO_MEMERR  0x0220u

/* Below a threshold that belongs to the voice -- 0.05 for Bruce -- a 'pmod'
 * loops forever inside SpeakBuffer; the floor is ten times the worst. */
#define PRO_INFLECTION_FLOOR     1.0
#define PRO_INFLECTION_REFERENCE 25.0
#define PRO_MAX_BUFFERS          1250

typedef struct { char rtype[16]; int rid; int entry; char name[256]; } ProIndexRow;
typedef struct { ProIndexRow *rows; int n; } ProIndex;

static struct {
    unsigned chan;
    unsigned creator;
    int id;
    int spanish;
    int have_base_pitch;  double base_pitch;
    int have_base_mod;    double base_mod;
} g_pro;

/* `resources.tsv` beside the binaries: type, id, map entry, name -- a Mac
 * resource name is not a file name.  As `read_index`: a comment or a line
 * without a tab is skipped; any row that will not parse discards the whole
 * index, because the Python catches ValueError around the file. */
static void pro_read_index(const char *folder, ProIndex *ix)
{
    char path[ENG_PATH + 32];
    unsigned char *data; int len, cap = 0, bad = 0;
    const char *p, *end;
    memset(ix, 0, sizeof *ix);
    sprintf(path, "%s/resources.tsv", folder);
    if (eng_slurp(path, &data, &len) != 0) return;
    p = (const char *)data; end = p + len;
    while (p < end && !bad) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *f[4]; size_t fl[4];
        const char *q = p; int k;
        if (le > p && le[-1] == '\r') le--;
        if (p < le && *p == '#') { p = nl ? nl + 1 : end; continue; }
        if (!memchr(p, '\t', (size_t)(le - p))) { p = nl ? nl + 1 : end; continue; }
        for (k = 0; k < 4; k++) {
            const char *t = k < 3 ? memchr(q, '\t', (size_t)(le - q)) : NULL;
            f[k] = q; fl[k] = (size_t)((t ? t : le) - q);
            if (k < 3 && !t) { bad = 1; break; }         /* fewer than four fields */
            q = t ? t + 1 : le;
        }
        if (bad) break;
        if (ix->n >= cap) {
            int want = cap ? cap * 2 : 64;
            ProIndexRow *r = (ProIndexRow *)realloc(ix->rows, sizeof(ProIndexRow) * (size_t)want);
            if (!r) { bad = 1; break; }
            ix->rows = r; cap = want;
        }
        {
            ProIndexRow *r = &ix->rows[ix->n];
            char num[32]; char *e;
            memset(r, 0, sizeof *r);
            if (fl[0] >= sizeof r->rtype || fl[3] >= sizeof r->name || fl[1] >= sizeof num || fl[2] >= sizeof num) { bad = 1; break; }
            memcpy(r->rtype, f[0], fl[0]);
            memcpy(num, f[1], fl[1]); num[fl[1]] = 0;
            r->rid = (int)strtol(num, &e, 10); if (e == num || *e) { bad = 1; break; }
            memcpy(num, f[2], fl[2]); num[fl[2]] = 0;
            r->entry = (int)strtol(num, &e, 10); if (e == num || *e) { bad = 1; break; }
            memcpy(r->name, f[3], fl[3]);
            ix->n++;
        }
        p = nl ? nl + 1 : end;
    }
    free(data);
    if (bad) { free(ix->rows); memset(ix, 0, sizeof *ix); }
}

static const ProIndexRow *pro_index_find(const ProIndex *ix, const char *rtype, int rid)
{
    int i;
    for (i = 0; i < ix->n; i++)
        if (ix->rows[i].rid == rid && strcmp(ix->rows[i].rtype, rtype) == 0) return &ix->rows[i];
    return NULL;
}

static int pro_set_info(unsigned selector, unsigned arg)
{
    unsigned args[2], result = 0;
    int reason;
    args[0] = selector; args[1] = arg;
    reason = osp_component_call(g_pro.chan, M2_SET_INFO, args, 2, 200000000LL, &result);
    return reason == OSP_STOP_SENTINEL ? eng_signed(result) : -1;
}

static unsigned pro_fixed_arg(double value)
{
    osp_w32(PRO_PARAM_BUF, eng_fixed(value));
    return PRO_PARAM_BUF;
}

static int pro_get_fixed(unsigned selector, double *value)
{
    unsigned args[2], result = 0;
    int reason;
    osp_w32(PRO_PARAM_BUF, 0);
    args[0] = selector; args[1] = PRO_PARAM_BUF;
    reason = osp_component_call(g_pro.chan, M2_GET_INFO, args, 2, 200000000LL, &result);
    if (reason != OSP_STOP_SENTINEL || result != 0) return 0;
    *value = eng_unfixed(osp_r32(PRO_PARAM_BUF));
    return 1;
}

/* Only the voice this instance was built with can be selected. */
static int pro_select(const char *creator, int id)
{
    if (eng_ostype(creator) != g_pro.creator || id != g_pro.id) return 0;
    osp_w32(PRO_VOICE_SPEC, g_pro.creator);
    osp_w32(PRO_VOICE_SPEC + 4, (unsigned)id);
    return pro_set_info(SO_CURRENT_VOICE, PRO_VOICE_SPEC) == 0;
}

/* One of the two files Pro opens: its Mac name and both forks. -> 0 or -1 */
static int pro_add_file(const char *folder, const char *macname_utf8, int *index_out)
{
    unsigned char *dat = NULL, *rf = NULL; int dlen = 0, rlen = 0, r;
    unsigned char mac[64]; int maclen;
    char path[ENG_PATH + 32];
    sprintf(path, "%s/datafork.bin", folder);
    if (eng_slurp(path, &dat, &dlen) != 0) { dat = NULL; dlen = 0; }
    sprintf(path, "%s/rsrcfork.bin", folder);
    if (eng_slurp(path, &rf, &rlen) != 0) { rf = NULL; rlen = 0; }
    if (!dlen && !rlen) { free(dat); free(rf); eng_fail("a Pro folder has neither fork -- re-run tools/extract_rom.py"); return -1; }
    maclen = eng_macroman(macname_utf8, mac, (int)sizeof mac);
    if (maclen > 63) { free(dat); free(rf); eng_fail("a Mac file name is at most 63 characters"); return -1; }
    r = osp_add_file(mac, maclen, dat ? dat : (const unsigned char *)"", dlen,
                     rf ? rf : (const unsigned char *)"", rlen);
    free(dat); free(rf);
    if (r < 0) { eng_fail("could not register the file"); return -1; }
    *index_out = r;
    return 0;
}

static int pro_open(const EngManifest *m)
{
    const EngVoice *v;
    const char *code_name, *manuf;
    unsigned char *data; int len, i, k;
    unsigned args[1], result = 0;
    int comp, named = 0, ttvd_id = -1, fidx[2];
    const char *folders[2]; const char *macnames[2];
    char path[ENG_PATH + 300];

    memset(&g_pro, 0, sizeof g_pro);
    if (!m->nvoices) { eng_fail("manifest names no voice"); return ENG_ERR_MANIFEST; }
    if (!m->folder[0]) { eng_fail("manifest names no engine folder"); return ENG_ERR_MANIFEST; }
    v = &m->voices[0];
    for (i = 0; i < m->nvoices; i++)
        if (strcmp(m->voices[i].creator, m->sel_creator) == 0 && m->voices[i].id == m->sel_id) { v = &m->voices[i]; break; }
    if (!v->folder[0]) { eng_fail("the voice has no folder"); return ENG_ERR_MANIFEST; }

    /* English (`gala`) or Spanish (`cami`): the entry code and the component
     * manufacturer, and nothing else. */
    if (strcmp(v->creator, "cami") == 0) { code_name = "gtse_99.bin"; manuf = "cami"; g_pro.spanish = 1; }
    else { code_name = "gtse_1.bin"; manuf = "gala"; }
    g_pro.creator = eng_ostype(v->creator);
    g_pro.id = v->id;

    /* Before anything is loaded: Open reads Gestalt('proc') and refuses a
     * 68000 or a 68010, and the synthesis modules use F-line instructions.
     * Pro waits on the clock, so Ticks advance on their own -- off for the
     * other engines on purpose. */
    osp_set_cpu(OSP_CPU_68040);
    osp_auto_ticks(1);
    sprintf(path, "%s/%s", m->folder, code_name);
    if (eng_slurp(path, &data, &len) != 0) { eng_fail("cannot read the engine's entry code"); return ENG_ERR_FILE; }
    if (osp_write_block(PRO_CODE, data, len) != 0) { free(data); eng_fail("engine code does not fit"); return ENG_ERR_OPEN; }
    free(data);
    osp_heap_init(PRO_HEAP, PRO_HEAP_SIZE);
    osp_enable_mem_traps(1);
    osp_w8(PRO_CPUFLAG, 0);
    osp_w16(PRO_RESERR, 0);
    osp_w16(PRO_MEMERR, 0);

    /* The forks first, because a resource is tagged with the file it came
     * from and the file has to exist to be tagged with. */
    folders[0] = m->folder;  macnames[0] = "MacinTalk Pro";
    folders[1] = v->folder;  macnames[1] = v->name;
    for (k = 0; k < 2; k++)
        if (pro_add_file(folders[k], macnames[k], &fidx[k]) != 0) return ENG_ERR_OPEN;

    for (k = 0; k < 2; k++) {
        ProIndex ix;
        EngDir dir;
        pro_read_index(folders[k], &ix);
        if (eng_listdir(folders[k], &dir) != 0) { free(ix.rows); eng_fail("cannot list a Pro folder"); return ENG_ERR_FILE; }
        for (i = 0; i < dir.n; i++) {
            const char *name = dir.names[i];
            char rtype[16]; int rid; unsigned hnd;
            const ProIndexRow *row;
            if (!strcmp(name, "datafork.bin") || !strcmp(name, "rsrcfork.bin")
                || !strcmp(name, "resources.tsv") || !strcmp(name, "names.tsv")) continue;
            if (eng_split_name(name, rtype, sizeof rtype, &rid) != 0) continue;
            if (strcmp(rtype, "thng") == 0) continue;       /* the Component Manager's own */
            sprintf(path, "%s/%s", folders[k], name);
            if (eng_slurp(path, &data, &len) != 0) { eng_dir_free(&dir); free(ix.rows); eng_fail("cannot read a Pro resource"); return ENG_ERR_FILE; }
            hnd = osp_add_resource(eng_ostype(rtype), rid, data, len, fidx[k]);
            free(data);
            if (!hnd) { eng_dir_free(&dir); free(ix.rows); eng_fail("a Pro resource would not register"); return ENG_ERR_OPEN; }
            /* Pro finds its modules by name and asks RsrcMapEntry where each
             * sits before reading it out of the file; both ride on the Handle. */
            row = pro_index_find(&ix, rtype, rid);
            if (row && row->entry) osp_map_entry(hnd, row->entry);
            if (row && row->name[0]) {
                unsigned char mac[300]; int maclen = eng_macroman(row->name, mac, (int)sizeof mac);
                if (maclen <= 63 && osp_name_resource(hnd, (const char *)mac, maclen) == 0) named++;
            }
            if (k == 1 && strcmp(rtype, "ttvd") == 0) ttvd_id = rid;
        }
        eng_dir_free(&dir);
        free(ix.rows);
    }
    if (!named) { eng_fail("no resource names -- re-run tools/extract_rom.py; MacinTalk Pro finds its modules by name"); return ENG_ERR_OPEN; }
    if (ttvd_id < 0) { eng_fail("the voice has no ttvd"); return ENG_ERR_OPEN; }

    /* Which file the voice lives in, and it matters: Pro asks the Speech
     * Manager for the voice's FSSpec and then OPENS it. */
    if (osp_add_voice(g_pro.creator, (unsigned)g_pro.id, ttvd_id, fidx[1]) < 0) { eng_fail("no room for the voice"); return ENG_ERR_OPEN; }

    /* thng 128: type 'ttsc', subtype 0, the manufacturer the descriptor is
     * opened against. */
    comp = osp_add_component(eng_ostype("ttsc"), 0, eng_ostype(manuf), PRO_CODE);
    if (comp < 0) { eng_fail("no room for the component"); return ENG_ERR_OPEN; }
    g_pro.chan = osp_open_instance(comp);
    if (!g_pro.chan) { eng_fail("cannot open the component instance"); return ENG_ERR_OPEN; }

    osp_set_reg(OSP_REG_A7, PRO_STACK);
    osp_set_reg(OSP_REG_SR, 0x2700u);
    osp_defer_callbacks(1);

    args[0] = g_pro.chan;
    if (osp_component_call(g_pro.chan, M2_OPEN, args, 1, 200000000LL, &result) != OSP_STOP_SENTINEL
        || result != 0) {
        eng_fail("MacinTalk Pro Open failed");
        return ENG_ERR_OPEN;
    }
    if (!pro_select(v->creator, v->id)) { eng_fail("MacinTalk Pro would not take the voice"); return ENG_ERR_OPEN; }
    return 0;
}

static void pro_close(void)
{
    unsigned result = 0;
    if (g_pro.chan) osp_component_call(g_pro.chan, M2_CLOSE, NULL, 0, 20000000LL, &result);
    g_pro.chan = 0;
}

static void pro_set_rate(int rate)
{
    pro_set_info(SO_RATE, pro_fixed_arg((double)rate));
}

/* The result code of 'pbas' is not one: Pro answers with the frequency it
 * just computed, truncated to sixteen bits, and the value takes perfectly.
 * So the call is made and its answer ignored. */
static void pro_set_pitch(int tenths)
{
    unsigned args[2], result = 0;
    if (!g_pro.have_base_pitch) {
        if (!pro_get_fixed(SO_PITCH_BASE, &g_pro.base_pitch)) return;
        g_pro.have_base_pitch = 1;
    }
    args[0] = SO_PITCH_BASE; args[1] = pro_fixed_arg(g_pro.base_pitch + tenths / 10.0);
    osp_component_call(g_pro.chan, M2_SET_INFO, args, 2, 200000000LL, &result);
}

static void pro_set_inflection(int percent)
{
    double value;
    if (!g_pro.have_base_mod) {
        if (!pro_get_fixed(SO_PITCH_MOD, &g_pro.base_mod)) return;
        g_pro.have_base_mod = 1;
    }
    if (g_pro.base_mod > 0) value = g_pro.base_mod * percent / 50.0;
    else value = PRO_INFLECTION_REFERENCE * (percent - 50 > 0 ? percent - 50 : 0) / 50.0;
    if (value < PRO_INFLECTION_FLOOR) value = PRO_INFLECTION_FLOOR;
    if (value > 100.0) value = 100.0;
    pro_set_info(SO_PITCH_MOD, pro_fixed_arg(value));
}

static void pro_translate(const unsigned char *text, int len, NumBuf *out)
{
    eng_translate_sm(text, len, g_pro.spanish, out);
}

static int pro_busy(void)
{
    unsigned args[1], result = 0;
    osp_w32(PRO_STATUS_BUF, 0); osp_w32(PRO_STATUS_BUF + 4, 0); osp_w32(PRO_STATUS_BUF + 8, 0);
    args[0] = PRO_STATUS_BUF;
    if (osp_component_call(g_pro.chan, M2_STATUS, args, 1, 20000000LL, &result) != OSP_STOP_SENTINEL)
        return 0;
    return osp_r8(PRO_STATUS_BUF) != 0 || osp_r32(PRO_STATUS_BUF + 2) != 0;
}

static int pro_begin(const unsigned char *text, int len)
{
    const unsigned char *raw = text;
    int n = len;
    unsigned args[3], result = 0;

    eng_strip(&raw, &n);
    if (n <= 0) return 0;
    osp_pcm_reset();
    osp_write_block(PRO_TEXT_BUF, raw, n);
    args[0] = PRO_TEXT_BUF; args[1] = (unsigned)n; args[2] = 0;
    return osp_component_call(g_pro.chan, M2_SPEAK, args, 3, 400000000LL, &result) == OSP_STOP_SENTINEL;
}

static int pro_pump(NumBuf *out)
{
    if (osp_buffers_taken() >= PRO_MAX_BUFFERS) return 0;
    if (!osp_run_callbacks(8, 200000000LL)) return 0;
    if (osp_pcm_len()) { eng_take_pcm(out); osp_pcm_reset(); }
    return pro_busy();
}

static void pro_quiet(void)
{
    unsigned args[1] = { 0 }, result = 0;
    osp_component_call(g_pro.chan, M2_STOP, args, 1, 20000000LL, &result);
}

static void pro_drain(void)
{
    osp_run_callbacks(64, 200000000LL);
    osp_pcm_reset();
}

static void pro_speak(const unsigned char *text, int len, NumBuf *out)
{
    if (!pro_begin(text, len)) return;
    while (pro_pump(out)) { }
    eng_take_pcm(out);
    pro_drain();
    if (out->oom) return;
    eng_trim(out, 1200, 220);
}

static void pro_stop(void) { }

static const EngOps ENG_PRO_OPS = {
    pro_open, pro_close, pro_select, pro_set_rate, pro_set_pitch, NULL,
    pro_set_inflection, pro_translate, pro_speak, pro_stop,
    pro_begin, pro_pump, pro_quiet, pro_drain
};
