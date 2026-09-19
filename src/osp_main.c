/* osp_main.c -- the host as a program: serve mode, a voice listing, a file
 * renderer, and a capabilities report.  Uses nothing but the public API, so
 * it is also the first C caller that proves the API is enough.
 *
 *   osp_host --serve <config>           the SAPI bridge's protocol on stdio
 *   osp_host --list  <config>           "id<TAB>label" per voice
 *   osp_host --render --voice ID --text T [--output F] [--rate R] ...
 *   osp_host --capabilities             JSON on stdout
 *
 * <config> is what the Python serve was launched with: the folder whose
 * macintalk/outspoken holds the data (NVDA's configuration path, or the SAPI
 * data root).  --root adds a folder to search for --render and --list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osp_host.h"
#include "osp_plat.h"

#define HOST_VERSION "2.0.0-dev"

static void usage(void)
{
    fprintf(stdout,
        "Usage:\n"
        "  osp_host --serve <config>            serve the SAPI bridge's protocol on stdin/stdout\n"
        "  osp_host --list <config>             one voice per line, id<TAB>label\n"
        "  osp_host --render --voice ID [--text T | --input F] [--output F.wav]\n"
        "                   [--rate 0-100] [--pitch 0-100] [--volume 0-100]\n"
        "                   [--numbers off|words|digits] [--config C] [--root DIR]...\n"
        "  osp_host --capabilities              what this build can do, as JSON\n"
        "Nothing of Apple's or Berkeley's is inside this program; point it at your own\n"
        "extracted engine data.\n");
}

static char *roots_for(const char *config, char extra[][1024], int nextra)
{
    char exe[1024], *out;
    int need, i;
    if (!osp_plat_exe_dir(exe, sizeof exe)) exe[0] = 0;
    need = osp_roots_default(config, exe, NULL, 0);
    if (need < 0) need = 0;
    out = (char *)malloc((size_t)need + 1 + (size_t)nextra * 1025);
    if (!out) return NULL;
    osp_roots_default(config, exe, out, need + 1);
    for (i = 0; i < nextra; i++) { strcat(out, extra[i]); strcat(out, "\n"); }
    return out;
}

static int capabilities(void)
{
    printf("{\"host\": \"osp_host\", \"version\": \"%s\", "
           "\"engines\": [\"sp\", \"mtk2\", \"mtk3\", \"gala\", \"cami\"], "
           "\"streaming\": true, \"cancel\": \"in-band\", \"sample_rate\": 22254, "
           "\"protocol\": \"OSP4\"}\n", HOST_VERSION);
    return 0;
}

static int render(int argc, char **argv)
{
    const char *voice = NULL, *text = NULL, *input = NULL, *output = "osp-out.wav", *config = "";
    int rate = 50, pitch = 50, volume = 100, numbers = 1, i, n, idx = -1;
    char extra[8][1024]; int nextra = 0;
    char *roots, entry[2048], *manifest;
    unsigned char *utf8 = NULL, *mac, *prepared;
    int maclen, need;
    FILE *out;
    unsigned long total = 0;

    for (i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--voice") && v) { voice = v; i++; }
        else if (!strcmp(a, "--text") && v) { text = v; i++; }
        else if (!strcmp(a, "--input") && v) { input = v; i++; }
        else if (!strcmp(a, "--output") && v) { output = v; i++; }
        else if (!strcmp(a, "--config") && v) { config = v; i++; }
        else if (!strcmp(a, "--root") && v) { if (nextra < 8) { strncpy(extra[nextra], v, 1023); extra[nextra][1023] = 0; nextra++; } i++; }
        else if (!strcmp(a, "--rate") && v) { rate = atoi(v); i++; }
        else if (!strcmp(a, "--pitch") && v) { pitch = atoi(v); i++; }
        else if (!strcmp(a, "--volume") && v) { volume = atoi(v); i++; }
        else if (!strcmp(a, "--numbers") && v) { numbers = !strcmp(v, "off") ? 0 : !strcmp(v, "digits") ? 2 : 1; i++; }
        else { fprintf(stderr, "unknown argument %s\n", a); return 2; }
    }
    roots = roots_for(config, extra, nextra);
    if (!roots) return 1;
    n = osp_catalogue_scan(roots);
    free(roots);
    if (n <= 0) { fprintf(stderr, "no voices found; point --config or --root at your extracted engine data\n"); return 1; }
    for (i = 0; i < n; i++) {
        char *tab;
        if (osp_catalogue_entry(i, entry, sizeof entry) < 0) continue;
        tab = strchr(entry, '\t'); if (tab) *tab = 0;
        if (!voice || !strcmp(entry, voice)) { idx = i; break; }
    }
    if (idx < 0) { fprintf(stderr, "no such voice: %s (try --list)\n", voice); return 1; }
    need = osp_catalogue_manifest(idx, NULL, 0);
    manifest = (char *)malloc((size_t)need + 1);
    if (!manifest) return 1;
    osp_catalogue_manifest(idx, manifest, need + 1);
    if (osp_engine_open(manifest) != 0) { fprintf(stderr, "cannot open the engine: %s\n", osp_engine_error()); free(manifest); return 1; }
    free(manifest);

    if (text) {
        utf8 = (unsigned char *)malloc(strlen(text) + 1);
        if (!utf8) return 1;
        strcpy((char *)utf8, text);
    } else {
        FILE *f = (input && strcmp(input, "-")) ? fopen(input, "rb") : stdin;
        size_t cap = 1 << 16, len = 0;
        if (!f) { fprintf(stderr, "cannot read %s\n", input); return 1; }
        utf8 = (unsigned char *)malloc(cap);
        if (!utf8) return 1;
        for (;;) {
            size_t got = fread(utf8 + len, 1, cap - len - 1, f);
            len += got;
            if (got == 0) break;
            if (len + 1 >= cap) { unsigned char *q = (unsigned char *)realloc(utf8, cap *= 2); if (!q) return 1; utf8 = q; }
        }
        utf8[len] = 0;
        if (f != stdin) fclose(f);
    }

    osp_engine_set_numbers(numbers);
    osp_set_inflection(50);
    osp_set_rate(rate); osp_set_pitch(pitch); osp_set_volume(volume);
    osp_apply_settings(0, 0);
    maclen = osp_text_macroman((const char *)utf8, NULL, 0);
    mac = (unsigned char *)malloc((size_t)maclen + 1);
    if (!mac) return 1;
    osp_text_macroman((const char *)utf8, mac, maclen + 1);
    free(utf8);
    need = osp_engine_translate(mac, maclen, NULL, 0);
    if (need < 0) { fprintf(stderr, "translate failed\n"); return 1; }
    prepared = (unsigned char *)malloc((size_t)need + 1);
    if (!prepared) return 1;
    osp_engine_translate(mac, maclen, prepared, need + 1);
    free(mac);

    out = strcmp(output, "-") ? fopen(output, "wb") : stdout;
    if (!out) { fprintf(stderr, "cannot write %s\n", output); return 1; }
    /* A WAV header with the sizes patched in at the end, unless the output is
     * a pipe, where the sizes are left at their maximum. */
    {
        unsigned char h[44] = { 'R','I','F','F', 0xFF,0xFF,0xFF,0xFF, 'W','A','V','E', 'f','m','t',' ',
                                16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0xFF,0xFF,0xFF,0xFF };
        unsigned rate_hz = 22254, byte_rate = rate_hz * 2;
        h[24] = (unsigned char)rate_hz; h[25] = (unsigned char)(rate_hz >> 8); h[26] = (unsigned char)(rate_hz >> 16);
        h[28] = (unsigned char)byte_rate; h[29] = (unsigned char)(byte_rate >> 8); h[30] = (unsigned char)(byte_rate >> 16);
        fwrite(h, 1, 44, out);
    }
    if (osp_engine_speak_start(prepared, need) == 0) {
        static unsigned char piece[8192];
        static short wide[8192];
        static unsigned char raw[16384];
        for (;;) {
            int got = osp_engine_pull(piece, sizeof piece), frames, k;
            if (got <= 0) break;
            frames = osp_pcm_widen(piece, got, wide, 8192);
            for (k = 0; k < frames; k++) { raw[2 * k] = (unsigned char)wide[k]; raw[2 * k + 1] = (unsigned char)((unsigned short)wide[k] >> 8); }
            fwrite(raw, 1, (size_t)frames * 2, out);
            total += (unsigned long)frames * 2;
        }
    }
    free(prepared);
    if (out != stdout) {
        unsigned char sz[4];
        unsigned long riff = total + 36;
        sz[0] = (unsigned char)riff; sz[1] = (unsigned char)(riff >> 8); sz[2] = (unsigned char)(riff >> 16); sz[3] = (unsigned char)(riff >> 24);
        fseek(out, 4, SEEK_SET); fwrite(sz, 1, 4, out);
        sz[0] = (unsigned char)total; sz[1] = (unsigned char)(total >> 8); sz[2] = (unsigned char)(total >> 16); sz[3] = (unsigned char)(total >> 24);
        fseek(out, 40, SEEK_SET); fwrite(sz, 1, 4, out);
        fclose(out);
        fprintf(stderr, "%lu frames -> %s\n", total / 2, output);
    }
    osp_engine_close();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "--capabilities")) return capabilities();
    if (!strcmp(argv[1], "--render")) return render(argc, argv);
    if (!strcmp(argv[1], "--list") || !strcmp(argv[1], "--serve")) {
        const char *config = argc > 2 ? argv[2] : "";
        char *roots = roots_for(config, NULL, 0);
        int rc;
        if (!roots) return 1;
        if (!strcmp(argv[1], "--list")) {
            osp_plat_stdio_binary();
            rc = osp_serve_list(roots, 1);
        } else {
            int out;
            osp_plat_stdio_binary();
            out = osp_plat_claim_stdout();
            if (out < 0) { free(roots); return 1; }
            rc = osp_serve_run(roots, 0, out);
        }
        free(roots);
        return rc;
    }
    usage();
    return 2;
}
