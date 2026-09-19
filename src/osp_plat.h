/* osp_plat.h -- the little the host needs from the operating system.
 *
 * Threads, a mutex, binary standard streams, claiming stdout, the registry
 * where there is one, the environment, and where the executable lives.
 * Implemented in osp_plat_win.c and osp_plat_posix.c, which are their own
 * translation units on purpose: <windows.h> declares names this host uses
 * for its own types, so it can never be included into osp_host.c, and the
 * POSIX half needs headers the host does not.  Everything here is plain C
 * with no host types, so either implementation links against the one
 * host.
 */
#ifndef OSP_PLAT_H
#define OSP_PLAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start a detached thread running fn(arg). -> 0, or -1 */
int  osp_plat_thread_start(void (*fn)(void *), void *arg);
void osp_plat_sleep_ms(int ms);

/* One process-wide mutex, which is all a serve loop needs. */
void osp_plat_lock(void);
void osp_plat_unlock(void);

/* Put fd 0 and fd 1 into binary mode (a no-op off Windows). */
void osp_plat_stdio_binary(void);

/* The protocol keeps the pipe; stdout stops being it.  Duplicates fd 1 and
 * returns the duplicate, then points fd 1 at stderr, or at the null device
 * when there is no stderr to have -- so a stray print anywhere lands where
 * chatter belongs and cannot corrupt the stream.  -> the fd, or -1. */
int  osp_plat_claim_stdout(void);

/* Read exactly n bytes. -> 1, or 0 at end of file / error. */
int  osp_plat_read_exact(int fd, void *p, int n);
/* Write all n bytes. -> 1, or 0 on error. */
int  osp_plat_write_all(int fd, const void *p, int n);

/* A REG_SZ from HKCU (machine=0) or HKLM (machine=1), in the 64-bit
 * (view=64), 32-bit (view=32) or default (view=0) registry view.  UTF-8 out.
 * -> 1 with the value, or 0 when absent, empty, not a string, or on a
 * platform with no registry. */
int  osp_plat_registry_string(int machine, int view, const char *key,
                              const char *name, char *out, int cap);

/* getenv, UTF-8 on every platform. -> 1 with the value, or 0. */
int  osp_plat_env(const char *name, char *out, int cap);

/* The folder the running executable is in, UTF-8, no trailing separator. */
int  osp_plat_exe_dir(char *out, int cap);

/* os.path.abspath, UTF-8: absolute, separators normalised, `.` and `..`
 * folded, no trailing separator; on Windows also lower-cased, which is
 * os.path.normcase.  -> 1, or 0. */
int  osp_plat_normkey(const char *path, char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* OSP_PLAT_H */
