/* osp_engine_mtk2.c -- MacinTalk 2 (Apple, 1992) driven from C.
 *
 * A port of `macintalk2.py`.  Two things genuinely differ from `.sp` and the
 * surface must not assume otherwise: there is no translation step, because
 * this engine ships its own front end and takes English; and speaking is
 * asynchronous -- SpeakBuffer renders one buffer and returns, and everything
 * after that arrives because the host keeps answering the Sound Manager.
 * Everything about the component protocol is in docs/macintalk2-components.md.
 *
 * Every address, selector, budget and trim value here is the Python's,
 * unchanged, and the render oracle holds this file to it byte for byte.
 */

#define M2_FRONT_BASE  0x00040000u
#define M2_BACK_BASE   0x00060000u
#define M2_HEAP        0x00080000u
#define M2_HEAP_SIZE   0x000E0000u
#define M2_STACK       0x00200000u
#define M2_TEXT_BUF    0x00195000u
#define M2_VOICE_SPEC  0x00196100u
#define M2_STATUS_BUF  0x00196200u
#define M2_PARAM_BUF   0x00196300u

#define M2_CPUFLAG 0x012Fu
#define M2_RESERR  0x0A60u
#define M2_MEMERR  0x0220u

/* Standard component selectors, from the -1..-6 table at Cecy 3 +$30, and
 * the component's own, identified from their handlers. */
#define M2_OPEN     (-1)
#define M2_CLOSE    (-2)
#define M2_STATUS   0
#define M2_SPEAK    1
#define M2_STOP     2
#define M2_GET_INFO 5
#define M2_SET_INFO 6

/* Speech Manager selectors, from Apple's Speech.h. */
#define SO_CURRENT_VOICE 0x63766F78u   /* 'cvox' */
#define SO_RATE          0x72617465u   /* 'rate' */
#define SO_PITCH_BASE    0x70626173u   /* 'pbas' */
#define SO_PITCH_MOD     0x706D6F64u   /* 'pmod' */

/* What a voice with no modulation of its own is given at the top of the
 * slider: RoboVox and Xero, above the midpoint only. */
#define M2_INFLECTION_REFERENCE 25.0

/* A ceiling on one utterance, in buffers: about 70 seconds here. */
#define M2_MAX_BUFFERS 800

#define M2_SILENT 0x80

static const struct { const char *type; int id; } M2_TABLES[6] = {
    { "ttsr", 1 }, { "ttsd", 1 }, { "ttsd", 2 }, { "ttss", 0 }, { "ttph", 1 }, { "ttop", 1 }
};

typedef struct { unsigned creator; int id; int loaded; } M2Voice;

static struct {
    unsigned chan;
    M2Voice voices[ENG_MAX_VOICES];
    int nvoices;
    int cur;                    /* index into voices, or -1 */
    /* Tenths of a semitone from the voice's own pitch, and that pitch as the
     * engine reports it.  The second is per-voice, so select drops it. */
    int have_base_pitch;  double base_pitch;
    int have_base_mod;    double base_mod;
} g_m2;

/* ---- helpers shared with MacinTalk 3 and Pro ----------------------------- */

static unsigned eng_ostype(const char *s)
{
    unsigned char b[4] = { ' ', ' ', ' ', ' ' };
    int i;
    for (i = 0; i < 4 && s[i]; i++) b[i] = (unsigned char)s[i];
    return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3];
}

/* A Fixed 16.16, as `_fixed`: int(round(x * 65536.0)) & 0xFFFFFFFF.  Python's
 * round() is half-to-even on the exact double, and so is nearbyint() under
 * the default rounding mode. */
static unsigned eng_fixed(double x)
{
    double r = nearbyint(x * 65536.0);
    long long v = (long long)r;
    return (unsigned)(v & 0xFFFFFFFFLL);
}
static double eng_unfixed(unsigned u)
{
    long long v = (long long)u;
    if (u & 0x80000000u) v -= 1LL << 32;
    return (double)v / 65536.0;
}
static int eng_signed(unsigned v) { return (int)v; }

/* The resource id in a file name of the extractor's shape: `ttvd_128.bin` is
 * 128 -- `int(basename.split("_")[1].split(".")[0])`.  -> 0, or -1 when the
 * name is not of that shape, which the Python raises on and skips. */
static int eng_rid_from_name(const char *path, int *rid)
{
    const char *base = path, *p, *us;
    char buf[32];
    size_t n = 0;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    us = strchr(base, '_');
    if (!us) return -1;
    for (p = us + 1; *p && *p != '.' && *p != '_'; p++) {
        if (n + 1 >= sizeof buf) return -1;
        buf[n++] = *p;
    }
    buf[n] = 0;
    if (!n) return -1;
    {
        char *end;
        long v = strtol(buf, &end, 10);
        if (*end || end == buf) return -1;
        *rid = (int)v;
    }
    return 0;
}

/* Register a file's bytes as a resource. -> the Handle, or 0 */
static unsigned eng_add_resource_file(const char *type, int rid, const char *path, int file_index)
{
    unsigned char *data; int len; unsigned h;
    if (eng_slurp(path, &data, &len) != 0) return 0;
    h = osp_add_resource(eng_ostype(type), rid, data, len, file_index);
    free(data);
    return h;
}

/* Sample counts of the bufferCmds taken from `since` onward, into `lengths`
 * (which the caller sizes); -> how many. */
static int eng_buflog_lengths(int since, unsigned *lengths, int cap)
{
    int i, n = 0, total = osp_buflog_n();
    for (i = since; i < total && n < cap; i++) {
        unsigned a, l;
        osp_buflog_get(i, &a, &l);
        lengths[n++] = l;
    }
    return n;
}

/* Take the PCM the host has collected. */
static void eng_take_pcm(NumBuf *out)
{
    unsigned pl = osp_pcm_len();
    int got;
    if (nb_reserve(out, pl + 1)) return;
    got = osp_pcm_get(out->p + out->len, (int)pl);
    if (got > 0) out->len += (size_t)got;
}

/* Drop a final buffer that merely restates the one before it.  MacinTalk 2
 * double-buffers, and when it finishes it can hand the Sound Manager the
 * *other* half again without refilling it; we take every bufferCmd offered,
 * so the last chunk of speech arrived twice.  Compare whole buffers against
 * every earlier one -- the restated buffer is the other half, two slots
 * back, with not always a silent buffer between.  Port of `_drop_restated`;
 * `pcm` is trimmed in place. */
static void eng_drop_restated(NumBuf *pcm, const unsigned *lengths, int nlen)
{
    size_t *lo, *hi;
    size_t off = 0;
    int i, end;
    if (nlen < 2) return;
    lo = (size_t *)malloc(sizeof(size_t) * (size_t)nlen * 2);
    if (!lo) return;
    hi = lo + nlen;
    for (i = 0; i < nlen; i++) {
        lo[i] = off < pcm->len ? off : pcm->len;
        off += lengths[i];
        hi[i] = off < pcm->len ? off : pcm->len;       /* a slice past the end is short */
    }
    end = nlen;
    for (;;) {
        int live_n = 0, last = -1, j, match = 0;
        for (i = 0; i < end; i++) {
            size_t k;
            int any = 0;
            for (k = lo[i]; k < hi[i]; k++) if (pcm->p[k] != M2_SILENT) { any = 1; break; }
            if (any) { live_n++; last = i; }
        }
        if (live_n < 2) break;
        for (j = 0; j < last; j++) {
            if (hi[j] - lo[j] == hi[last] - lo[last]
                && memcmp(pcm->p + lo[j], pcm->p + lo[last], hi[last] - lo[last]) == 0) {
                match = 1; break;
            }
        }
        if (!match) break;
        end = last;
    }
    if (end < nlen) pcm->len = hi[end - 1];
    free(lo);
}

/* Drop the silence at both ends, leaving a little at each: `keep` about
 * 50 ms at the end and `lead` about 10 ms at the start, because cutting hard
 * on a sample clicks.  Port of `_trim`, in place. */
static void eng_trim(NumBuf *pcm, size_t keep, size_t lead)
{
    size_t n = pcm->len, start = 0, end, from, to;
    if (!n) return;
    while (start < n && pcm->p[start] == M2_SILENT) start++;
    if (start >= n) { pcm->len = 0; return; }        /* nothing but silence */
    end = n;
    while (end > start && pcm->p[end - 1] == M2_SILENT) end--;
    from = start > lead ? start - lead : 0;
    to = end + keep < n ? end + keep : n;
    memmove(pcm->p, pcm->p + from, to - from);
    pcm->len = to - from;
}

/* ---- the engine ---------------------------------------------------------------- */

static int m2_set_info(unsigned selector, unsigned arg)
{
    unsigned args[2], result = 0;
    int reason;
    args[0] = selector; args[1] = arg;
    reason = osp_component_call(g_m2.chan, M2_SET_INFO, args, 2, 50000000LL, &result);
    return reason == OSP_STOP_SENTINEL ? eng_signed(result) : -1;
}

/* SetSpeechInfo takes a pointer for every selector, including the scalar
 * ones: put the Fixed somewhere and hand over its address. */
static unsigned m2_fixed_arg(double value)
{
    osp_w32(M2_PARAM_BUF, eng_fixed(value));
    return M2_PARAM_BUF;
}

/* GetSpeechInfo of a Fixed. -> 1 with *value, or 0 */
static int m2_get_fixed(unsigned selector, double *value)
{
    unsigned args[2], result = 0;
    int reason;
    osp_w32(M2_PARAM_BUF, 0);
    args[0] = selector; args[1] = M2_PARAM_BUF;
    reason = osp_component_call(g_m2.chan, M2_GET_INFO, args, 2, 50000000LL, &result);
    if (reason != OSP_STOP_SENTINEL || result != 0) return 0;
    *value = eng_unfixed(osp_r32(M2_PARAM_BUF));
    return 1;
}

static int m2_select(const char *creator, int id)
{
    osp_w32(M2_VOICE_SPEC, eng_ostype(creator));
    osp_w32(M2_VOICE_SPEC + 4, (unsigned)id);
    if (m2_set_info(SO_CURRENT_VOICE, M2_VOICE_SPEC) != 0) return 0;
    /* The new voice brings its own 'pbas' and 'pmod', so the cached ones are
     * now wrong; taking a voice resets the channel to that voice's own. */
    g_m2.have_base_pitch = 0;
    g_m2.have_base_mod = 0;
    return 1;
}

static int m2_open(const EngManifest *m)
{
    const char *p1 = eng_file(m, "Cecy_1.bin"), *p3 = eng_file(m, "Cecy_3.bin");
    unsigned char *data; int len, i, fe;
    unsigned args[1], result = 0;

    memset(&g_m2, 0, sizeof g_m2);
    g_m2.cur = -1;
    if (!p1 || !p3) { eng_fail("manifest names no Cecy_1.bin / Cecy_3.bin"); return ENG_ERR_MANIFEST; }

    if (eng_slurp(p3, &data, &len) != 0) { eng_fail("cannot read Cecy_3.bin"); return ENG_ERR_FILE; }
    if (osp_write_block(M2_FRONT_BASE, data, len) != 0) { free(data); eng_fail("front end does not fit"); return ENG_ERR_OPEN; }
    free(data);
    if (eng_slurp(p1, &data, &len) != 0) { eng_fail("cannot read Cecy_1.bin"); return ENG_ERR_FILE; }
    if (osp_write_block(M2_BACK_BASE, data, len) != 0) { free(data); eng_fail("back end does not fit"); return ENG_ERR_OPEN; }
    free(data);

    osp_heap_init(M2_HEAP, M2_HEAP_SIZE);
    osp_enable_mem_traps(1);
    osp_w8(M2_CPUFLAG, 0);
    osp_w16(M2_RESERR, 0);
    osp_w16(M2_MEMERR, 0);

    for (i = 0; i < 6; i++) {
        char name[32];
        const char *path;
        sprintf(name, "%s_%d.bin", M2_TABLES[i].type, M2_TABLES[i].id);
        path = eng_file(m, name);
        if (path) eng_add_resource_file(M2_TABLES[i].type, M2_TABLES[i].id, path, -1);
    }

    /* Every voice's resources, under the ids its own ttvd asks for.  A voice
     * that will not load is dropped rather than fatal: one bad extraction
     * must not cost the user the other nine. */
    for (i = 0; i < m->nvoices; i++) {
        const EngVoice *v = &m->voices[i];
        int k, ttvd = -1, ok = 1;
        for (k = 0; k < v->nfiles && ok; k++) {
            int rid;
            if (eng_rid_from_name(v->files[k].path, &rid) != 0) { ok = 0; break; }
            if (!eng_add_resource_file(v->files[k].name, rid, v->files[k].path, -1)) { ok = 0; break; }
            if (strcmp(v->files[k].name, "ttvd") == 0) ttvd = rid;
        }
        if (!ok || ttvd < 0) continue;
        /* We are the Speech Manager: GetVoiceInfo('fref') answers with the
         * ttvd id, because that is what the engine opens a voice by. */
        if (osp_add_voice(eng_ostype(v->creator), (unsigned)v->id, ttvd, -1) < 0) continue;
        g_m2.voices[g_m2.nvoices].creator = eng_ostype(v->creator);
        g_m2.voices[g_m2.nvoices].id = v->id;
        g_m2.voices[g_m2.nvoices].loaded = 1;
        g_m2.nvoices++;
    }
    if (!g_m2.nvoices) { eng_fail("no MacinTalk 2 voice could be loaded"); return ENG_ERR_OPEN; }

    fe = osp_add_component(eng_ostype("ttsc"), eng_ostype("mtk2"), eng_ostype("mtk2"), M2_FRONT_BASE);
    osp_add_component(eng_ostype("t2be"), eng_ostype("t2be"), eng_ostype("mtk2"), M2_BACK_BASE);
    if (fe < 0) { eng_fail("no room for the component"); return ENG_ERR_OPEN; }
    g_m2.chan = osp_open_instance(fe);
    if (!g_m2.chan) { eng_fail("cannot open the component instance"); return ENG_ERR_OPEN; }

    osp_set_reg(OSP_REG_A7, M2_STACK);
    osp_set_reg(OSP_REG_SR, 0x2700u);
    /* MacinTalk 2's callback only refills on its second invocation, so
     * answering it while the engine is still mid-call spends the first one
     * before there is anything for it to be about. */
    osp_defer_callbacks(1);

    args[0] = g_m2.chan;
    if (osp_component_call(g_m2.chan, M2_OPEN, args, 1, 50000000LL, &result) != OSP_STOP_SENTINEL
        || result != 0) {
        eng_fail("MacinTalk 2 Open failed");
        return ENG_ERR_OPEN;
    }

    /* The requested voice, else the first that loaded -- matched on creator
     * and id, never on anything else. */
    {
        unsigned want = eng_ostype(m->sel_creator);
        int found = 0;
        for (i = 0; i < g_m2.nvoices; i++)
            if (g_m2.voices[i].creator == want && g_m2.voices[i].id == m->sel_id) { found = 1; break; }
        if (!found) i = 0;
        {
            char c4[5];
            c4[0] = (char)(g_m2.voices[i].creator >> 24); c4[1] = (char)(g_m2.voices[i].creator >> 16);
            c4[2] = (char)(g_m2.voices[i].creator >> 8);  c4[3] = (char)g_m2.voices[i].creator; c4[4] = 0;
            m2_select(c4, g_m2.voices[i].id);
        }
    }
    return 0;
}

static void m2_close(void)
{
    unsigned result = 0;
    if (g_m2.chan) osp_component_call(g_m2.chan, M2_CLOSE, NULL, 0, 20000000LL, &result);
    g_m2.chan = 0;
}

static void m2_set_rate(int rate)
{
    m2_set_info(SO_RATE, m2_fixed_arg((double)rate));
}

/* 'pbas' is a musical scale, twelve to the octave, 60 at middle C: ask the
 * voice's own and move from it by tenths of a semitone. */
static void m2_set_pitch(int tenths)
{
    if (!g_m2.have_base_pitch) {
        if (!m2_get_fixed(SO_PITCH_BASE, &g_m2.base_pitch)) return;
        g_m2.have_base_pitch = 1;
    }
    m2_set_info(SO_PITCH_BASE, m2_fixed_arg(g_m2.base_pitch + tenths / 10.0));
}

/* NVDA's 0-100 with 50 leaving the voice as recorded; a voice whose own
 * 'pmod' is zero (RoboVox, Xero) gets a reference value above the midpoint
 * so the slider is not dead. */
static void m2_set_inflection(int percent)
{
    double value;
    if (!g_m2.have_base_mod) {
        if (!m2_get_fixed(SO_PITCH_MOD, &g_m2.base_mod)) return;
        g_m2.have_base_mod = 1;
    }
    if (g_m2.base_mod > 0) value = g_m2.base_mod * percent / 50.0;
    else value = M2_INFLECTION_REFERENCE * (percent - 50 > 0 ? percent - 50 : 0) / 50.0;
    m2_set_info(SO_PITCH_MOD, m2_fixed_arg(value < 100.0 ? value : 100.0));
}

static void m2_translate(const unsigned char *text, int len, NumBuf *out)
{
    eng_translate_sm(text, len, 0, out);
}

/* SpeechStatusInfo.outputBusy, which is how the engine says it is done:
 * outputBusy is byte 0 and inputBytesLeft a long at +2. */
static int m2_busy(void)
{
    unsigned args[1], result = 0;
    osp_w32(M2_STATUS_BUF, 0); osp_w32(M2_STATUS_BUF + 4, 0); osp_w32(M2_STATUS_BUF + 8, 0);
    args[0] = M2_STATUS_BUF;
    if (osp_component_call(g_m2.chan, M2_STATUS, args, 1, 20000000LL, &result) != OSP_STOP_SENTINEL)
        return 0;
    return osp_r8(M2_STATUS_BUF) != 0 || osp_r32(M2_STATUS_BUF + 2) != 0;
}

/* -> 8-bit unsigned PCM, trailing silence trimmed.  SpeakBuffer returns as
 * soon as the first buffer is queued; each callback installs a deferred
 * task, and *that* renders.  The stopping condition comes from the engine,
 * not from the callback chain going quiet: pumping until nothing is pending
 * once gave 131 seconds of silence. */
static void m2_speak(const unsigned char *text, int len, NumBuf *out)
{
    const unsigned char *raw = text;
    int n = len, mark, nlen;
    unsigned args[3], result = 0;
    unsigned lengths[M2_MAX_BUFFERS + 64];

    eng_strip(&raw, &n);
    if (n <= 0) return;
    osp_pcm_reset();
    mark = osp_buflog_n();
    osp_write_block(M2_TEXT_BUF, raw, n);
    args[0] = M2_TEXT_BUF; args[1] = (unsigned)n; args[2] = 0;
    if (osp_component_call(g_m2.chan, M2_SPEAK, args, 3, 400000000LL, &result) != OSP_STOP_SENTINEL)
        return;
    while (osp_buffers_taken() < M2_MAX_BUFFERS) {
        if (!osp_run_callbacks(8, 200000000LL)) break;      /* nothing pending: finished */
        if (!m2_busy()) break;
    }
    /* Take the audio NOW, then let the engine settle and throw away whatever
     * that produces: a callback still pending for the utterance just finished
     * would otherwise queue its buffer at the front of the next one. */
    eng_take_pcm(out);
    nlen = eng_buflog_lengths(mark, lengths, (int)(sizeof lengths / sizeof lengths[0]));
    osp_run_callbacks(64, 200000000LL);
    osp_pcm_reset();
    if (out->oom) return;
    eng_drop_restated(out, lengths, nlen);
    eng_trim(out, 1200, 220);
}

/* Deliberately does not touch the emulator: a component call drives the
 * 68000, and two threads stepping one CPU corrupts it.  Nothing is lost --
 * rendering takes 15-150 ms and cancel's real work is draining the queues. */
static void m2_stop(void) { }

static const EngOps ENG_MTK2_OPS = {
    m2_open, m2_close, m2_select, m2_set_rate, m2_set_pitch, NULL,
    m2_set_inflection, m2_translate, m2_speak, m2_stop,
    NULL, NULL, NULL, NULL
};
