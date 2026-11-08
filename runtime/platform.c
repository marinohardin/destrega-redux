/* Host platform shims, see platform.h. */
#include "platform.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#endif

/* ---- paths ------------------------------------------------------------------------------- */

bool plat_is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

const char *plat_last_sep(const char *path)
{
    const char *r = NULL;
    for (const char *p = path; *p; p++)
        if (plat_is_sep(*p)) r = p;
    return r;
}

bool plat_is_absolute(const char *p)
{
#ifdef _WIN32
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && plat_is_sep(p[2])) return true;
    return plat_is_sep(p[0]);   /* \\server\share or \rooted */
#else
    return p[0] == '/';
#endif
}

#ifdef _WIN32
/* UTF-8 -> UTF-16, malloc'd (NULL on failure) */
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}
#endif

bool plat_replace_file(const char *src, const char *dst)
{
#ifdef _WIN32
    wchar_t *ws = wide(src), *wd = wide(dst);
    bool ok = ws && wd && MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(ws);
    free(wd);
    return ok;
#else
    return rename(src, dst) == 0;
#endif
}

void plat_fsync(FILE *f)
{
    if (!f) return;
    fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
}

bool plat_same_file(const char *a, const char *b)
{
    if (!strcmp(a, b)) return true;
#ifdef _WIN32
    wchar_t *wa = wide(a), *wb = wide(b);
    wchar_t fa[4096], fb[4096];
    bool ok = wa && wb && _wfullpath(fa, wa, 4096) && _wfullpath(fb, wb, 4096);
    if (ok) {   /* NTFS names are case-insensitive; normalise separators too */
        for (wchar_t *p = fa; *p; p++) if (*p == L'/') *p = L'\\';
        for (wchar_t *p = fb; *p; p++) if (*p == L'/') *p = L'\\';
        ok = CompareStringOrdinal(fa, -1, fb, -1, TRUE) == CSTR_EQUAL;
    }
    free(wa);
    free(wb);
    return ok;
#else
    char ra[PATH_MAX], rb[PATH_MAX];
    return realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb);
#endif
}

/* ---- environment and processes ------------------------------------------------------------ */

void plat_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void plat_unsetenv(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");   /* an empty value removes the variable */
#else
    unsetenv(name);
#endif
}

bool plat_executable_path(char *out, size_t n)
{
#ifdef _WIN32
    wchar_t w[4096];
    DWORD k = GetModuleFileNameW(NULL, w, 4096);
    if (k == 0 || k >= 4096) return false;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)n;
    char tmp[PATH_MAX];
    if (_NSGetExecutablePath(out, &sz) != 0) return false;
    if (realpath(out, tmp)) snprintf(out, n, "%s", tmp);
    return true;
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) return false;
    out[k] = 0;
    return true;
#endif
}

#ifdef _WIN32
/* Append one argument to a CreateProcess command line, quoted the way the MS C runtime
 * parses it back (backslashes are literal except before a quote). */
static void append_arg(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *a)
{
    size_t need = *len + 2 * wcslen(a) + 4;
    if (need > *cap) {
        while (*cap < need) *cap = *cap ? *cap * 2 : 1024;
        *buf = realloc(*buf, *cap * sizeof **buf);
    }
    wchar_t *o = *buf + *len;
    if (*len) *o++ = L' ';
    bool q = !*a || wcspbrk(a, L" \t\n\v\"") != NULL;
    if (q) *o++ = L'"';
    for (const wchar_t *p = a;; p++) {
        size_t bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (!*p) {   /* double trailing backslashes before the closing quote */
            for (size_t i = 0; i < (q ? 2 * bs : bs); i++) *o++ = L'\\';
            break;
        }
        if (*p == L'"') { for (size_t i = 0; i < 2 * bs + 1; i++) *o++ = L'\\'; }
        else for (size_t i = 0; i < bs; i++) *o++ = L'\\';
        *o++ = *p;
    }
    if (q) *o++ = L'"';
    *o = 0;
    *len = (size_t)(o - *buf);
}
#endif

void plat_exec(const char *exe, char **argv)
{
#ifdef _WIN32
    wchar_t *cmd = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; argv[i]; i++) {
        wchar_t *w = wide(argv[i]);
        if (!w) { free(cmd); return; }
        append_arg(&cmd, &len, &cap, w);
        free(w);
    }
    wchar_t *wexe = wide(exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    fflush(NULL);
    BOOL ok = wexe && cmd && CreateProcessW(wexe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    free(wexe);
    if (!ok) return;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    exit((int)code);
#else
    execv(exe, argv);
#endif
}

/* ---- timing and CPU topology --------------------------------------------------------------- */

uint64_t plat_thread_cpu_ns(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime;
    uint64_t ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100u;
#else
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

int plat_perf_cpus(void)
{
    long cores = 0;
#if defined(__APPLE__)
    int v = 0;
    size_t sz = sizeof v;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &v, &sz, NULL, 0) == 0) cores = v;
    if (cores <= 0) cores = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_WIN32)
    /* hybrid CPUs: count the logical processors of the cores with the highest EfficiencyClass
     * (byte 1 of PROCESSOR_RELATIONSHIP; older SDK headers call it Reserved) */
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    unsigned char *buf = len ? malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        int best = -1;
        for (int pass = 0; pass < 2; pass++)
            for (DWORD off = 0; off < len;) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
                int cls = ((const unsigned char *)&e->Processor)[1];
                if (pass == 0 && cls > best) best = cls;
                if (pass == 1 && cls == best)
                    for (WORD g = 0; g < e->Processor.GroupCount; g++)
                        cores += __builtin_popcountll((unsigned long long)e->Processor.GroupMask[g].Mask);
                off += e->Size;
            }
    }
    free(buf);
    if (cores <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cores = (long)si.dwNumberOfProcessors;
    }
#else
    cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return cores > 0 ? (int)cores : 1;
}

/* ---- threads ------------------------------------------------------------------------------- */

typedef struct { void (*fn)(void *); void *arg; } ThreadStart;

#ifdef _WIN32
static unsigned __stdcall thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

bool plat_thread_start(plat_thread *t, void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn;
    s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_entry, s, 0, NULL);
    if (!h) { free(s); return false; }
    *t = (void *)h;
    return true;
}

void plat_thread_join(plat_thread t)
{
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
}

void plat_mutex_lock(plat_mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->p); }
void plat_mutex_unlock(plat_mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->p); }
void plat_cond_wait(plat_cond *c, plat_mutex *m)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->p, (PSRWLOCK)&m->p, INFINITE, 0);
}
void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->p); }
void plat_thread_boost(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
#else
static void *thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

bool plat_thread_start(plat_thread *t, void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, thread_entry, s) != 0) { free(s); return false; }
    return true;
}

void plat_thread_join(plat_thread t) { pthread_join(t, NULL); }
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
void plat_cond_wait(plat_cond *c, plat_mutex *m) { pthread_cond_wait(c, m); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(c); }
void plat_thread_boost(void)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
#endif
