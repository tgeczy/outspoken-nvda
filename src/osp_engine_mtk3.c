/* osp_engine_mtk3.c -- classic MacinTalk 3 (Apple, 1994) driven from C.
 *
 * A port of `macintalk3.py`: the 1994 68k build, running as 68k.  The same
 * `ttsc` protocol as MacinTalk 2, with what differs worth reading there
 * before touching anything: it requires a 68040 (`ttvi 10 +0x234` is
 * `rtd`); the engine hides in `ttvi` 10, "Bach", with `ttvi` 11 the PowerPC
 * build that must never be registered; a voice is usually nothing but a
 * 714-byte parameter set, nine of the nineteen also carrying a `ttvw`; and
 * all nineteen fit at once, so switching is one 'cvox'.
 *
 * Every address, selector, budget and trim value here is the Python's.
 */

#define M3_CODE        0x00040000u
#define M3_HEAP        0x00080000u
#define M3_HEAP_SIZE   0x00200000u
#define M3_STACK       0x00400000u
#define M3_TEXT_BUF    0x00410000u
#define M3_VOICE_SPEC  0x00411000u
#define M3_STATUS_BUF  0x00412000u
#define M3_PARAM_BUF   0x00413000u

#define M3_CPUFLAG 0x012Fu
#define M3_RESERR  0x0A60u
#define M3_MEMERR  0x0220u

/* The depth given at the top of the slider to a voice that has none of its
 * own, in 'pmod' units: between Albert's 12.500 and Fred's 50.000. */
#define M3_INFLECTION_REFERENCE 25.0
#define M3_MAX_BUFFERS 700

/* The engine's own resources; `ttvi 11` is the PowerPC build. */
static const char *const M3_ENGINE_TYPES[5] = { "ttvi", "ttss", "ttsp", "STR ", "vers" };

static struct {
    unsigned chan;
    M2Voice voices[ENG_MAX_VOICES];
    int nvoices;
    int have_base_pitch;  double base_pitch;
    int have_base_mod;    double base_mod;
} g_m3;

static int m3_set_info(unsigned selector, unsigned arg)
{
    unsigned args[2], result = 0;
    int reason;
    args[0] = selector; args[1] = arg;
    reason = osp_component_call(g_m3.chan, M2_SET_INFO, args, 2, 50000000LL, &result);
    return reason == OSP_STOP_SENTINEL ? eng_signed(result) : -1;
}

static unsigned m3_fixed_arg(double value)
{
    osp_w32(M3_PARAM_BUF, eng_fixed(value));
    return M3_PARAM_BUF;
}

static int m3_get_fixed(unsigned selector, double *value)
{
    unsigned args[2], result = 0;
    int reason;
    osp_w32(M3_PARAM_BUF, 0);
    args[0] = selector; args[1] = M3_PARAM_BUF;
    reason = osp_component_call(g_m3.chan, M2_GET_INFO, args, 2, 50000000LL, &result);
    if (reason != OSP_STOP_SENTINEL || result != 0) return 0;
    *value = eng_unfixed(osp_r32(M3_PARAM_BUF));
    return 1;
}

static int m3_select(const char *creator, int id)
{
    osp_w32(M3_VOICE_SPEC, eng_ostype(creator));
    osp_w32(M3_VOICE_SPEC + 4, (unsigned)id);
    if (m3_set_info(SO_CURRENT_VOICE, M3_VOICE_SPEC) != 0) return 0;
    g_m3.have_base_pitch = 0;
    g_m3.have_base_mod = 0;
    return 1;
}

/* `ttvi_10.bin` -> ("ttvi", 10), as `_split`: the extension must be .bin,
 * the stem must hold an underscore, and what follows the LAST one must be an
 * integer.  -> 0, or -1 for anything else. */
static int eng_split_name(const char *name, char *rtype, size_t rcap, int *rid)
{
    const char *dot = strrchr(name, '.'), *us;
    size_t stem_len;
    char num[32];
    size_t n;
    if (!dot) return -1;
    if (!((dot[1] | 0x20) == 'b' && (dot[2] | 0x20) == 'i' && (dot[3] | 0x20) == 'n' && !dot[4])) return -1;
    stem_len = (size_t)(dot - name);
    us = NULL;
    for (n = 0; n < stem_len; n++) if (name[n] == '_') us = name + n;
    if (!us) return -1;
    if ((size_t)(us - name) >= rcap) return -1;
    memcpy(rtype, name, (size_t)(us - name));
    rtype[us - name] = 0;
    n = (size_t)(dot - us - 1);
    if (!n || n >= sizeof num) return -1;
    memcpy(num, us + 1, n);
    num[n] = 0;
    {
        char *end;
        long v = strtol(num, &end, 10);
        if (end == num || *end) return -1;
        *rid = (int)v;
    }
    return 0;
}

static int m3_open(const EngManifest *m)
{
    unsigned char *data; int len, i;
    EngDir dir;
    unsigned args[1], result = 0;
    int comp;
    char path[ENG_PATH + 300];

    memset(&g_m3, 0, sizeof g_m3);
    if (!m->folder[0]) { eng_fail("manifest names no engine folder"); return ENG_ERR_MANIFEST; }

    /* Not optional, and not a preference: `+0x234` is `rtd`, so a 68000
     * refuses to Open, and synthesis uses instructions a 68020 lacks too. */
    osp_set_cpu(OSP_CPU_68040);
    sprintf(path, "%s/ttvi_10.bin", m->folder);
    if (eng_slurp(path, &data, &len) != 0) { eng_fail("cannot read ttvi_10.bin"); return ENG_ERR_FILE; }
    if (osp_write_block(M3_CODE, data, len) != 0) { free(data); eng_fail("engine code does not fit"); return ENG_ERR_OPEN; }
    free(data);
    osp_heap_init(M3_HEAP, M3_HEAP_SIZE);
    osp_enable_mem_traps(1);
    osp_w8(M3_CPUFLAG, 0);
    osp_w16(M3_RESERR, 0);
    osp_w16(M3_MEMERR, 0);

    if (eng_listdir(m->folder, &dir) != 0) { eng_fail("cannot list the engine folder"); return ENG_ERR_FILE; }
    for (i = 0; i < dir.n; i++) {
        char rtype[16]; int rid, k, known = 0;
        if (eng_split_name(dir.names[i], rtype, sizeof rtype, &rid) != 0) continue;
        if (strcmp(rtype, "ttvi") == 0 && rid == 11) continue;        /* SKIP */
        for (k = 0; k < 5; k++) if (strcmp(rtype, M3_ENGINE_TYPES[k]) == 0) known = 1;
        if (!known) continue;
        sprintf(path, "%s/%s", m->folder, dir.names[i]);
        eng_add_resource_file(rtype, rid, path, -1);
    }
    eng_dir_free(&dir);

    /* Every voice's resources, under the ids its own ttvd asks for.  A voice
     * that will not load is dropped rather than fatal.  `vers` is metadata,
     * not a resource. */
    for (i = 0; i < m->nvoices; i++) {
        const EngVoice *v = &m->voices[i];
        int k, ttvd = -1, ok = 1;
        for (k = 0; k < v->nfiles && ok; k++) {
            int rid;
            if (strcmp(v->files[k].name, "ttvd") != 0 && strcmp(v->files[k].name, "ttvw") != 0) continue;
            if (eng_rid_from_name(v->files[k].path, &rid) != 0) { ok = 0; break; }
            if (!eng_add_resource_file(v->files[k].name, rid, v->files[k].path, -1)) { ok = 0; break; }
            if (strcmp(v->files[k].name, "ttvd") == 0) ttvd = rid;
        }
        if (!ok || ttvd < 0) continue;
        /* The VoiceSpec id and the resource id are not the same number --
         * Bubbles is VoiceSpec 50 in a resource numbered 12. */
        if (osp_add_voice(eng_ostype(v->creator), (unsigned)v->id, ttvd, -1) < 0) continue;
        g_m3.voices[g_m3.nvoices].creator = eng_ostype(v->creator);
        g_m3.voices[g_m3.nvoices].id = v->id;
        g_m3.voices[g_m3.nvoices].loaded = 1;
        g_m3.nvoices++;
    }
    if (!g_m3.nvoices) { eng_fail("no MacinTalk 3 voice could be loaded"); return ENG_ERR_OPEN; }

    comp = osp_add_component(eng_ostype("ttsc"), eng_ostype("mtk3"), eng_ostype("mtk3"), M3_CODE);
    if (comp < 0) { eng_fail("no room for the component"); return ENG_ERR_OPEN; }
    g_m3.chan = osp_open_instance(comp);
    if (!g_m3.chan) { eng_fail("cannot open the component instance"); return ENG_ERR_OPEN; }
    osp_set_reg(OSP_REG_A7, M3_STACK);
    osp_set_reg(OSP_REG_SR, 0x2700u);
    /* Not optional either: the engine's callback installs a deferred task
     * and queues the next command, and running callbacks inside the call runs
     * them against state SpeakBuffer has already moved past. */
    osp_defer_callbacks(1);

    args[0] = g_m3.chan;
    if (osp_component_call(g_m3.chan, M2_OPEN, args, 1, 50000000LL, &result) != OSP_STOP_SENTINEL
        || eng_signed(result) != 0) {
        eng_fail("MacinTalk 3 would not open");
        return ENG_ERR_OPEN;
    }
    {
        unsigned want = eng_ostype(m->sel_creator);
        int found = 0;
        char c4[5];
        for (i = 0; i < g_m3.nvoices; i++)
            if (g_m3.voices[i].creator == want && g_m3.voices[i].id == m->sel_id) { found = 1; break; }
        if (!found) i = 0;
        c4[0] = (char)(g_m3.voices[i].creator >> 24); c4[1] = (char)(g_m3.voices[i].creator >> 16);
        c4[2] = (char)(g_m3.voices[i].creator >> 8);  c4[3] = (char)g_m3.voices[i].creator; c4[4] = 0;
        if (!m3_select(c4, g_m3.voices[i].id)) { eng_fail("MacinTalk 3 would not take the voice"); return ENG_ERR_OPEN; }
    }
    return 0;
}

static void m3_close(void)
{
    unsigned result = 0;
    if (g_m3.chan) osp_component_call(g_m3.chan, M2_CLOSE, NULL, 0, 20000000LL, &result);
    g_m3.chan = 0;
}

static void m3_set_rate(int rate)
{
    m3_set_info(SO_RATE, m3_fixed_arg((double)rate));
}

static void m3_set_pitch(int tenths)
{
    if (!g_m3.have_base_pitch) {
        if (!m3_get_fixed(SO_PITCH_BASE, &g_m3.base_pitch)) return;
        g_m3.have_base_pitch = 1;
    }
    m3_set_info(SO_PITCH_BASE, m3_fixed_arg(g_m3.base_pitch + tenths / 10.0));
}

/* 'pmod' is a depth: half at 25, the voice's own at 50, twice at 100.  The
 * nine voices whose own is zero get an absolute depth above the midpoint. */
static void m3_set_inflection(int percent)
{
    double value;
    if (!g_m3.have_base_mod) {
        if (!m3_get_fixed(SO_PITCH_MOD, &g_m3.base_mod)) return;
        g_m3.have_base_mod = 1;
    }
    if (g_m3.base_mod > 0) value = g_m3.base_mod * percent / 50.0;
    else value = M3_INFLECTION_REFERENCE * (percent - 50 > 0 ? percent - 50 : 0) / 50.0;
    m3_set_info(SO_PITCH_MOD, m3_fixed_arg(value < 100.0 ? value : 100.0));
}

static void m3_translate(const unsigned char *text, int len, NumBuf *out)
{
    eng_translate_sm(text, len, 0, out);
}

static int m3_busy(void)
{
    unsigned args[1], result = 0;
    osp_w32(M3_STATUS_BUF, 0); osp_w32(M3_STATUS_BUF + 4, 0); osp_w32(M3_STATUS_BUF + 8, 0);
    args[0] = M3_STATUS_BUF;
    if (osp_component_call(g_m3.chan, M2_STATUS, args, 1, 20000000LL, &result) != OSP_STOP_SENTINEL)
        return 0;
    return osp_r8(M3_STATUS_BUF) != 0 || osp_r32(M3_STATUS_BUF + 2) != 0;
}

/* SpeakBuffer.  -> 1 when there is something to render, 0 for nothing. */
static int m3_begin(const unsigned char *text, int len)
{
    const unsigned char *raw = text;
    int n = len;
    unsigned args[3], result = 0;
    unsigned char *buf;

    eng_strip(&raw, &n);
    if (n <= 0) return 0;
    osp_pcm_reset();
    /* The engine reads its text buffer as a C string in places, so the
     * terminator goes in even though the length is passed too. */
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf) return 0;
    memcpy(buf, raw, (size_t)n);
    buf[n] = 0;
    osp_write_block(M3_TEXT_BUF, buf, n + 1);
    free(buf);
    args[0] = M3_TEXT_BUF; args[1] = (unsigned)n; args[2] = 0;
    return osp_component_call(g_m3.chan, M2_SPEAK, args, 3, 400000000LL, &result) == OSP_STOP_SENTINEL;
}

/* One round of being the Sound Manager: appends what landed, -> 1 while the
 * engine is still busy.  The same loop body as the Python's, so that the
 * blocking and streamed renders are one and the same. */
static int m3_pump(NumBuf *out)
{
    if (osp_buffers_taken() >= M3_MAX_BUFFERS) return 0;   /* the ceiling: a finding, not an utterance */
    if (!osp_run_callbacks(8, 200000000LL)) return 0;      /* nothing pending: really finished */
    if (osp_pcm_len()) { eng_take_pcm(out); osp_pcm_reset(); }
    return m3_busy();
}

/* StopSpeech(kImmediate), from the rendering thread only. */
static void m3_quiet(void)
{
    unsigned args[1] = { 0 }, result = 0;
    osp_component_call(g_m3.chan, M2_STOP, args, 1, 20000000LL, &result);
}

/* Drain whatever the engine still has queued and throw it away, so the
 * tail of this utterance cannot arrive at the front of the next one. */
static void m3_drain(void)
{
    osp_run_callbacks(64, 200000000LL);
    osp_pcm_reset();
}

static void m3_speak(const unsigned char *text, int len, NumBuf *out)
{
    if (!m3_begin(text, len)) return;
    while (m3_pump(out)) { }
    eng_take_pcm(out);
    m3_drain();
    if (out->oom) return;
    eng_trim(out, 1200, 220);                    /* ospaudio.trim: KEEP, LEAD */
}

static void m3_stop(void) { }

static const EngOps ENG_MTK3_OPS = {
    m3_open, m3_close, m3_select, m3_set_rate, m3_set_pitch, NULL,
    m3_set_inflection, m3_translate, m3_speak, m3_stop,
    m3_begin, m3_pump, m3_quiet, m3_drain
};
