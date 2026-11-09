/* Host platform shims: the few things POSIX and Windows do differently (files, processes,
 * threads, CPU topology). Everything else in the runtime is plain C11 + SDL3.
 *
 * Paths are UTF-8 everywhere. On Windows the executable carries a manifest with
 * activeCodePage = UTF-8 (runtime/win/destrega.manifest), so fopen/rename/remove take UTF-8
 * as well; the functions here convert to UTF-16 themselves and do not depend on it. */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Move src over dst, replacing dst if it exists (atomic on POSIX; MoveFileEx with
 * MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows, where rename() refuses to
 * replace). */
bool plat_replace_file(const char *src, const char *dst);
/* Flush a written file to disk before it is moved into place (fsync / _commit). */
void plat_fsync(FILE *f);
/* Whether two paths name the same file (realpath / _fullpath + case-insensitive compare). */
bool plat_same_file(const char *a, const char *b);
/* Path separators: '/' everywhere, also '\\' on Windows. */
bool plat_is_sep(char c);
const char *plat_last_sep(const char *path);   /* last separator in path, or NULL */
bool plat_is_absolute(const char *path);       /* "/x", or "C:\x", "\\server" on Windows */

void plat_setenv(const char *name, const char *value);
void plat_unsetenv(const char *name);
/* Absolute path of the running executable. */
bool plat_executable_path(char *out, size_t n);
/* Replace the process with exe argv[] (argv[0] = exe, NULL-terminated). POSIX: execv.
 * Windows: start it with the same console/handles, wait for it and exit with its code (so a
 * caller waiting on this process sees the restarted one). Returns only on failure. */
void plat_exec(const char *exe, char **argv);

/* localtime_r / localtime_s */
void plat_localtime(const time_t *t, struct tm *out);

/* Monotonic clock, nanoseconds. */
uint64_t plat_mono_ns(void);
void plat_sleep_us(uint64_t us);
/* Return addresses of the calling thread's host call chain, innermost first, starting at the
 * caller of plat_backtrace (execinfo backtrace / CaptureStackBackTrace; 0 where unsupported).
 * Save states compare it to know whether a snapshot can be restored here. */
int plat_backtrace(void **pc, int max);
/* This machine's IPv4 addresses (up interfaces, no loopback) as "a, b, ..." for the Host Game
 * dialog (getifaddrs / GetAdaptersAddresses). Returns how many were written. */
int plat_local_ipv4(char *out, size_t n);
/* The IPv4 default gateway (the home router) as "a.b.c.d": macOS sysctl route table, Linux
 * /proc/net/route, Windows GetIpForwardTable. Local lookup only, nothing is sent. */
bool plat_default_gateway_ipv4(char *out, size_t n);

/* CPU time of the calling thread, nanoseconds. */
uint64_t plat_thread_cpu_ns(void);
/* Logical CPUs of the fastest core class (macOS: performance cores; Windows: the highest
 * EfficiencyClass; elsewhere: all online CPUs). At least 1. */
int plat_perf_cpus(void);

/* Minimal threads for the hi-res worker pool (pthreads, or Win32 SRW locks / condition
 * variables). Statically initialisable with PLAT_MUTEX_INIT / PLAT_COND_INIT. */
#ifdef _WIN32
typedef struct { void *p; } plat_mutex;   /* SRWLOCK */
typedef struct { void *p; } plat_cond;    /* CONDITION_VARIABLE */
typedef void *plat_thread;                /* HANDLE */
#define PLAT_MUTEX_INIT { 0 }
#define PLAT_COND_INIT { 0 }
#else
#include <pthread.h>
typedef pthread_mutex_t plat_mutex;
typedef pthread_cond_t plat_cond;
typedef pthread_t plat_thread;
#define PLAT_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define PLAT_COND_INIT PTHREAD_COND_INITIALIZER
#endif
bool plat_thread_start(plat_thread *t, void (*fn)(void *), void *arg);
void plat_thread_join(plat_thread t);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);
void plat_cond_wait(plat_cond *c, plat_mutex *m);
void plat_cond_broadcast(plat_cond *c);
/* Raise the calling worker thread's scheduling class (macOS QoS user-interactive). */
void plat_thread_boost(void);

/* Spin-wait hint. */
static inline void plat_cpu_relax(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#endif
}
#endif
