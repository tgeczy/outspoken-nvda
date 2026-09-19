/* osp_settings.c -- the driver's settings, on the driver's own scales.
 *
 * NVDA, SAPI, the command line and Android all speak in 0-100: a rate, a
 * pitch, a volume, an inflection.  The engines do not -- they take words per
 * minute, tenths of a semitone from the voice's own pitch, hertz for the
 * 1984 driver, a 'pmod' depth -- and until 2.0 the arithmetic between the
 * two lived in `outspoken.py`, so only NVDA had it.  It lives here now,
 * ported from `SynthDriver._applySettings`, `_pitchTenths`, `_baseHz`,
 * `_gainTables` and `_to16`, and `tools/settings_oracle.py` diffs it
 * against them over every value of every slider.  Pure arithmetic, so that
 * oracle needs no engine data and runs in CI.
 *
 * Volume is applied here rather than in the engines, and the reason is
 * arithmetic: every one of the four hands back 8-bit unsigned, so
 * attenuating inside an engine would quantise 256 levels down to whatever
 * the slider left of them.  Scaling after the widening keeps all 256.  There
 * is no boost, measured rather than assumed: eleven of MacinTalk 3's voices
 * already peak at the rails.
 */

#define SET_RATE_MIN        60.0
#define SET_RATE_MAX        900.0
#define SET_PITCH_SEMITONES 12

static struct {
    int rate, pitch, volume, inflection;   /* the user's 0-100 settings */
    int volume_adj;                        /* a VolumeCommand's offset, 0 = none */
} g_set = { 50, 50, 100, 50, 0 };

static int set_clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

/* Words per minute, or the 1984 driver's rate field, from 0-100: a curve
 * from 60 to 900, `int()` truncating as the Python's does. */
static int set_engine_rate(int rate, int radj)
{
    int r = set_clamp100(rate + radj);
    return (int)(SET_RATE_MIN * pow(SET_RATE_MAX / SET_RATE_MIN, r / 100.0));
}

/* NVDA's 0-100 as tenths of a semitone either side of the voice's own
 * pitch; 50 is the voice exactly as recorded.  `adj` is a PitchCommand's
 * offset -- how NVDA marks a capital -- clamped in together. */
static int set_pitch_tenths(int pitch, int adj)
{
    int p = set_clamp100(pitch + adj);
    return (int)nearbyint((p - 50) * SET_PITCH_SEMITONES * 10 / 50.0);
}

/* 1984 has no voice apart from its pitch and nothing to ask, so the offset
 * goes on the base this voice is named for. */
static double set_sp_hz(double base_hz, int tenths)
{
    return base_hz * pow(2.0, tenths / 120.0);
}

/* What the mapping would apply, without applying it: the oracle's window.
 * -> the engine rate, the tenths, and for the 1984 driver the hertz. */
OSP_API void osp_settings_preview(int rate, int radj, int pitch, int padj, double base_hz,
                                  int *engine_rate, int *tenths, double *hz)
{
    int t = set_pitch_tenths(pitch, padj);
    if (engine_rate) *engine_rate = set_engine_rate(rate, radj);
    if (tenths) *tenths = t;
    if (hz) *hz = set_sp_hz(base_hz, t);
}

OSP_API void osp_set_rate(int percent)       { g_set.rate = percent; }
OSP_API void osp_set_pitch(int percent)      { g_set.pitch = percent; }
OSP_API void osp_set_inflection(int percent) { g_set.inflection = percent; }
/* The user's volume and, separately, what a VolumeCommand has asked on top
 * of it: an offset on the same 0-100 scale, 0 meaning the setting again. */
OSP_API void osp_set_volume(int percent)     { g_set.volume = percent; }
OSP_API void osp_set_volume_offset(int adj)  { g_set.volume_adj = adj; }
OSP_API int  osp_get_rate(void)              { return g_set.rate; }
OSP_API int  osp_get_pitch(void)             { return g_set.pitch; }
OSP_API int  osp_get_volume(void)            { return g_set.volume; }
OSP_API int  osp_get_inflection(void)        { return g_set.inflection; }

/* Push the settings at the open engine, as the driver does before every
 * utterance: rate and inflection, then pitch -- re-applied per utterance
 * because NVDA's capital-letter offset changes within a sequence.  `radj`
 * and `padj` are the RateCommand and PitchCommand offsets in force. */
OSP_API void osp_apply_settings(int radj, int padj)
{
    int tenths = set_pitch_tenths(g_set.pitch, padj);
    if (!g_eng.ops) return;
    osp_engine_set_rate(set_engine_rate(g_set.rate, radj));
    osp_engine_set_inflection(g_set.inflection);
    if (g_eng.kind == ENG_KIND_SP) osp_engine_set_voice_hz(set_sp_hz(g_eng.sp_hz, tenths));
    else osp_engine_set_pitch(tenths);
}

/* The 8-to-16 widening with the volume folded in:
 * `int(round((b - 128) * 256 * g))`, clamped, with the Python's half-to-even
 * rounding.  At 100 that is exactly the sample with its top bit flipped, in
 * the high byte, which is what the driver always did.  -> frames written,
 * or the frames needed when `out` is too small. */
OSP_API int osp_pcm_widen(const unsigned char *pcm8, int n, short *out, int cap)
{
    int volume = set_clamp100(g_set.volume + g_set.volume_adj);
    double g = volume / 100.0;
    short table[256];
    int i;
    if (n < 0) n = 0;
    if (!out || cap < n) return n;
    for (i = 0; i < 256; i++) {
        double v = nearbyint(((double)i - 128.0) * 256.0 * g);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        table[i] = (short)v;
    }
    for (i = 0; i < n; i++) out[i] = table[pcm8[i]];
    return n;
}

/* The last utterance, widened. */
OSP_API int osp_engine_pcm16(short *out, int cap)
{
    return osp_pcm_widen(g_eng.pcm.p, (int)g_eng.pcm.len, out, cap);
}
