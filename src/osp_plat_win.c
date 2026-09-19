/* osp_plat_win.c -- osp_plat.h on Win32.  Its own translation unit, so
 * <windows.h> never meets the host's own names. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "osp_plat.h"

static CRITICAL_SECTION g_cs;
static int g_cs_ready;

static void cs_init(void)
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; }
}

typedef struct { void (*fn)(void *); void *arg; } Start;

static DWORD WINAPI trampoline(LPVOID p)
{
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int osp_plat_thread_start(void (*fn)(void *), void *arg)
{
    Start *s = (Start *)malloc(sizeof *s);
    HANDLE h;
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    h = CreateThread(NULL, 0, trampoline, s, 0, NULL);
    if (!h) { free(s); return -1; }
    CloseHandle(h);
    return 0;
}

void osp_plat_sleep_ms(int ms) { Sleep(ms < 0 ? 0 : (DWORD)ms); }
void osp_plat_lock(void)   { cs_init(); EnterCriticalSection(&g_cs); }
void osp_plat_unlock(void) { LeaveCriticalSection(&g_cs); }

void osp_plat_stdio_binary(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

int osp_plat_claim_stdout(void)
{
    int fd = _dup(1);
    if (fd < 0) return -1;
    _setmode(fd, _O_BINARY);
    fflush(stdout);
    if (_dup2(2, 1) != 0) {
        int nul = _open("NUL", _O_WRONLY);
        if (nul >= 0) { _dup2(nul, 1); _close(nul); }
    }
    return fd;
}

int osp_plat_read_exact(int fd, void *p, int n)
{
    char *q = (char *)p;
    while (n > 0) {
        int got = _read(fd, q, (unsigned)n);
        if (got <= 0) return 0;
        q += got; n -= got;
    }
    return 1;
}

int osp_plat_write_all(int fd, const void *p, int n)
{
    const char *q = (const char *)p;
    while (n > 0) {
        int put = _write(fd, q, (unsigned)n);
        if (put <= 0) return 0;
        q += put; n -= put;
    }
    return 1;
}

static int to_utf8(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    return n > 0;
}

static int to_wide(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

int osp_plat_registry_string(int machine, int view, const char *key,
                             const char *name, char *out, int cap)
{
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, h;
    REGSAM sam = KEY_READ | (view == 64 ? KEY_WOW64_64KEY : view == 32 ? KEY_WOW64_32KEY : 0);
    wchar_t wkey[512], wname[128], wval[2048];
    DWORD type = 0, size = sizeof wval;
    if (!to_wide(key, wkey, 512) || !to_wide(name, wname, 128)) return 0;
    if (RegOpenKeyExW(root, wkey, 0, sam, &h) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExW(h, wname, NULL, &type, (LPBYTE)wval, &size) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(h); return 0;
    }
    RegCloseKey(h);
    wval[size / sizeof(wchar_t)] = 0;
    if (!wval[0]) return 0;
    return to_utf8(wval, out, cap);
}

int osp_plat_env(const char *name, char *out, int cap)
{
    wchar_t wname[128], wval[2048];
    DWORD n;
    if (!to_wide(name, wname, 128)) return 0;
    n = GetEnvironmentVariableW(wname, wval, 2048);
    if (!n || n >= 2048) return 0;
    return to_utf8(wval, out, cap);
}

int osp_plat_exe_dir(char *out, int cap)
{
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    wchar_t *slash;
    if (!n || n >= MAX_PATH) return 0;
    slash = wcsrchr(w, L'\\');
    if (slash) *slash = 0;
    return to_utf8(w, out, cap);
}

int osp_plat_normkey(const char *path, char *out, int cap)
{
    wchar_t w[2048], full[2048];
    size_t n, i;
    if (!to_wide(path, w, 2048)) return 0;
    if (!_wfullpath(full, w, 2048)) return 0;
    n = wcslen(full);
    while (n > 3 && (full[n - 1] == L'\\' || full[n - 1] == L'/')) full[--n] = 0;
    for (i = 0; i < n; i++) full[i] = (wchar_t)towlower(full[i]);
    return to_utf8(full, out, cap);
}
