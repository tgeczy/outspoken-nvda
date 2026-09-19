/* outspoken_jni.c -- JNI bridge from Kotlin (OutspokenNative) to the host.
 *
 * Thin: it marshals strings and the PCM buffer across the boundary and calls
 * the host's own API (src/osp_host.h).  The host itself -- Musashi, the
 * engines, the rules, the settings -- is linked into this same .so from the
 * same sources the NVDA DLL and the SAPI program are built from, so these
 * functions call osp_* directly, no IPC.
 *
 * C, not C++: the whole host is C, and a C++ bridge would drag libc++ into
 * the APK for nothing.
 *
 * The engine is process-global (one open at a time), so every call here is
 * serialised on the Kotlin side, on the worker's one thread.  nativeStop is
 * the exception: it sets the host's stop flag and enters no guest code.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "osp_host.h"

#define OUT_RATE 22254

/* What is open, so a voice change within one engine is a select and not a
 * rebuild.  The rule is the NVDA driver's (`_sync` in outspoken.py): same
 * kind and not Pro, select; otherwise open the entry's manifest, which
 * closes whatever was open. */
static char g_open_id[256];
static char g_open_kind[32];
static char g_error[256];

/* The size-needed contract, in one place: call `fn` with a growing buffer
 * until it fits.  -> a malloc'd, NUL-terminated buffer and its length, or
 * NULL with `*len` negative. */
typedef int (*sized_fn)(void *ctx, char *out, int cap);

static char *sized(sized_fn fn, void *ctx, int *len)
{
    int cap = 4096, n;
    char *buf = NULL;
    for (;;) {
        char *grown = realloc(buf, (size_t)cap + 1);
        if (!grown) { free(buf); *len = -1; return NULL; }
        buf = grown;
        n = fn(ctx, buf, cap);
        if (n < 0) { free(buf); *len = n; return NULL; }
        if (n <= cap) { buf[n] = 0; *len = n; return buf; }
        cap = n + 1;
    }
}

static int entry_fn(void *ctx, char *out, int cap) { return osp_catalogue_entry(*(int *)ctx, out, cap); }
static int manifest_fn(void *ctx, char *out, int cap) { return osp_catalogue_manifest(*(int *)ctx, out, cap); }
static int skipped_fn(void *ctx, char *out, int cap) { return osp_catalogue_skipped(*(int *)ctx, out, cap); }
static int macroman_fn(void *ctx, char *out, int cap) { return osp_text_macroman((const char *)ctx, (unsigned char *)out, cap); }

typedef struct { const unsigned char *text; int len; } TranslateCtx;
static int translate_fn(void *ctx, char *out, int cap)
{
    TranslateCtx *t = ctx;
    return osp_engine_translate(t->text, t->len, (unsigned char *)out, cap);
}

static char *jstring_dup(JNIEnv *env, jstring s)
{
    const char *p;
    char *out;
    if (!s) return NULL;
    p = (*env)->GetStringUTFChars(env, s, NULL);
    if (!p) return NULL;
    out = strdup(p);
    (*env)->ReleaseStringUTFChars(env, s, p);
    return out;
}

/* Every catalogue line, or every skipped line, joined with newlines. */
static jstring lines(JNIEnv *env, int count, sized_fn fn)
{
    size_t total = 0, cap = 0;
    char *all = NULL;
    jstring result;
    int i;
    for (i = 0; i < count; i++) {
        int len;
        char *line = sized(fn, &i, &len);
        if (!line) continue;
        if (total + (size_t)len + 2 > cap) {
            char *grown;
            cap = (total + (size_t)len + 2) * 2;
            grown = realloc(all, cap);
            if (!grown) { free(all); free(line); return NULL; }
            all = grown;
        }
        memcpy(all + total, line, (size_t)len);
        total += (size_t)len;
        all[total++] = '\n';
        free(line);
    }
    if (!all) return (*env)->NewStringUTF(env, "");
    all[total] = 0;
    result = (*env)->NewStringUTF(env, all);
    free(all);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeScan(JNIEnv *env, jclass cls, jstring jroots)
{
    char *roots = jstring_dup(env, jroots);
    int n;
    (void)cls;
    if (!roots) return NULL;
    n = osp_catalogue_scan(roots);
    free(roots);
    if (n < 0) return NULL;
    return lines(env, n, entry_fn);
}

JNIEXPORT jstring JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeSkipped(JNIEnv *env, jclass cls)
{
    (void)cls;
    return lines(env, osp_catalogue_skipped_count(), skipped_fn);
}

JNIEXPORT jstring JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeError(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_error);
}

/* Field `k` of a tab-separated catalogue line, copied into `out`. */
static int field(const char *line, int k, char *out, size_t cap)
{
    const char *p = line;
    size_t n;
    while (k-- > 0) {
        p = strchr(p, '\t');
        if (!p) return 0;
        p++;
    }
    n = strcspn(p, "\t\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

JNIEXPORT jint JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeUseVoice(JNIEnv *env, jclass cls, jstring jroots, jstring jid)
{
    char *roots = jstring_dup(env, jroots);
    char *id = jstring_dup(env, jid);
    int n, i, found = -1, rc = 0;
    char kind[32] = "", creator[32] = "", voice_id[32] = "";
    (void)cls;
    g_error[0] = 0;
    if (!roots || !id) { free(roots); free(id); strcpy(g_error, "no roots or id"); return -1; }
    n = osp_catalogue_scan(roots);
    free(roots);
    for (i = 0; i < n && found < 0; i++) {
        int len;
        char *line = sized(entry_fn, &i, &len);
        char this_id[256];
        if (!line) continue;
        if (field(line, 0, this_id, sizeof this_id) && !strcmp(this_id, id)) {
            found = i;
            field(line, 2, kind, sizeof kind);
            field(line, 3, creator, sizeof creator);
            field(line, 4, voice_id, sizeof voice_id);
        }
        free(line);
    }
    if (found < 0) {
        snprintf(g_error, sizeof g_error, "no voice %s under the roots", id);
        free(id);
        return -2;
    }
    if (g_open_id[0] && !strcmp(g_open_kind, kind)
            && (strcmp(kind, "gala") != 0 || !strcmp(g_open_id, id))) {
        if (strcmp(g_open_id, id) != 0) {
            /* A refusal leaves the previous voice in place: speaking in the
             * wrong voice beats silence, as the NVDA driver decided. */
            if (osp_engine_select(creator, atoi(voice_id)))
                strncpy(g_open_id, id, sizeof g_open_id - 1);
            else
                snprintf(g_error, sizeof g_error, "the engine refused voice %s", id);
        }
    } else {
        int len;
        char *manifest = sized(manifest_fn, &found, &len);
        if (!manifest) { strcpy(g_error, "no manifest"); free(id); return -3; }
        rc = osp_engine_open(manifest);
        free(manifest);
        if (rc != 0) {
            const char *why = osp_engine_error();
            snprintf(g_error, sizeof g_error, "%s: %s (%d)", id, why ? why : "", rc);
            g_open_id[0] = 0; g_open_kind[0] = 0;
            free(id);
            return rc < 0 ? rc : -4;
        }
        strncpy(g_open_id, id, sizeof g_open_id - 1);
        strncpy(g_open_kind, kind, sizeof g_open_kind - 1);
    }
    free(id);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeSettings(JNIEnv *env, jclass cls, jint rate, jint pitch,
                                                     jint inflection, jint volume, jint numbers,
                                                     jint ratePercent)
{
    (void)env; (void)cls;
    osp_set_rate(rate);
    osp_set_pitch(pitch);
    osp_set_inflection(inflection);
    osp_set_volume(volume);
    osp_set_volume_offset(0);
    osp_engine_set_numbers(numbers);
    osp_apply_settings(0, 0);
    if (ratePercent > 0 && ratePercent != 100) {
        /* The requesting app's speech rate on top of the slider: the words
         * per minute the slider stands for, scaled, within the curve's own
         * range, so the top of the slider at 200% is still the top. */
        int engine_rate = 0, tenths = 0, wpm;
        double hz = 0;
        osp_settings_preview(rate, 0, pitch, 0, 110.0, &engine_rate, &tenths, &hz);
        wpm = (int)((long)engine_rate * ratePercent / 100);
        if (wpm < 60) wpm = 60;
        if (wpm > 900) wpm = 900;
        osp_engine_set_rate(wpm);
    }
}

JNIEXPORT jint JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeStart(JNIEnv *env, jclass cls, jbyteArray jutf8)
{
    jsize n;
    jbyte *b;
    char *utf8, *mac, *prepared;
    int maclen, plen, r;
    TranslateCtx t;
    (void)cls;
    if (!jutf8) return 1;
    n = (*env)->GetArrayLength(env, jutf8);
    utf8 = malloc((size_t)n + 1);
    if (!utf8) return -1;
    b = (*env)->GetByteArrayElements(env, jutf8, NULL);
    if (!b) { free(utf8); return -1; }
    memcpy(utf8, b, (size_t)n);
    utf8[n] = 0;
    (*env)->ReleaseByteArrayElements(env, jutf8, b, JNI_ABORT);
    mac = sized(macroman_fn, utf8, &maclen);
    free(utf8);
    if (!mac) return -1;
    t.text = (const unsigned char *)mac; t.len = maclen;
    prepared = sized(translate_fn, &t, &plen);
    free(mac);
    if (!prepared) return plen < 0 ? plen : -1;
    r = osp_engine_speak_start((const unsigned char *)prepared, plen);
    free(prepared);
    return r == 0 ? 0 : (r > 0 ? 1 : r);
}

JNIEXPORT jint JNICALL
Java_com_outspoken_tts_OutspokenNative_nativePull(JNIEnv *env, jclass cls, jshortArray jout)
{
    static unsigned char pcm8[1 << 16];
    jsize cap = (*env)->GetArrayLength(env, jout);
    jshort *out;
    int n;
    (void)cls;
    if (cap > (jsize)sizeof pcm8) cap = (jsize)sizeof pcm8;
    n = osp_engine_pull(pcm8, (int)cap);
    if (n <= 0) return n;
    out = (*env)->GetShortArrayElements(env, jout, NULL);
    if (!out) return -1;
    osp_pcm_widen(pcm8, n, out, (int)cap);
    (*env)->ReleaseShortArrayElements(env, jout, out, 0);   /* copy back */
    return n;
}

JNIEXPORT void JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeCancel(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    osp_engine_cancel();
}

JNIEXPORT void JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    osp_engine_stop();
}

JNIEXPORT void JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeClose(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    osp_engine_close();
    g_open_id[0] = 0; g_open_kind[0] = 0;
}

JNIEXPORT jint JNICALL
Java_com_outspoken_tts_OutspokenNative_nativeSampleRate(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return OUT_RATE;
}
