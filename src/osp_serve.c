/* osp_serve.c -- serve mode: the protocol the SAPI bridge speaks, in C.
 *
 * A port of `sapi/osp_serve.py`, which stays the specification and which
 * `tests/test_sapi_serve.py` holds both of them to: the bytes the in-process
 * NVDA driver feeds its player, for the same text, voice and settings.
 *
 * Requests arrive on the input fd, framed, little-endian:
 *
 *     'OSP4' | seq | rate | pitch | volume | namelen | textlen | name | text
 *
 * and a cancel is 'OSPC' | seq.  The seq is the whole point of the cancel:
 * pipes buffer, so a cancel sent for one utterance can arrive after that
 * utterance already finished and the next one started, and an untagged
 * cancel then cuts the wrong render -- heard as the NEXT utterance losing its
 * tail.  A cancel only acts when its seq is the one rendering.
 *
 * rate/pitch/volume are the driver's own 0-100 integers; the text is UTF-8.
 * The response is 'OSPR' | status, then PCM in chunks as the engine produces
 * them -- u32 frame count, then frames*2 bytes of signed 16-bit mono at
 * 22254 Hz -- and a zero frame count to finish.  status != 0 means the
 * utterance failed and no audio follows.
 *
 * Requests and cancels arrive on a reader thread, so a cancel can land WHILE
 * an utterance renders -- the same shape the NVDA driver has, a queue plus a
 * cancel.  The renderer polls the cancel between pulls and abandons the
 * utterance through the engine layer; for the 1984 driver, whose render is
 * one blocking call, the reader also sets its stop flag directly, which is
 * the one cross-thread stop that engine was designed for.
 */

#define SERVE_REQ    0x4F535034u        /* 'OSP4' */
#define SERVE_RSP    0x4F535052u        /* 'OSPR' */
#define SERVE_CANCEL 0x4F535043u        /* 'OSPC' */
/* Frames per chunk on the wire.  The bridge clamps a chunk at 220,500
 * frames and reads ASCII in the stream as a frame count, so chunks stay
 * small and the stream stays private (see osp_plat_claim_stdout). */
#define SERVE_CHUNK  4096
#define SERVE_QUEUE  64

typedef struct {
    unsigned seq;
    int rate, pitch, volume;
    char voice[256];
    unsigned char *text;
    int textlen;
} ServeReq;

static struct {
    int in_fd, out_fd;
    volatile int eof;
    volatile unsigned current;      /* the seq being rendered, 0 for none */
    volatile int cancel_now;
    ServeReq queue[SERVE_QUEUE];
    int qn;
    unsigned cancelled[256];
    int ncancelled;
    /* the catalogue, and which entry the engine is open on */
    int cur_entry;
    char cur_kind[8];
} g_serve;

/* The settings a request does not carry: the SAPI bridge reads them from
 * its settings files and passes them on the command line, and replaces the
 * serve when they change -- Panthera's rule that a settings change respawns
 * the host rather than being quietly ignored by one that read its
 * arguments at startup.  The driver's own defaults until told otherwise. */
static int g_serve_inflection = 50;
static int g_serve_numbers = ENG_NUM_WORDS;

OSP_API void osp_serve_set_defaults(int inflection, int numbers)
{
    if (inflection < 0) inflection = 0;
    if (inflection > 100) inflection = 100;
    g_serve_inflection = inflection;
    g_serve_numbers = numbers == ENG_NUM_DIGITS ? ENG_NUM_DIGITS
                    : numbers == ENG_NUM_OFF ? ENG_NUM_OFF : ENG_NUM_WORDS;
}

static unsigned serve_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static void serve_put_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int serve_cancelled_has(unsigned seq)
{
    int i;
    for (i = 0; i < g_serve.ncancelled; i++) if (g_serve.cancelled[i] == seq) return 1;
    return 0;
}
static void serve_cancelled_drop(unsigned seq)
{
    int i;
    for (i = 0; i < g_serve.ncancelled; i++)
        if (g_serve.cancelled[i] == seq) { g_serve.cancelled[i] = g_serve.cancelled[--g_serve.ncancelled]; return; }
}

static void serve_reader(void *arg)
{
    unsigned char head[24], magic[4];
    (void)arg;
    for (;;) {
        unsigned m;
        if (!osp_plat_read_exact(g_serve.in_fd, magic, 4)) break;
        m = serve_u32(magic);
        if (m == SERVE_CANCEL) {
            unsigned char sb[4]; unsigned seq;
            if (!osp_plat_read_exact(g_serve.in_fd, sb, 4)) break;
            seq = serve_u32(sb);
            osp_plat_lock();
            if (g_serve.ncancelled < 256) g_serve.cancelled[g_serve.ncancelled++] = seq;
            if (seq == g_serve.current) {
                g_serve.cancel_now = 1;
                osp_engine_stop();          /* the 1984 driver's flag; a no-op elsewhere */
            }
            osp_plat_unlock();
            continue;
        }
        if (m != SERVE_REQ) break;
        if (!osp_plat_read_exact(g_serve.in_fd, head, 24)) break;
        {
            ServeReq r;
            unsigned nv = serve_u32(head + 16), nt = serve_u32(head + 20);
            unsigned char *name;
            memset(&r, 0, sizeof r);
            r.seq = serve_u32(head);
            r.rate = (int)serve_u32(head + 4);
            r.pitch = (int)serve_u32(head + 8);
            r.volume = (int)serve_u32(head + 12);
            if (nv > 255 || nt > (16u << 20)) break;
            name = (unsigned char *)malloc(nv + 1);
            r.text = (unsigned char *)malloc(nt + 1);
            if (!name || !r.text) { free(name); free(r.text); break; }
            if (!osp_plat_read_exact(g_serve.in_fd, name, (int)nv) || !osp_plat_read_exact(g_serve.in_fd, r.text, (int)nt)) {
                free(name); free(r.text); break;
            }
            memcpy(r.voice, name, nv); r.voice[nv] = 0; free(name);
            r.text[nt] = 0; r.textlen = (int)nt;
            osp_plat_lock();
            while (g_serve.qn >= SERVE_QUEUE) { osp_plat_unlock(); osp_plat_sleep_ms(5); osp_plat_lock(); }
            g_serve.queue[g_serve.qn++] = r;
            osp_plat_unlock();
        }
    }
    g_serve.eof = 1;
}

static int serve_write(const void *p, int n) { return osp_plat_write_all(g_serve.out_fd, p, n); }

static int serve_entry_by_id(const char *id, char *kind, char *creator, int *vid)
{
    int i, n = osp_catalogue_count();
    char entry[2048];
    for (i = 0; i < n; i++) {
        char *f[9]; int k; char *p;
        if (osp_catalogue_entry(i, entry, sizeof entry) < 0) continue;
        for (k = 0, p = entry; k < 9; k++) { f[k] = p; p = strchr(p, '\t'); if (!p) { p = f[k] + strlen(f[k]); } else *p++ = 0; }
        if (strcmp(f[0], id) == 0) {
            strcpy(kind, f[2]); strcpy(creator, f[3]); *vid = atoi(f[4]);
            return i;
        }
    }
    return -1;
}

/* driver._set_voice: the entry's engine if it is not the one open, else a
 * select within it.  -> 0, or -1 when the voice cannot be had. */
static int serve_ensure_voice(const char *id)
{
    char kind[8], creator[8];
    int vid, i;
    if (!id[0]) return g_serve.cur_entry >= 0 ? 0 : -1;
    i = serve_entry_by_id(id, kind, creator, &vid);
    if (i < 0) return -1;
    if (i == g_serve.cur_entry) return 0;
    if (g_serve.cur_entry >= 0 && strcmp(kind, g_serve.cur_kind) == 0 && strcmp(kind, "gala") != 0) {
        /* The same engine: a select, as the driver switches voices within
         * MacinTalk 2 and 3 without rebuilding.  Pro rebuilds per voice. */
        if (osp_engine_select(creator, vid)) { g_serve.cur_entry = i; return 0; }
    }
    {
        char *manifest;
        int need = osp_catalogue_manifest(i, NULL, 0);
        if (need < 0) return -1;
        manifest = (char *)malloc((size_t)need + 1);
        if (!manifest) return -1;
        osp_catalogue_manifest(i, manifest, need + 1);
        if (osp_engine_open(manifest) != 0) { free(manifest); g_serve.cur_entry = -1; return -1; }
        free(manifest);
        g_serve.cur_entry = i;
        strcpy(g_serve.cur_kind, kind);
        /* The bridge's settings, or the driver's defaults when nothing
         * configured them: numbers as words, inflection at the middle. */
        osp_engine_set_numbers(g_serve_numbers);
        osp_set_inflection(g_serve_inflection);
    }
    return 0;
}

/* UTF-8 to MacRoman for text bound for an engine, as the Python decodes the
 * request with "replace" and the engines encode with "replace".  A malformed
 * sequence becomes one `?` there and one per byte here; noted, not chased.
 * Size-needed contract. */
OSP_API int osp_text_macroman(const char *utf8, unsigned char *out, int cap)
{
    int need = (int)strlen(utf8);
    unsigned char *tmp = (unsigned char *)malloc((size_t)need + 1);
    int n;
    if (!tmp) return -1;
    n = eng_macroman(utf8, tmp, need + 1);
    if (out && cap > 0) memcpy(out, tmp, (size_t)(n < cap ? n : cap));
    free(tmp);
    return n;
}

/* One utterance: settings, translate, render, stream.  Mirrors the serve
 * script's per-request block, including the response before any audio. */
static void serve_one(const ServeReq *r)
{
    unsigned char hdr[8], tail[4];
    int status = 0;
    if (serve_ensure_voice(r->voice) != 0) status = 1;
    else {
        osp_set_rate(r->rate < 0 ? 0 : r->rate > 100 ? 100 : r->rate);
        osp_set_pitch(r->pitch < 0 ? 0 : r->pitch > 100 ? 100 : r->pitch);
        osp_set_volume(r->volume < 0 ? 0 : r->volume > 100 ? 100 : r->volume);
        osp_set_volume_offset(0);
    }
    serve_put_u32(hdr, SERVE_RSP);
    serve_put_u32(hdr + 4, (unsigned)status);
    if (!serve_write(hdr, 8)) return;
    if (status) return;

    {
        unsigned char *mac = (unsigned char *)malloc((size_t)r->textlen + 1);
        unsigned char *prepared;
        int maclen, need;
        static unsigned char piece[SERVE_CHUNK];
        static short wide[SERVE_CHUNK];
        if (!mac) goto done;
        maclen = eng_macroman((const char *)r->text, mac, r->textlen + 1);
        /* The driver's _flush: nothing but whitespace is nothing to say. */
        {
            const unsigned char *s = mac; int n = maclen;
            eng_strip(&s, &n);
            if (n <= 0) { free(mac); goto done; }
        }
        osp_apply_settings(0, 0);
        need = osp_engine_translate(mac, maclen, NULL, 0);
        if (need < 0) { free(mac); goto done; }
        prepared = (unsigned char *)malloc((size_t)need + 1);
        if (!prepared) { free(mac); goto done; }
        osp_engine_translate(mac, maclen, prepared, need + 1);
        free(mac);
        if (osp_engine_speak_start(prepared, need) == 0) {
            for (;;) {
                int n, i, frames;
                unsigned char chdr[4];
                if (g_serve.cancel_now) osp_engine_cancel();
                n = osp_engine_pull(piece, SERVE_CHUNK);
                if (n <= 0) break;
                frames = osp_pcm_widen(piece, n, wide, SERVE_CHUNK);
                serve_put_u32(chdr, (unsigned)frames);
                if (!serve_write(chdr, 4)) break;
                /* little-endian on the wire, whatever the host is */
                {
                    static unsigned char raw[SERVE_CHUNK * 2];
                    for (i = 0; i < frames; i++) { raw[2 * i] = (unsigned char)wide[i]; raw[2 * i + 1] = (unsigned char)((unsigned short)wide[i] >> 8); }
                    if (!serve_write(raw, frames * 2)) break;
                }
            }
        }
        free(prepared);
    }
done:
    serve_put_u32(tail, 0);
    serve_write(tail, 4);
}

/* `--list`: one voice per line, "id<TAB>label", for token registration. */
OSP_API int osp_serve_list(const char *roots, int out_fd)
{
    int i, n = osp_catalogue_scan(roots);
    char entry[2048];
    if (n < 0) return 1;
    for (i = 0; i < n; i++) {
        char *tab2;
        if (osp_catalogue_entry(i, entry, sizeof entry) < 0) continue;
        tab2 = strchr(entry, '\t');
        if (tab2) tab2 = strchr(tab2 + 1, '\t');
        if (tab2) *tab2 = 0;
        osp_plat_write_all(out_fd, entry, (int)strlen(entry));
        osp_plat_write_all(out_fd, "\n", 1);
    }
    return 0;
}

/* Serve until the input closes.  `in_fd` carries requests; `out_fd` is the
 * private duplicate of stdout the caller claimed.  -> the exit status. */
OSP_API int osp_serve_run(const char *roots, int in_fd, int out_fd)
{
    memset(&g_serve, 0, sizeof g_serve);
    g_serve.in_fd = in_fd; g_serve.out_fd = out_fd;
    g_serve.cur_entry = -1;
    if (osp_catalogue_scan(roots) < 0) return 1;
    if (osp_plat_thread_start(serve_reader, NULL) != 0) return 1;
    for (;;) {
        ServeReq r;
        int have = 0;
        osp_plat_lock();
        if (g_serve.qn) { r = g_serve.queue[0]; memmove(g_serve.queue, g_serve.queue + 1, sizeof(ServeReq) * (size_t)(g_serve.qn - 1)); g_serve.qn--; have = 1; }
        osp_plat_unlock();
        if (!have) {
            if (g_serve.eof) break;
            osp_plat_sleep_ms(2);
            continue;
        }
        osp_plat_lock();
        if (serve_cancelled_has(r.seq)) {
            /* Cancelled before it rendered: an empty, well-formed response
             * keeps the protocol in step. */
            unsigned char hdr[12];
            serve_cancelled_drop(r.seq);
            osp_plat_unlock();
            serve_put_u32(hdr, SERVE_RSP); serve_put_u32(hdr + 4, 0); serve_put_u32(hdr + 8, 0);
            serve_write(hdr, 12);
            free(r.text);
            continue;
        }
        g_serve.current = r.seq;
        g_serve.cancel_now = 0;
        osp_plat_unlock();
        serve_one(&r);
        osp_plat_lock();
        g_serve.current = 0;
        serve_cancelled_drop(r.seq);
        osp_plat_unlock();
        free(r.text);
    }
    osp_engine_close();
    return 0;
}
