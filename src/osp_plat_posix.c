/* osp_plat_posix.c -- osp_plat.h on Linux and everything else POSIX. */
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include "osp_plat.h"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct { void (*fn)(void *); void *arg; } Start;

static void *trampoline(void *p)
{
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int osp_plat_thread_start(void (*fn)(void *), void *arg)
{
    Start *s = (Start *)malloc(sizeof *s);
    pthread_t t;
    pthread_attr_t a;
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, trampoline, s) != 0) { pthread_attr_destroy(&a); free(s); return -1; }
    pthread_attr_destroy(&a);
    return 0;
}

void osp_plat_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms < 0) ms = 0;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void osp_plat_lock(void)   { pthread_mutex_lock(&g_mutex); }
void osp_plat_unlock(void) { pthread_mutex_unlock(&g_mutex); }

void osp_plat_stdio_binary(void) { }

int osp_plat_claim_stdout(void)
{
    int fd = dup(1);
    if (fd < 0) return -1;
    fflush(stdout);
    if (dup2(2, 1) < 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); close(nul); }
    }
    return fd;
}

int osp_plat_read_exact(int fd, void *p, int n)
{
    char *q = (char *)p;
    while (n > 0) {
        ssize_t got = read(fd, q, (size_t)n);
        if (got <= 0) return 0;
        q += got; n -= (int)got;
    }
    return 1;
}

int osp_plat_write_all(int fd, const void *p, int n)
{
    const char *q = (const char *)p;
    while (n > 0) {
        ssize_t put = write(fd, q, (size_t)n);
        if (put <= 0) return 0;
        q += put; n -= (int)put;
    }
    return 1;
}

int osp_plat_registry_string(int machine, int view, const char *key,
                             const char *name, char *out, int cap)
{
    (void)machine; (void)view; (void)key; (void)name; (void)out; (void)cap;
    return 0;
}

int osp_plat_env(const char *name, char *out, int cap)
{
    const char *v = getenv(name);
    if (!v || !*v || (int)strlen(v) >= cap) return 0;
    strcpy(out, v);
    return 1;
}

int osp_plat_exe_dir(char *out, int cap)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *slash;
    if (n <= 0) return 0;
    buf[n] = 0;
    slash = strrchr(buf, '/');
    if (slash) *slash = 0;
    if ((int)strlen(buf) >= cap) return 0;
    strcpy(out, buf);
    return 1;
}

/* os.path.abspath without resolving links: absolute, `.`/`..` folded,
 * duplicate separators collapsed, no trailing separator. */
int osp_plat_normkey(const char *path, char *out, int cap)
{
    char work[PATH_MAX * 2];
    char *parts[512];
    int nparts = 0, i;
    size_t n = 0;
    if (path[0] != '/') {
        if (!getcwd(work, sizeof work)) return 0;
        n = strlen(work);
        if (n + 1 + strlen(path) + 1 >= sizeof work) return 0;
        work[n++] = '/';
        strcpy(work + n, path);
    } else {
        if (strlen(path) + 1 >= sizeof work) return 0;
        strcpy(work, path);
    }
    {
        char *save = NULL, *tok = strtok_r(work, "/", &save);
        while (tok) {
            if (!strcmp(tok, ".")) { }
            else if (!strcmp(tok, "..")) { if (nparts) nparts--; }
            else if (nparts < 512) parts[nparts++] = tok;
            tok = strtok_r(NULL, "/", &save);
        }
    }
    n = 0;
    if (cap < 2) return 0;
    if (!nparts) { strcpy(out, "/"); return 1; }
    for (i = 0; i < nparts; i++) {
        size_t l = strlen(parts[i]);
        if (n + 1 + l + 1 > (size_t)cap) return 0;
        out[n++] = '/';
        memcpy(out + n, parts[i], l);
        n += l;
    }
    out[n] = 0;
    return 1;
}
