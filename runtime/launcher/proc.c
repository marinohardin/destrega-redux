#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct Proc { HANDLE h; };

static void append_quoted(wchar_t *cmd, size_t cap, const char *arg)
{
    size_t n = wcslen(cmd);
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, 4096)) w[0] = 0;
    if (n && n + 1 < cap) cmd[n++] = L' ';
    bool q = !w[0] || wcspbrk(w, L" \t\"");
    if (q && n + 1 < cap) cmd[n++] = L'"';
    for (wchar_t *p = w; *p && n + 4 < cap; p++) {
        size_t bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (!*p) { for (size_t k = 0; k < (q ? 2 * bs : bs) && n + 2 < cap; k++) cmd[n++] = L'\\'; break; }
        if (*p == L'"') { for (size_t k = 0; k < 2 * bs + 1 && n + 2 < cap; k++) cmd[n++] = L'\\'; }
        else for (size_t k = 0; k < bs && n + 2 < cap; k++) cmd[n++] = L'\\';
        cmd[n++] = *p;
    }
    if (q && n + 1 < cap) cmd[n++] = L'"';
    cmd[n] = 0;
}

Proc *proc_start(char *const argv[], const char *log_path, char *err, int errn)
{
    static wchar_t cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_quoted(cmd, 32768, argv[i]);
    wchar_t wlog[4096];
    MultiByteToWideChar(CP_UTF8, 0, log_path, -1, wlog, 4096);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileW(wlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { snprintf(err, (size_t)errn, "cannot write %s", log_path); return NULL; }
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(out);
    if (!ok) { snprintf(err, (size_t)errn, "cannot run %s (Windows error %lu)", argv[0], (unsigned long)GetLastError()); return NULL; }
    CloseHandle(pi.hThread);
    Proc *p = malloc(sizeof *p);
    p->h = pi.hProcess;
    return p;
}
bool proc_poll(Proc *p, int *code)
{
    if (WaitForSingleObject(p->h, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(p->h, &c);
    *code = (int)c;
    return true;
}
void proc_free(Proc *p) { if (p) { CloseHandle(p->h); free(p); } }
int proc_wait(Proc *p)
{
    WaitForSingleObject(p->h, INFINITE);
    int c = -1;
    proc_poll(p, &c);
    proc_free(p);
    return c;
}
void proc_kill(Proc *p)
{
    TerminateProcess(p->h, 1);
    proc_wait(p);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

struct Proc { pid_t pid; };

Proc *proc_start(char *const argv[], const char *log_path, char *err, int errn)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r) { snprintf(err, (size_t)errn, "cannot run %s (%s)", argv[0], strerror(r)); return NULL; }
    Proc *p = malloc(sizeof *p);
    p->pid = pid;
    return p;
}
static int decode(int st) { return WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
bool proc_poll(Proc *p, int *code)
{
    int st;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == 0) return false;
    *code = r < 0 ? -1 : decode(st);
    return true;
}
void proc_free(Proc *p) { free(p); }
int proc_wait(Proc *p)
{
    int st, c = -1;
    pid_t r;
    do r = waitpid(p->pid, &st, 0); while (r < 0 && errno == EINTR);
    if (r > 0) c = decode(st);
    free(p);
    return c;
}
void proc_kill(Proc *p)
{
    kill(p->pid, SIGKILL);
    proc_wait(p);
}
#endif

char *proc_capture(char *const argv[], const char *tmp_path, int *code)
{
    remove(tmp_path);
    char err[256];
    Proc *p = proc_start(argv, tmp_path, err, sizeof err);
    if (!p) return NULL;
    *code = proc_wait(p);
    FILE *f = fopen(tmp_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    size_t got = s ? fread(s, 1, (size_t)n, f) : 0;
    fclose(f);
    remove(tmp_path);
    if (!s) return NULL;
    s[got] = 0;
    return s;
}
