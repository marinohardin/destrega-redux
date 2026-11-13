/* First-launch launcher: find, or build and load, the game module (launcher.h). */
#include "launcher.h"
#include "embedded.h"
#include "proc.h"
#include "recompiler.h"
#include "sha256.h"

#include "../disc.h"
#include "../game_abi.h"
#include "../game_load.h"
#include "../platform.h"
#include "game_sdk.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define MOD_EXT ".dll"
#define EXE_SUFFIX ".exe"
#elif defined(__APPLE__)
#define MOD_EXT ".dylib"
#define EXE_SUFFIX ""
#else
#define MOD_EXT ".so"
#define EXE_SUFFIX ""
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define TARGET_ARCH "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
#define TARGET_ARCH "x86_64"
#else
#define TARGET_ARCH "other"
#endif
#if defined(_WIN32)
#define TARGET_OS "windows"
#elif defined(__APPLE__)
#define TARGET_OS "macos"
#else
#define TARGET_OS "linux"
#endif

/* Compile flags of every unit: the same as CMake's gen/ library (-O1: the generated code is
 * huge and goto-heavy) plus what a module needs. Part of the key. */
static const char *const k_cflags[] = {
    "-O1", "-w", "-fno-strict-aliasing", "-fvisibility=hidden",
#ifndef _WIN32
    "-fPIC",
#endif
    NULL};

/* ---- options -------------------------------------------------------------------------------- */

static const char *s_opt_module, *s_opt_cc, *s_opt_analysis, *s_opt_recompile, *s_opt_selftest;
static bool s_opt_rebuild;
static int s_opt_jobs;

int launcher_parse_arg(int argc, char **argv, int i)
{
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (!strcmp(a, "--game-module") && more) { s_opt_module = argv[i + 1]; return 2; }
    if (!strcmp(a, "--game-cc") && more) { s_opt_cc = argv[i + 1]; return 2; }
    if (!strcmp(a, "--analysis-dir") && more) { s_opt_analysis = argv[i + 1]; return 2; }
    if (!strcmp(a, "--recompile") && more) { s_opt_recompile = argv[i + 1]; return 2; }
    if (!strcmp(a, "--game-jobs") && more) { s_opt_jobs = atoi(argv[i + 1]); return 2; }
    if (!strcmp(a, "--rebuild-game")) { s_opt_rebuild = true; return 1; }
    if (!strcmp(a, "--game-selftest") && more) { s_opt_selftest = argv[i + 1]; return 2; }
    return 0;
}

void launcher_usage(void)
{
    fprintf(stderr,
            "game module (this build keeps the recompiled game in a module built on first launch):\n"
            "  --game-module PATH    load this game module (no key check, no build)\n"
            "  --game-cc \"CMD ...\"   C compiler for the module (default: the bundled zig cc, else cc / clang / gcc)\n"
            "  --game-jobs N         parallel compiler runs (default: performance cores)\n"
            "  --rebuild-game        build the module even if the one in the data folder is current\n"
            "  --analysis-dir DIR    read the recompiler's analysis inputs from DIR instead of the built-in ones\n"
            "  --recompile OUTDIR    write the generated C (as recomp/recomp.py would) and exit\n"
            "  --game-selftest DIR   test (CI, no disc): build and load a module from the C files in DIR\n"
            "                        instead of the disc's code (needs --data-dir), then exit\n");
}

/* ---- small helpers -------------------------------------------------------------------------- */

/* --game-selftest DIR: the regular files of DIR (C sources, headers and a files.cmake naming the
 * units, like the recompiler's output), sorted by name */
static int cmp_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }
static char **selftest_files(int *n)
{
    int count = 0;
    char **g = SDL_GlobDirectory(s_opt_selftest, NULL, 0, &count);
    *n = 0;
    if (!g) return NULL;
    char **out = calloc((size_t)count + 1, sizeof *out);
    for (int i = 0; i < count; i++) {
        char p[4400];
        SDL_PathInfo info;
        snprintf(p, sizeof p, "%s/%s", s_opt_selftest, g[i]);
        if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE) out[(*n)++] = strdup(g[i]);
    }
    SDL_free(g);
    qsort(out, (size_t)*n, sizeof *out, cmp_str);
    return out;
}
static void free_list(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

static char s_log_path[4096];
static char s_selftest_cc[1200];   /* compiler + where it was found, for the self-test's line */
static FILE *s_log;

static void logf_(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "launcher: %s\n", line);
    if (s_log) { fprintf(s_log, "launcher: %s\n", line); fflush(s_log); }
}

static double now_s(void) { return (double)plat_mono_ns() / 1e9; }

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)(n < 0 ? 0 : n) + 1);
    size_t got = p ? fread(p, 1, (size_t)(n < 0 ? 0 : n), f) : 0;
    fclose(f);
    if (!p) return NULL;
    p[got] = 0;
    if (len) *len = got;
    return p;
}

static bool write_file(const char *path, const void *data, size_t n)
{
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    return ok && plat_replace_file(tmp, path);
}

static const EmbeddedFile *embedded(const char *name)
{
    for (int i = 0; i < g_embedded_count; i++)
        if (!strcmp(g_embedded_files[i].name, name)) return &g_embedded_files[i];
    return NULL;
}

/* the recompiler's analysis inputs: --analysis-dir, else the built-in copy */
static char *analysis_read(const char *rel, size_t *len)
{
    if (s_opt_analysis) {
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", s_opt_analysis, rel);
        return slurp(p, len);
    }
    char name[256];
    snprintf(name, sizeof name, "analysis/%s", rel);
    const EmbeddedFile *e = embedded(name);
    if (!e) return NULL;
    char *p = malloc(e->size + 1);
    memcpy(p, e->data, e->size);
    p[e->size] = 0;
    *len = e->size;
    return p;
}

static uint8_t *rc_disc_file(void *ud, const char *path, uint32_t *size)
{
    (void)ud;
    return disc_load_file(path, size);
}
static char *rc_analysis_file(void *ud, const char *rel, size_t *len) { (void)ud; return analysis_read(rel, len); }
static void rc_log(void *ud, const char *line) { (void)ud; if (s_log) { fprintf(s_log, "recompiler: %s\n", line); fflush(s_log); } }

/* empties a directory this launcher made (files only, one level), creating it if needed */
static bool fresh_dir(const char *dir)
{
    int n = 0;
    char **list = SDL_GlobDirectory(dir, "*", 0, &n);
    if (list) {
        for (int i = 0; i < n; i++) {
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", dir, list[i]);
            SDL_RemovePath(p);
        }
        SDL_free(list);
    }
    return SDL_CreateDirectory(dir);
}

/* ---- the compiler --------------------------------------------------------------------------- */

typedef struct {
    char *argv[8];     /* the command (e.g. {".../zig", "cc"}), NULL-terminated */
    int argc;
    char store[8][2048];
    char version[1024];   /* first line of --version */
    char *version_full;
    bool zig;
    char where[64];       /* "bundled zig", "--game-cc", "system" */
} Compiler;

static bool exe_dir(char *out, size_t n)
{
    if (!plat_executable_path(out, n)) return false;
    const char *sep = plat_last_sep(out);
    if (!sep) return false;
    out[sep - out + 1] = 0;
    return true;
}

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static bool cc_probe(Compiler *c, const char *tmp)
{
    char *argv[10];
    int k = 0;
    for (; k < c->argc; k++) argv[k] = c->argv[k];
    argv[k++] = "--version";
    argv[k] = NULL;
    int code = -1;
    char *out = proc_capture(argv, tmp, &code);
    if (!out || code != 0) { free(out); return false; }
    c->version_full = out;
    size_t l = strcspn(out, "\r\n");
    snprintf(c->version, sizeof c->version, "%.*s", (int)l, out);
    return true;
}

static void cc_set(Compiler *c, const char *const *words, int n, const char *where)
{
    c->argc = n;
    for (int i = 0; i < n; i++) {
        snprintf(c->store[i], sizeof c->store[i], "%s", words[i]);
        c->argv[i] = c->store[i];
    }
    c->argv[n] = NULL;
    snprintf(c->where, sizeof c->where, "%s", where);
    const char *b = plat_last_sep(c->store[0]);
    b = b ? b + 1 : c->store[0];
    c->zig = !strncmp(b, "zig", 3) && n > 1 && !strcmp(c->store[1], "cc");
}

static bool find_compiler(Compiler *c, const char *tmp, char *why, size_t whyn)
{
    memset(c, 0, sizeof *c);
    if (s_opt_cc) {
        char buf[2048], *words[8];
        snprintf(buf, sizeof buf, "%s", s_opt_cc);
        int n = 0;
        for (char *t = strtok(buf, " "); t && n < 7; t = strtok(NULL, " ")) words[n++] = t;
        if (!n) { snprintf(why, whyn, "--game-cc is empty"); return false; }
        cc_set(c, (const char *const *)words, n, "--game-cc");
        if (cc_probe(c, tmp)) return true;
        snprintf(why, whyn, "the compiler given with --game-cc (%s) does not run", s_opt_cc);
        return false;
    }
    char dir[4096], zig[4300];
    const char *env = getenv("DESTREGA_ZIG");
    const char *cands[3];
    int nc = 0;
    char c1[4300], c2[4300];
    if (env && *env) cands[nc++] = env;
    if (exe_dir(dir, sizeof dir)) {
        snprintf(c1, sizeof c1, "%szig/zig" EXE_SUFFIX, dir);              /* zip / AppImage layout */
        snprintf(c2, sizeof c2, "%s../Resources/zig/zig" EXE_SUFFIX, dir); /* macOS .app */
        cands[nc++] = c1;
        cands[nc++] = c2;
    }
    for (int i = 0; i < nc; i++) {
        if (!file_exists(cands[i])) continue;
        snprintf(zig, sizeof zig, "%s", cands[i]);
        const char *w[2] = {zig, "cc"};
        cc_set(c, w, 2, "bundled zig");
        if (cc_probe(c, tmp)) return true;
    }
    static const char *const sys[] = {
#ifdef _WIN32
        "clang", "gcc",
#else
        "cc", "clang", "gcc",
#endif
        NULL};
    for (int i = 0; sys[i]; i++) {
        const char *w[1] = {sys[i]};
        cc_set(c, w, 1, "system");
        if (cc_probe(c, tmp)) return true;
    }
    snprintf(why, whyn,
             "The build tools that come with Destrega (the \"zig\" folder next to the program) are missing, and no "
             "C compiler was found on this computer. Reinstall Destrega%s.",
#ifdef __APPLE__
             ", or install Apple's Command Line Tools (xcode-select --install)"
#else
             ""
#endif
    );
    return false;
}

/* ---- the key -------------------------------------------------------------------------------- */

static void key_str(Sha256 *s, const char *tag, const char *v)
{
    sha256_update(s, tag, strlen(tag) + 1);
    sha256_update(s, v, strlen(v) + 1);
}
static void key_bytes(Sha256 *s, const char *tag, const void *p, size_t n)
{
    uint8_t d[32];
    Sha256 t;
    sha256_init(&t);
    sha256_update(&t, p, n);
    sha256_final(&t, d);
    sha256_update(s, tag, strlen(tag) + 1);
    sha256_update(s, d, 32);
}

static bool compute_key(const Compiler *c, char out[65], char *why, size_t whyn)
{
    Sha256 s;
    sha256_init(&s);
    key_str(&s, "destrega-game-module", "1");
    key_str(&s, "sdk", GAME_ABI_SDK_ID);
    key_str(&s, "recompiler", GAME_RECOMP_ID);
    if (s_opt_selftest) {   /* the self-test's C files stand in for the analysis inputs and the disc */
        int nf = 0;
        char **files = selftest_files(&nf);
        for (int i = 0; i < nf; i++) {
            char p[4400];
            size_t len = 0;
            snprintf(p, sizeof p, "%s/%s", s_opt_selftest, files[i]);
            char *d = slurp(p, &len);
            if (d) key_bytes(&s, files[i], d, len);
            free(d);
        }
        free_list(files, nf);
        if (!nf) { snprintf(why, whyn, "no files in %s", s_opt_selftest); return false; }
        goto compiler;
    }
    const char *names[64];
    int n = recomp_analysis_inputs(names, 64);
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        char *p = analysis_read(names[i], &len);
        if (p) key_bytes(&s, names[i], p, len);
        else key_str(&s, names[i], "(absent)");
        free(p);
    }
    /* the disc files the code comes from: SLUS_007.32 and every overlay */
    static const char *const disc_files[] = {"SLUS_007.32", NULL};
    for (int i = 0; disc_files[i]; i++) {
        u32 size = 0;
        u8 *p = disc_load_file(disc_files[i], &size);
        if (!p) { snprintf(why, whyn, "%s is missing from the disc", disc_files[i]); return false; }
        key_bytes(&s, disc_files[i], p, size);
        free(p);
    }
    for (int i = 5; i < n; i++) {   /* overlays/<NAME>.procs.json -> OVL/<NAME>.BIN */
        char path[96];
        snprintf(path, sizeof path, "OVL/%.*s.BIN", (int)(strlen(names[i]) - 9 - 11), names[i] + 9);
        u32 size = 0;
        u8 *p = disc_load_file(path, &size);
        if (!p) { snprintf(why, whyn, "%s is missing from the disc", path); return false; }
        key_bytes(&s, path, p, size);
        free(p);
    }
compiler:
    for (int i = 0; i < c->argc; i++) key_str(&s, "cc", c->argv[i]);
    key_str(&s, "cc-version", c->version_full ? c->version_full : "");
    for (int i = 0; k_cflags[i]; i++) key_str(&s, "cflag", k_cflags[i]);
    key_str(&s, "target", TARGET_OS "-" TARGET_ARCH);
    uint8_t d[32];
    sha256_final(&s, d);
    sha256_hex(d, out);
    return true;
}

/* ---- the build (worker thread) -------------------------------------------------------------- */

typedef struct {
    char name[64];
    size_t size;
} Unit;

typedef struct {
    /* in */
    char game_dir[4096], build_dir[4096], module_path[4200], key[65];
    Compiler cc;
    int jobs;
    /* progress (written by the worker, read by the UI thread) */
    volatile double frac;
    char step[160];
    volatile int done;        /* 1 ok, -1 failed, -2 cancelled */
    volatile bool cancel;
    char err[1024];
    double t_recomp, t_compile, t_link;
    int units;
} Build;

static void set_step(Build *b, const char *s) { snprintf(b->step, sizeof b->step, "%s", s); }

static bool build_fail(Build *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->err, sizeof b->err, fmt, ap);
    va_end(ap);
    logf_("FAILED: %s", b->err);
    return false;
}

static bool write_sdk(Build *b, const char *sdk_dir)
{
    if (!fresh_dir(sdk_dir)) return build_fail(b, "cannot create %s", sdk_dir);
    for (int i = 0; i < g_embedded_count; i++) {
        const EmbeddedFile *e = &g_embedded_files[i];
        if (e->kind != 0) continue;
        char p[4400];
        snprintf(p, sizeof p, "%s/%s", sdk_dir, e->name + 4);   /* "sdk/" */
        if (!write_file(p, e->data, e->size)) return build_fail(b, "cannot write %s", p);
    }
    char p[4400];
    snprintf(p, sizeof p, "%s/game_sdk.h", sdk_dir);
    const char *h = "/* written by the launcher */\n#pragma once\n#define GAME_ABI_SDK_ID \"" GAME_ABI_SDK_ID "\"\n";
    if (!write_file(p, h, strlen(h))) return build_fail(b, "cannot write %s", p);
    return true;
}

static int cmp_unit(const void *x, const void *y)
{
    const Unit *a = x, *c = y;
    return a->size < c->size ? 1 : a->size > c->size ? -1 : strcmp(a->name, c->name);
}

static bool read_units(Build *b, const char *gen_dir, Unit **out, int *n)
{
    char p[4400];
    snprintf(p, sizeof p, "%s/files.cmake", gen_dir);
    char *txt = slurp(p, NULL);
    if (!txt) return build_fail(b, "the recompiler wrote no file list");
    Unit *u = NULL;
    int k = 0;
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        const char *s = strstr(line, "${GEN_DIR}/");
        if (!s) continue;
        u = realloc(u, (size_t)(k + 2) * sizeof *u);
        snprintf(u[k].name, sizeof u[k].name, "%s", s + 11);
        char f[4500];
        snprintf(f, sizeof f, "%s/%s", gen_dir, u[k].name);
        SDL_PathInfo info;
        u[k].size = SDL_GetPathInfo(f, &info) ? (size_t)info.size : 0;
        k++;
    }
    free(txt);
    if (!k) return build_fail(b, "the recompiler produced no code");
    *out = u;
    *n = k;
    return true;
}

static bool compile_all(Build *b, const char *gen_dir, const char *sdk_dir, const char *obj_dir, Unit *u, int n)
{
    /* the module glue is compiled like a generated file, from the SDK folder */
    snprintf(u[n].name, sizeof u[n].name, "game_module.c");
    u[n].size = 1024;
    n++;
    qsort(u, (size_t)n, sizeof *u, cmp_unit);   /* largest first: the parallel tail is shortest */
    size_t total = 0, finished = 0;
    for (int i = 0; i < n; i++) total += u[i].size;
    int jobs = b->jobs < 1 ? 1 : b->jobs;
    Proc **running = calloc((size_t)jobs, sizeof *running);
    int *which = calloc((size_t)jobs, sizeof *which);
    char side[4400], inc_sdk[4400], inc_gen[4400];
    snprintf(side, sizeof side, "%s/game_module_side.h", sdk_dir);
    snprintf(inc_sdk, sizeof inc_sdk, "-I%s", sdk_dir);
    snprintf(inc_gen, sizeof inc_gen, "-I%s", gen_dir);
    int next = 0, active = 0;
    bool ok = true;
    while ((next < n && ok) || active) {
        for (int j = 0; j < jobs && next < n && ok && !b->cancel; j++) {
            if (running[j]) continue;
            char src[4500], obj[4500];
            if (!strcmp(u[next].name, "game_module.c")) snprintf(src, sizeof src, "%s/game_module.c", sdk_dir);
            else snprintf(src, sizeof src, "%s/%s", gen_dir, u[next].name);
            snprintf(obj, sizeof obj, "%s/%s.o", obj_dir, u[next].name);
            char *argv[40];
            int k = 0;
            for (int i = 0; i < b->cc.argc; i++) argv[k++] = b->cc.argv[i];
            argv[k++] = "-c";
            for (int i = 0; k_cflags[i]; i++) argv[k++] = (char *)k_cflags[i];
            argv[k++] = "-include";
            argv[k++] = side;
            argv[k++] = inc_sdk;
            argv[k++] = inc_gen;
            argv[k++] = "-o";
            argv[k++] = obj;
            argv[k++] = src;
            argv[k] = NULL;
            if (s_log) {
                fprintf(s_log, "$");
                for (int i = 0; argv[i]; i++) fprintf(s_log, " %s", argv[i]);
                fprintf(s_log, "\n");
                fflush(s_log);
            }
            char err[512];
            running[j] = proc_start(argv, s_log_path, err, sizeof err);
            if (!running[j]) { ok = build_fail(b, "%s", err); break; }
            which[j] = next++;
            active++;
        }
        if (b->cancel) {
            for (int j = 0; j < jobs; j++) if (running[j]) { proc_kill(running[j]); running[j] = NULL; }
            free(running); free(which);
            return false;
        }
        bool any = false;
        for (int j = 0; j < jobs; j++) {
            int code;
            if (!running[j] || !proc_poll(running[j], &code)) continue;
            proc_free(running[j]);
            running[j] = NULL;
            active--;
            any = true;
            if (code != 0) {
                if (ok) build_fail(b, "the compiler failed on %s (exit status %d)", u[which[j]].name, code);
                ok = false;
                continue;
            }
            finished += u[which[j]].size;
            b->frac = 0.05 + 0.88 * (double)finished / (double)total;
            char st[160];
            snprintf(st, sizeof st, "Compiling the game code (%d of %d parts)", ++b->units, n);
            set_step(b, st);
        }
        if (!any) plat_sleep_us(20000);
    }
    free(running);
    free(which);
    return ok;
}

static bool link_module(Build *b, const char *obj_dir, Unit *u, int n, const char *tmp_module)
{
    char **argv = calloc((size_t)n + 32, sizeof *argv);
    char (*objs)[4500] = calloc((size_t)n, sizeof *objs);
    int k = 0;
    for (int i = 0; i < b->cc.argc; i++) argv[k++] = b->cc.argv[i];
    argv[k++] = "-shared";
#ifdef __APPLE__
    argv[k++] = "-Wl,-install_name,@rpath/destrega_game.dylib";
#endif
    argv[k++] = "-o";
    argv[k++] = (char *)tmp_module;
    for (int i = 0; i < n; i++) {
        snprintf(objs[i], sizeof objs[i], "%s/%s.o", obj_dir, u[i].name);
        argv[k++] = objs[i];
    }
    argv[k] = NULL;
    if (s_log) {
        fprintf(s_log, "$");
        for (int i = 0; argv[i]; i++) fprintf(s_log, " %s", argv[i]);
        fprintf(s_log, "\n");
        fflush(s_log);
    }
    remove(tmp_module);
    char err[512];
    Proc *p = proc_start(argv, s_log_path, err, sizeof err);
    bool ok = p != NULL;
    if (!p) build_fail(b, "%s", err);
    while (p) {
        int code;
        if (b->cancel) { proc_kill(p); ok = false; break; }
        if (proc_poll(p, &code)) {
            proc_free(p);
            if (code) ok = build_fail(b, "linking the game module failed (exit status %d)", code);
            break;
        }
        plat_sleep_us(20000);
    }
    free(argv);
    free(objs);
    return ok;
}

static bool run_build(Build *b)
{
    char gen[4400], sdk[4400], obj[4400], tmp_module[4400], keyfile[4400];
    snprintf(gen, sizeof gen, "%sgen", b->build_dir);
    snprintf(sdk, sizeof sdk, "%ssdk", b->build_dir);
    snprintf(obj, sizeof obj, "%sobj", b->build_dir);
    snprintf(tmp_module, sizeof tmp_module, "%snew-destrega_game" MOD_EXT, b->game_dir);
    snprintf(keyfile, sizeof keyfile, "%sgame.key", b->game_dir);
    remove(keyfile);   /* an interrupted build never leaves a module that looks current */

    set_step(b, "Reading the game code from your disc");
    double t0 = now_s();
    if (!fresh_dir(gen)) return build_fail(b, "cannot create %s", gen);
    int files = 0;
    if (s_opt_selftest) {   /* copy the self-test's C files instead of recompiling the disc */
        char **list = selftest_files(&files);
        for (int i = 0; i < files; i++) {
            char src[4400], dst[4400];
            size_t len = 0;
            snprintf(src, sizeof src, "%s/%s", s_opt_selftest, list[i]);
            snprintf(dst, sizeof dst, "%s/%s", gen, list[i]);
            char *d = slurp(src, &len);
            bool ok = d && write_file(dst, d, len);
            free(d);
            if (!ok) { free_list(list, files); return build_fail(b, "cannot copy %s", src); }
        }
        free_list(list, files);
    } else {
        RecompInput in = {rc_disc_file, rc_analysis_file, NULL, gen, rc_log};
        const char *err = recomp_run(&in, &files);
        if (err) return build_fail(b, "the recompiler failed: %s", err);
    }
    b->t_recomp = now_s() - t0;
    logf_("recompiled in %.2f s (%d files)", b->t_recomp, files);
    b->frac = 0.05;
    if (b->cancel) return false;

    if (!write_sdk(b, sdk)) return false;
    Unit *u = NULL;
    int n = 0;
    if (!read_units(b, gen, &u, &n)) return false;
    if (!fresh_dir(obj)) { free(u); return build_fail(b, "cannot create %s", obj); }
    set_step(b, "Compiling the game code");
    t0 = now_s();
    bool ok = compile_all(b, gen, sdk, obj, u, n);
    b->t_compile = now_s() - t0;
    if (ok) logf_("compiled %d units in %.2f s with %d parallel jobs", n + 1, b->t_compile, b->jobs);
    if (ok && !b->cancel) {
        set_step(b, "Putting the game together");
        t0 = now_s();
        ok = link_module(b, obj, u, n + 1, tmp_module);
        b->t_link = now_s() - t0;
        if (ok) logf_("linked in %.2f s", b->t_link);
    }
    free(u);
    if (!ok || b->cancel) return false;
    if (!plat_replace_file(tmp_module, b->module_path)) return build_fail(b, "cannot write %s", b->module_path);
    if (!write_file(keyfile, b->key, strlen(b->key))) return build_fail(b, "cannot write %s", keyfile);
    b->frac = 1;
    return true;
}

static void build_thread(void *arg)
{
    Build *b = arg;
    bool ok = run_build(b);
    b->done = ok ? 1 : b->cancel ? -2 : -1;
}

/* ---- error reporting ------------------------------------------------------------------------ */

static char *log_tail(int max_lines)
{
    size_t len = 0;
    char *txt = slurp(s_log_path, &len);
    if (!txt) return NULL;
    size_t i = len;
    int lines = 0;
    while (i > 0 && lines <= max_lines) if (txt[--i] == '\n') lines++;
    char *r = strdup(txt + (i ? i + 1 : 0));
    free(txt);
    return r;
}

static void show_error(void *win, bool headless, const char *msg, const Compiler *cc)
{
    char *tail = log_tail(60);
    size_t n = strlen(msg) + (tail ? strlen(tail) : 0) + 4096;
    char *details = malloc(n);
    snprintf(details, n,
             "Destrega could not prepare the game.\n%s\n\nProgram: %s, %s-%s\nCompiler: %s (%s)\nBuild log: %s\n\n"
             "Last lines of the build log:\n%s",
             msg, GAME_ABI_SDK_ID, TARGET_OS, TARGET_ARCH, cc && cc->argc ? cc->argv[0] : "none",
             cc && cc->version[0] ? cc->version : "-", s_log_path, tail ? tail : "(none)");
    fprintf(stderr, "launcher: %s\n", msg);
    if (!headless) {
        char text[2048];
        snprintf(text, sizeof text,
                 "Destrega could not prepare the game for this computer.\n\n%s\n\n"
                 "\"Copy Details\" puts a full report on the clipboard, for a bug report.",
                 msg);
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Copy Details"}};
        SDL_MessageBoxData d = {SDL_MESSAGEBOX_ERROR, (SDL_Window *)win, "Destrega", text, 2, buttons, NULL};
        int hit = 0;
        if (SDL_ShowMessageBox(&d, &hit) && hit == 1) {
            SDL_SetClipboardText(details);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega",
                                     "The details are on the clipboard. Destrega will now close.", (SDL_Window *)win);
        }
    }
    free(details);
    free(tail);
}

/* ---- entry ---------------------------------------------------------------------------------- */

static bool dev_module(char *out, size_t n)
{
    char dir[4096];
    if (!exe_dir(dir, sizeof dir)) return false;
    snprintf(out, n, "%sdestrega_game" MOD_EXT, dir);
    return file_exists(out);
}

static bool do_recompile(void)
{
    if (!SDL_CreateDirectory(s_opt_recompile)) {
        fprintf(stderr, "launcher: cannot create %s\n", s_opt_recompile);
        return false;
    }
    double t0 = now_s();
    RecompInput in = {rc_disc_file, rc_analysis_file, NULL, s_opt_recompile, NULL};
    int files = 0;
    const char *err = recomp_run(&in, &files);
    if (err) { fprintf(stderr, "launcher: recompile failed: %s\n", err); return false; }
    fprintf(stderr, "launcher: recompiled into %s (%d C files) in %.2f s\n", s_opt_recompile, files, now_s() - t0);
    return true;
}

int launcher_selftest(const char *data_dir, bool data_dir_given)
{
    if (!s_opt_selftest) return -1;
    if (!data_dir_given) {
        fprintf(stderr, "launcher: --game-selftest needs --data-dir (it never touches the player's data folder)\n");
        return 1;
    }
    int code = 1;
    if (!launcher_prepare(data_dir, true, NULL, &code)) {
        fprintf(stderr, "launcher: self-test FAILED\n");
        return code ? code : 1;
    }
    if (s_selftest_cc[0])
        fprintf(stderr, "launcher: self-test passed: game module built with %s and loaded (interface %u)\n",
                s_selftest_cc, GAME_ABI_VERSION);
    else
        fprintf(stderr, "launcher: self-test passed: the game module was current and loaded (interface %u)\n",
                GAME_ABI_VERSION);
    return 0;
}

bool launcher_prepare(const char *data_dir, bool headless, void *win, int *exit_code)
{
    *exit_code = 1;
    if (s_opt_recompile) {
        *exit_code = do_recompile() ? 0 : 1;
        return false;
    }
    const char *err;
    if (s_opt_module && !s_opt_selftest) {
        err = game_module_load(s_opt_module);
        if (!err) { logf_("game module %s", s_opt_module); return true; }
        show_error(win, headless, err, NULL);
        return false;
    }
    char path[4400];
    if (!s_opt_rebuild && !s_opt_analysis && !s_opt_selftest && dev_module(path, sizeof path)) {
        err = game_module_load(path);
        if (!err) { logf_("development game module %s", path); return true; }
        logf_("%s; using the data folder's module", err);
    }

    static Build b;   /* large; the worker thread reads it */
    memset(&b, 0, sizeof b);
    snprintf(b.game_dir, sizeof b.game_dir, "%sgame%c", data_dir,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    snprintf(b.build_dir, sizeof b.build_dir, "%sbuild%c", b.game_dir, b.game_dir[strlen(b.game_dir) - 1]);
    snprintf(b.module_path, sizeof b.module_path, "%sdestrega_game" MOD_EXT, b.game_dir);
    if (!SDL_CreateDirectory(b.build_dir)) {
        char m[4600];
        snprintf(m, sizeof m, "Cannot create the folder %s.", b.build_dir);
        show_error(win, headless, m, NULL);
        return false;
    }
    /* the compiler's caches stay in the data folder (Zig would use the user's cache folder) */
    char cache[4400];
    snprintf(cache, sizeof cache, "%szig-cache", b.build_dir);
    plat_setenv("ZIG_GLOBAL_CACHE_DIR", cache);
    plat_setenv("ZIG_LOCAL_CACHE_DIR", cache);

    double t0 = now_s();
    char tmp[4400], why[1024] = "";
    snprintf(tmp, sizeof tmp, "%scc-version.txt", b.build_dir);
    bool have_cc = find_compiler(&b.cc, tmp, why, sizeof why);
    char key[65] = "";
    if (have_cc && !compute_key(&b.cc, key, why, sizeof why)) have_cc = false;
    double t_key = now_s() - t0;

    /* current module? */
    char keyfile[4400];
    snprintf(keyfile, sizeof keyfile, "%sgame.key", b.game_dir);
    char *old = slurp(keyfile, NULL);
    bool current = have_cc && old && !strcmp(old, key) && file_exists(b.module_path);
    if (!have_cc && old && file_exists(b.module_path) && !s_opt_rebuild && !s_opt_analysis) {
        /* no compiler (any more): a module that loads and passes the ABI checks still runs */
        current = true;
        logf_("no compiler found (%s); trying the existing module", why);
    }
    free(old);
    if (current && !s_opt_rebuild) {
        err = game_module_load(b.module_path);
        if (!err) {
            logf_("game module %s is current (key %.12s, checked in %.2f s)", b.module_path, key, t_key);
            return true;
        }
        logf_("%s; building it again", err);
        if (!have_cc) { show_error(win, headless, err, NULL); return false; }
    }
    if (!have_cc) { show_error(win, headless, why, NULL); return false; }

    snprintf(s_log_path, sizeof s_log_path, "%sbuild.log", b.build_dir);
    remove(s_log_path);
    s_log = fopen(s_log_path, "ab");
    snprintf(b.key, sizeof b.key, "%s", key);
    b.jobs = s_opt_jobs > 0 ? s_opt_jobs : plat_perf_cpus();
    if (b.jobs < 1) b.jobs = 1;
    logf_("building the game module (key %.12s): compiler %s [%s], %s, %d jobs", key, b.cc.argv[0], b.cc.where,
          b.cc.version, b.jobs);
    logf_("compiler check and key in %.2f s", t_key);
    snprintf(s_selftest_cc, sizeof s_selftest_cc, "%s [%s]", b.cc.version, b.cc.where);

    LauncherUI *ui = headless ? NULL : lui_open(win);
    double tb = now_s();
    plat_thread th;
    if (!plat_thread_start(&th, build_thread, &b)) {
        lui_close(ui);
        show_error(win, headless, "Cannot start the build.", &b.cc);
        return false;
    }
    int last_pct = -1;
    while (!b.done) {
        if (ui) {
            lui_update(ui, b.frac, b.step);
            if (lui_cancelled(ui)) b.cancel = true;
        } else {
            int pct = (int)(b.frac * 100);
            if (pct / 10 != last_pct / 10) { fprintf(stderr, "launcher: preparing the game: %d%%\n", pct); last_pct = pct; }
        }
        plat_sleep_us(30000);
    }
    plat_thread_join(th);
    lui_close(ui);
    double total = now_s() - tb;
    if (b.done == -2) {
        logf_("cancelled after %.1f s", total);
        if (s_log) fclose(s_log), s_log = NULL;
        *exit_code = 0;
        return false;
    }
    if (b.done < 0) {
        if (s_log) fclose(s_log), s_log = NULL;
        show_error(win, headless, b.err, &b.cc);
        return false;
    }
    logf_("game module built in %.1f s (recompile %.2f s, compile %.1f s, link %.2f s)", total, b.t_recomp,
          b.t_compile, b.t_link);
    if (s_log) fclose(s_log), s_log = NULL;
    err = game_module_load(b.module_path);
    if (err) { show_error(win, headless, err, &b.cc); return false; }
    logf_("game module %s loaded", b.module_path);
    return true;
}
