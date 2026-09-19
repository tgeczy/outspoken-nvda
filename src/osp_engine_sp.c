/* osp_engine_sp.c -- the 1984 MacinTalk driver, `.sp`, driven from C.
 *
 * A port of `engine.py`.  The whole sequence, and why each step is there, is
 * in docs/driver-api.md, docs/sound-model.md and docs/frame-format.md:
 *
 *     Open                      allocates dCtlStorage, loads TALK 1
 *     driver+$0034              install the per-frame callback  <- load-bearing
 *     driver+$001E              hand over the channel and two buffers
 *     Prime (_Write)            speak; PCM arrives at every bufferCmd
 *
 * Every address, budget and padding value below is the Python's, unchanged:
 * they decide where the callback lands and the PCM depends on them, and the
 * render oracle holds this file to the Python byte for byte.
 */

#define SP_DRV_BASE   0x00040000u
#define SP_HEAP       0x00080000u
#define SP_HEAP_SIZE  0x00080000u
#define SP_STACK      0x00200000u
#define SP_WORK       0x00190000u

#define SP_CPUFLAG    0x012Fu
#define SP_RESERR     0x0A60u
#define SP_EXPORT_MACSTARTSOUND 0x001Eu
#define SP_EXPORT_SET_CALLBACK  0x0034u
#define SP_BUF_BYTES  (22u + 3870u)
/* The most SetBufLength will ever declare: it clamps to $F1E. */
#define SP_BUF_LIMIT  0xF1Eu

#define SP_FLAG  (SP_WORK + 0x280u)     /* our stop flag, polled by the hook */
#define SP_HOOK  (SP_WORK + 0x200u)

/* The driver's own globals, found from the disassembly of SetBufLength:
 * `lea.l $4c16(pc), a4`.  See engine.py for the listing. */
#define SP_GLOBALS (SP_DRV_BASE + 0x4C16u)
#define SP_G_STOP  (SP_GLOBALS + 0x0Cu)   /* where the synthesiser stopped */
#define SP_G_BUFA  (SP_GLOBALS + 0x14u)   /* buffer A's SoundHeader */
#define SP_G_BUFB  (SP_GLOBALS + 0x18u)   /* buffer B's SoundHeader */

/* A few milliseconds of ramp at each end, and a short fixed gap around each
 * utterance, mostly in front -- the reasoning is in engine.py; the numbers
 * are computed the way it computes them. */
#define SP_FADE     90
#define SP_LEAD_MS  70
#define SP_TAIL_MS  40

static struct {
    unsigned entries[5];        /* open, prime, control, status, close */
    unsigned dce, pb;
    unsigned bufa, bufb;
    int have_rules, have_dict;
    volatile int speaking;
} g_sp;

static unsigned sp_storage(void)
{
    /* dCtlStorage is a HANDLE at DCE+$14; writing through the handle itself
     * changes nothing at all and is very quiet about it. */
    return osp_r32(osp_r32(g_sp.dce + 0x14u));
}

static int sp_open_driver(void)
{
    unsigned off;
    for (off = 0; off < 0x80u; off += 4) { osp_w32(g_sp.dce + off, 0); osp_w32(g_sp.pb + off, 0); }
    osp_w32(g_sp.dce + 0, SP_DRV_BASE);
    osp_w16(g_sp.dce + 4, 0x4600u);
    osp_w16(g_sp.dce + 24, 0xFFEFu);
    osp_w16(g_sp.pb + 24, 0xFFEFu);
    osp_set_reg(OSP_REG_SR, 0x2700u);
    osp_set_reg(OSP_REG_A7, SP_STACK);
    osp_set_reg(OSP_REG_A0, g_sp.pb);
    osp_set_reg(OSP_REG_A1, g_sp.dce);
    if (osp_call(SP_DRV_BASE + g_sp.entries[0], osp_magic_sentinel(), 5000000LL) != OSP_STOP_SENTINEL) {
        eng_fail("MacinTalk Open did not return");
        return -1;
    }
    return 0;
}

/* The per-frame callback -- see docs/frame-format.md.  It reads f[0] and
 * f[1] of every frame, and bit 7 of f[0] ends the utterance.  It also polls
 * our stop flag: returning with N set is the engine's own designed way to
 * stop, which is why the export is called SetStopSpeechCallback. */
static int sp_install_hook(void)
{
    static const unsigned short code[] = {
        0x4A39, (SP_FLAG >> 16) & 0xFFFFu, SP_FLAG & 0xFFFFu,   /* tst.b FLAG.l          */
        0x660E,                                                 /* bne.s -> stop         */
        0x1B5E, 0x0001,                                         /* move.b (a6)+, $1(a5)  */
        0x1B5E, 0x0003,                                         /* move.b (a6)+, $3(a5)  */
        0x4A2D, 0x0001,                                         /* tst.b  $1(a5) -- restore N */
        0x4E75,                                                 /* rts                   */
        0x70FF,                                                 /* moveq #-1, d0 (N set = stop) */
        0x4E75                                                  /* rts                   */
    };
    unsigned args[1] = { SP_HOOK };
    int i;
    for (i = 0; i < (int)(sizeof code / sizeof code[0]); i++)
        osp_w16(SP_HOOK + 2u * (unsigned)i, code[i]);
    osp_w8(SP_FLAG, 0);
    osp_set_reg(OSP_REG_A7, SP_STACK);
    if (osp_call_with_args(SP_DRV_BASE + SP_EXPORT_SET_CALLBACK, args, 1, 1000LL) != OSP_STOP_SENTINEL) {
        eng_fail("could not install the speech callback");
        return -1;
    }
    return 0;
}

static int sp_start_sound(void)
{
    unsigned chan = SP_WORK + 0x400u, rec = SP_WORK + 0x300u, off;
    unsigned args[1] = { rec };
    g_sp.bufa = SP_WORK + 0x1000u;
    g_sp.bufb = SP_WORK + 0x3000u;
    for (off = 0; off < 0x80u; off += 4) osp_w32(chan + off, 0);
    /* ChannelBusy short-circuits on chan+$20 == -1 and reports idle without
     * asking the Sound Manager, which is exactly our model: buffers are
     * consumed the instant they are handed over. */
    osp_w16(chan + 0x20u, 0xFFFFu);
    for (off = 0; off < SP_BUF_BYTES + 4u; off += 4) {
        osp_w32(g_sp.bufa + off, 0);
        osp_w32(g_sp.bufb + off, 0);
    }
    osp_w32(rec + 0, chan);
    osp_w32(rec + 4, g_sp.bufa);
    osp_w32(rec + 8, g_sp.bufb);
    osp_set_reg(OSP_REG_A7, SP_STACK);
    osp_set_reg(OSP_REG_A1, g_sp.dce);
    if (osp_call_with_args(SP_DRV_BASE + SP_EXPORT_MACSTARTSOUND, args, 1, 10000000LL) != OSP_STOP_SENTINEL) {
        eng_fail("MACSTARTSOUND failed");
        return -1;
    }
    return 0;
}

static int sp_open(const EngManifest *m)
{
    const char *p_drvr = eng_file(m, "DRVR_1030.bin");
    const char *p_talk = eng_file(m, "TALK_1001.bin");
    const char *p_rulz = eng_file(m, "RULZ_1129.bin");
    const char *p_dict = eng_file(m, "DICT_-4048.bin");
    unsigned char *image, *talk, *blob;
    int image_len, talk_len, blob_len, i;

    memset(&g_sp, 0, sizeof g_sp);
    if (!p_drvr || !p_talk) { eng_fail("manifest names no DRVR_1030.bin / TALK_1001.bin"); return ENG_ERR_MANIFEST; }
    if (eng_slurp(p_drvr, &image, &image_len) != 0) { eng_fail("cannot read DRVR_1030.bin"); return ENG_ERR_FILE; }
    if (image_len < 18) { free(image); eng_fail("DRVR_1030.bin is too short"); return ENG_ERR_FILE; }
    /* (open, prime, control, status, close) from the DRVR header. */
    for (i = 0; i < 5; i++)
        g_sp.entries[i] = ((unsigned)image[8 + 2 * i] << 8) | image[9 + 2 * i];

    if (osp_write_block(SP_DRV_BASE, image, image_len) != 0) { free(image); eng_fail("driver image does not fit"); return ENG_ERR_OPEN; }
    free(image);
    osp_heap_init(SP_HEAP, SP_HEAP_SIZE);
    osp_enable_mem_traps(1);
    osp_w8(SP_CPUFLAG, 0);
    osp_w16(SP_RESERR, 0);

    if (eng_slurp(p_talk, &talk, &talk_len) != 0) { eng_fail("cannot read TALK_1001.bin"); return ENG_ERR_FILE; }
    if (osp_add_resource(0x54414C4Bu /* 'TALK' */, 1, talk, talk_len, -1) == 0) { free(talk); eng_fail("TALK would not register"); return ENG_ERR_OPEN; }
    free(talk);

    /* RULZ only drives the English front end.  Without it the synthesiser
     * still speaks, but only if it is handed phonemes -- translate() then
     * passes text straight through, exactly as engine.py does. */
    if (p_rulz && eng_slurp(p_rulz, &blob, &blob_len) == 0) {
        g_sp.have_rules = osp_nrl_load(blob, blob_len) == 0;
        free(blob);
    }
    /* Berkeley's exception list.  Optional: without it the engine still
     * speaks, it just says "sea-rch" for SEARCH.  A list that does not parse
     * is simply not loaded, as the Python catches the ValueError. */
    if (p_dict && eng_slurp(p_dict, &blob, &blob_len) == 0) {
        g_sp.have_dict = osp_nrl_load_dictionary(blob, blob_len) == 0;
        free(blob);
    }

    g_sp.dce = SP_WORK;
    g_sp.pb = SP_WORK + 0x100u;
    if (sp_open_driver() || sp_install_hook() || sp_start_sound()) return ENG_ERR_OPEN;
    return 0;
}

static void sp_close(void)
{
    g_sp.speaking = 0;
}

static void sp_set_voice_hz(double hz)
{
    int v = (int)hz;                       /* int(): truncation, as in Python */
    if (v < 65) v = 65;
    if (v > 500) v = 500;
    osp_w16(sp_storage() + 0x30u, (unsigned)v);
}

static void sp_set_rate(int rate)
{
    if (rate < 40) rate = 40;
    if (rate > 2560) rate = 2560;
    osp_w16(sp_storage() + 0x32u, (unsigned)rate);
}

/* 1984 has no inflection control and the slider cannot pretend it does:
 * `Control` takes four csCodes and none is a contour.  Present so the
 * driver can call it on every engine without asking which one it has. */
static void sp_set_inflection(int percent) { (void)percent; }

/* English -> phonemes, as engine.py's translate: the letter name for a lone
 * letter (typing echo wants the NAME), else numbers, then Berkeley's
 * respelling, then the rules.  Numbers and respelling apply to the whole
 * text; only the letter test looks at the stripped copy. */
static void sp_translate(const unsigned char *text, int len, NumBuf *out)
{
    const unsigned char *t = text;
    int tn = len, need;
    NumBuf a, b;

    if (!g_sp.have_rules) { nb_put(out, text, (size_t)len); return; }

    eng_strip(&t, &tn);
    if (tn == 1 && eng_is_alpha(t[0])) {
        eng_text_into(osp_nrl_letter_name, t, 1, out);
        return;
    }

    nb_init(&a);
    eng_numbers(text, len, 0, &a);
    if (a.oom) { out->oom = 1; nb_free(&a); return; }

    nb_init(&b);
    if (g_sp.have_dict) eng_text_into(osp_nrl_respell, a.p, (int)a.len, &b);
    else nb_put(&b, a.p, a.len);
    nb_free(&a);
    if (b.oom) { out->oom = 1; nb_free(&b); return; }

    eng_text_into(osp_nrl_translate, b.p, (int)b.len, out);
    nb_free(&b);
    (void)need;
}

/* The samples the engine wrote and never handed over.
 *
 * The driver fills a buffer, hands it over with bufferCmd when it is full,
 * and switches to the other one.  At the end of speech it is normally
 * part-way through a buffer -- and it neither shortens that buffer nor hands
 * it over; it leaves globals[$0C] pointing at where it stopped, for the
 * NEXT utterance's SetupA3 to apply to the wrong buffer.  So the end of
 * every utterance was being spoken one utterance late, and above about 686
 * wpm -- the whole word inside one buffer -- nothing at all.  Read the
 * length from the pointer rather than scanning for silence: the engine's
 * own trailing hiss is not $80. */
static void sp_last_buffer(NumBuf *out)
{
    unsigned stop = osp_r32(SP_G_STOP), bases[2], k;
    if (!stop) return;
    bases[0] = osp_r32(SP_G_BUFA);
    bases[1] = osp_r32(SP_G_BUFB);
    for (k = 0; k < 2; k++) {
        unsigned area = bases[k] + 0x16u;
        if (area <= stop && stop <= area + SP_BUF_LIMIT) {
            unsigned n = stop - area;
            if (nb_reserve(out, n)) return;
            if (osp_read_block(area, out->p + out->len, (int)n) == 0) out->len += n;
            return;
        }
    }
}

/* Strip the engine's padding and ramp the edges, then put a short fixed gap
 * back -- mostly in front, where an interruption cannot destroy it.  The
 * arithmetic is engine.py's `_tidy`, including int()'s truncation. */
static void sp_tidy(const unsigned char *pcm, size_t n, NumBuf *out)
{
    size_t i = 0, j = n, len, fade, k;
    int lead = (int)(22254.5454 * SP_LEAD_MS / 1000.0);
    int tail = (int)(22254.5454 * SP_TAIL_MS / 1000.0);
    unsigned char *o;
#define SP_IS_PAD(c) ((c) == 0x80 || (c) == 0x60 || (c) == 0x40)
    while (i < n && SP_IS_PAD(pcm[i])) i++;
    while (j > i && SP_IS_PAD(pcm[j - 1])) j--;
#undef SP_IS_PAD
    if (j - i < 2) return;
    len = j - i;
    {
        unsigned char z = 0x80;
        for (k = 0; k < (size_t)lead; k++) nb_put(out, &z, 1);
    }
    {
        size_t start = out->len;
        nb_put(out, pcm + i, len);
        if (out->oom) return;
        o = out->p + start;
    }
    fade = (size_t)SP_FADE < len / 2 ? (size_t)SP_FADE : len / 2;
    for (k = 0; k < fade; k++) {
        double g = (double)k / (double)fade;
        o[k] = (unsigned char)(128 + (int)(((int)o[k] - 128) * g));
        o[len - 1 - k] = (unsigned char)(128 + (int)(((int)o[len - 1 - k] - 128) * g));
    }
    {
        unsigned char z = 0x80;
        for (k = 0; k < (size_t)tail; k++) nb_put(out, &z, 1);
    }
}

static void sp_speak(const unsigned char *phonemes, int len, NumBuf *out)
{
    const unsigned char *raw = phonemes;
    int n = len, i;
    unsigned txt = SP_WORK + 0x8000u, txt_h = SP_WORK + 0x7000u;
    NumBuf got;
    unsigned char silence[3870];

    osp_w8(SP_FLAG, 0);
    /* Wipe the sound buffers first: MACSTARTSOUND is handed two buffers once
     * and the engine reuses them, so anything a short utterance does not
     * reach is still the PREVIOUS one's speech. */
    memset(silence, 0x80, sizeof silence);
    osp_write_block(g_sp.bufa + 22u, silence, (int)sizeof silence);
    osp_write_block(g_sp.bufb + 22u, silence, (int)sizeof silence);

    eng_strip(&raw, &n);
    if (n <= 0) return;
    /* A trailing space and a cleared run after it are both load-bearing: the
     * parser reads past the text it was given. */
    {
        unsigned char *buf = (unsigned char *)malloc((size_t)n + 1 + 64);
        if (!buf) { out->oom = 1; return; }
        memcpy(buf, raw, (size_t)n);
        buf[n] = ' ';
        memset(buf + n + 1, ' ', 64);
        osp_write_block(txt, buf, n + 1 + 64);
        free(buf);
    }
    osp_w32(txt_h, txt);
    osp_w32(g_sp.pb + 32u, txt_h);          /* ioBuffer is a HANDLE, not a pointer */
    osp_w32(g_sp.pb + 36u, (unsigned)(n + 1));
    osp_w32(g_sp.pb + 40u, 0);
    osp_w16(g_sp.pb + 16u, 1);
    osp_pcm_reset();
    osp_set_reg(OSP_REG_A7, SP_STACK);
    osp_set_reg(OSP_REG_A0, g_sp.pb);
    osp_set_reg(OSP_REG_A1, g_sp.dce);
    g_sp.speaking = 1;
    osp_call(SP_DRV_BASE + g_sp.entries[1], osp_magic_sentinel(), 400000000LL);
    /* Clear it on the way out as well as on the way in, so a stop that lands
     * late cannot survive into the next utterance. */
    g_sp.speaking = 0;
    osp_w8(SP_FLAG, 0);

    nb_init(&got);
    {
        unsigned pl = osp_pcm_len();
        if (nb_reserve(&got, pl + 1)) { out->oom = 1; nb_free(&got); return; }
        i = osp_pcm_get(got.p, (int)pl);
        got.len = i < 0 ? 0 : (size_t)i;
    }
    sp_last_buffer(&got);
    if (got.oom) { out->oom = 1; nb_free(&got); return; }
    sp_tidy(got.p, got.len, out);
    nb_free(&got);
}

/* Interrupt the utterance in flight, if there is one.  Only while actually
 * synthesising: the flag is polled by the frame callback, and setting it
 * while the engine is idle poisons the NEXT utterance instead. */
static void sp_stop(void)
{
    if (g_sp.speaking) osp_w8(SP_FLAG, 1);
}

static const EngOps ENG_SP_OPS = {
    sp_open, sp_close, NULL, sp_set_rate, NULL, sp_set_voice_hz,
    sp_set_inflection, sp_translate, sp_speak, sp_stop
};
