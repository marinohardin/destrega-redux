/* Save states: complete emulated-state snapshots at the frame boundary (see savestate.h).
 *
 * Where a snapshot is valid
 * -------------------------
 * Guest code is recompiled C: a guest `jal` is a host call and a guest `jr ra` a host
 * return, so the host C stack holds the guest's control flow. Everything else (registers,
 * memory, devices) lives in globals. A snapshot taken at host point P can therefore only be
 * restored while the host is at the *same* point P: same chain of host return addresses,
 * same host stack depth, same shadow call stack. Generated code keeps no guest value in a
 * C local across a call or an EVT_CHECK (branch conditions and jump targets are consumed
 * before the call / the check), so at such a point the globals are the whole machine.
 *
 * The frame boundary is the VBlank scheduler event (bus.c vblank_cb), which runs inside
 * rt_event() from an EVT_CHECK of whatever guest function is executing. ss_vblank_begin()
 * captures the host return-address chain there; that chain identifies the guest call chain
 * *and* the EVT_CHECK site. ss_safe_point() additionally refuses points where a C helper
 * keeps guest state in its own locals while guest code runs:
 *   - an interrupt handler is running (rt.c take_interrupt keeps the interrupted Cpu),
 *   - g_ss_unsafe > 0: BIOS event delivery / SysEnqIntRP calls (bios.c), override
 *     verification (override.c), the interpreter (interp.c).
 * In practice the game is waiting in VSync() at almost every vblank, so consecutive frames
 * share one context and any frame can be restored at any later frame of the same loop.
 */
#include "savestate.h"
#include "hires.h"
#include "pgxp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

int g_ss_unsafe;
bool rt_in_exception(void);
extern u32 g_callstack[1024];
extern u32 g_depth;

static u64 s_frame_dummy;
static u64 *s_frame = &s_frame_dummy;
void ss_set_frame_counter(u64 *frame) { s_frame = frame ? frame : &s_frame_dummy; }

/* ---- visiting ---------------------------------------------------------------------- */

static void frontend_state_io(StateIO *io) { SS_VAR(io, *s_frame); }

typedef struct { const char *name; void (*fn)(StateIO *); } Module;
static const Module s_modules[] = {
    {"cpu", cpu_state_io}, {"bus", bus_state_io}, {"bios", bios_state_io}, {"gpu", gpu_state_io},
    {"gte", gte_state_io}, {"spu", spu_state_io}, {"cdrom", cdrom_state_io}, {"mdec", mdec_state_io},
    {"sio", sio_state_io}, {"memcard", memcard_state_io}, {"frontend", frontend_state_io},
};
#define NMOD (int)(sizeof s_modules / sizeof s_modules[0])

void ss_visit_all(StateIO *io) {
    for (int i = 0; i < NMOD; i++) s_modules[i].fn(io);
}

u64 ss_hash_all(void) {
    StateIO io = { .mode = SS_HASH, .hash = 0xcbf29ce484222325ull };
    ss_visit_all(&io);
    return io.hash;
}

static void hash_modules(u64 out[NMOD]) {
    for (int i = 0; i < NMOD; i++) {
        StateIO io = { .mode = SS_HASH, .hash = 0xcbf29ce484222325ull };
        s_modules[i].fn(&io);
        out[i] = io.hash;
    }
}

/* RAM in 64 KiB chunks, for mismatch reports */
#define RAM_CHUNKS 32
static void hash_ram_chunks(u64 out[RAM_CHUNKS]) {
    for (int i = 0; i < RAM_CHUNKS; i++) out[i] = ss_hash_bytes(0, g_ram + (size_t)i * 65536, 65536);
}

static u64 now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

/* ---- host context ------------------------------------------------------------------- */

typedef struct { u32 n; void *pc[64]; void *sp; u32 depth, cs_hash; bool valid; } HostCtx;
static HostCtx s_ctx;   /* captured at the top of ss_vblank_begin (valid only inside it) */

static u32 callstack_hash(void) {
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < g_depth && i < 1024; i++) h = (h ^ g_callstack[i]) * 0x01000193u;
    return h;
}

__attribute__((noinline)) static void capture_ctx(HostCtx *c) {
#if defined(_WIN32)
    c->n = CaptureStackBackTrace(1, 64, c->pc, NULL);
#elif defined(HAVE_BACKTRACE)
    c->n = (u32)backtrace(c->pc, 64);
#else
    c->n = 0;
#endif
    c->sp = __builtin_frame_address(0);
    c->depth = g_depth;
    c->cs_hash = callstack_hash();
    c->valid = true;
}

static u32 ctx_hash(const HostCtx *c) {
    u32 h = 0x811c9dc5u ^ c->depth ^ c->cs_hash;
    for (u32 i = 0; i < c->n; i++) h = (h ^ (u32)(uintptr_t)c->pc[i]) * 0x01000193u;
    return h ^ (u32)(uintptr_t)c->sp;
}

bool ss_safe_point(void) {
    return s_ctx.valid && !rt_in_exception() && g_ss_unsafe == 0;
}

bool ss_context_matches(const Snapshot *s) {
    if (!s->valid || !s_ctx.valid) return false;
    if (s->depth != s_ctx.depth || s->callstack_hash != s_ctx.cs_hash || s->ctx_n != s_ctx.n + 1) return false;
    if (s->ctx[0] != s_ctx.sp) return false;
    for (u32 i = 0; i < s_ctx.n; i++) if (s->ctx[i + 1] != s_ctx.pc[i]) return false;
    return true;
}

/* ---- snapshots ---------------------------------------------------------------------- */

static struct { u64 n, ns, max_ns, bytes; } s_st_save, s_st_load;

bool ss_save(Snapshot *s) {
    if (!ss_safe_point()) return false;
    u64 t0 = now_ns();
    StateIO io = { .mode = SS_SAVE, .buf = s->buf, .cap = s->cap };
    ss_visit_all(&io);
    s->buf = io.buf; s->cap = io.cap; s->len = io.pos;
    s->frame = *s_frame + 1;   /* the frame this vblank starts (see ss_vblank_begin) */
    s->depth = s_ctx.depth; s->callstack_hash = s_ctx.cs_hash;
    s->ctx[0] = s_ctx.sp;
    for (u32 i = 0; i < s_ctx.n && i < 63; i++) s->ctx[i + 1] = s_ctx.pc[i];
    s->ctx_n = s_ctx.n + 1;
    s->valid = true;
    u64 dt = now_ns() - t0;
    s_st_save.n++; s_st_save.ns += dt; s_st_save.bytes += io.pos;
    if (dt > s_st_save.max_ns) s_st_save.max_ns = dt;
    return true;
}

static int s_reseed_hires = 1;
bool ss_restore(const Snapshot *s) {
    if (!ss_context_matches(s) || !ss_safe_point()) return false;
    u64 t0 = now_ns();
    StateIO io = { .mode = SS_LOAD, .buf = s->buf, .cap = s->len };
    ss_visit_all(&io);
    pgxp_reset();                        /* presentation-only associations are stale now */
    if (s_reseed_hires) hires_reseed();
    u64 dt = now_ns() - t0;
    s_st_load.n++; s_st_load.ns += dt; s_st_load.bytes += io.pos;
    if (dt > s_st_load.max_ns) s_st_load.max_ns = dt;
    return true;
}

void ss_free(Snapshot *s) { free(s->buf); memset(s, 0, sizeof *s); }

/* ---- frame-boundary statistics (where is it safe?) -------------------------------- */

static struct { u64 vblanks, safe, in_exc, unsafe_helper; } s_pt;
typedef struct { u32 key, guest_top, depth; u64 n; } CtxStat;
static CtxStat s_ctxs[64];
static int s_ctx_n;
static bool s_point_stats;
static u64 s_point_from;   /* --ss-stats F: contexts from frame F on */
static u64 s_ctx_other;

static void note_point(void) {
    s_pt.vblanks++;
    if (rt_in_exception()) { s_pt.in_exc++; return; }
    if (g_ss_unsafe) { s_pt.unsafe_helper++; return; }
    s_pt.safe++;
    if (!s_point_stats || *s_frame + 1 < s_point_from) return;
    u32 k = ctx_hash(&s_ctx);
    for (int i = 0; i < s_ctx_n; i++) if (s_ctxs[i].key == k) { s_ctxs[i].n++; return; }
    if (s_ctx_n < 64)
        s_ctxs[s_ctx_n++] = (CtxStat){k, g_depth ? g_callstack[(g_depth - 1) & 1023] : 0, g_depth, 1};
    else s_ctx_other++;
}

/* ---- options / test modes --------------------------------------------------------- */

/* --savestate-test N:M,...  save at frame N, run to the first frame >= M whose host context
 *                           matches, restore N, re-run with the same inputs; every frame's
 *                           complete state hash must equal the first pass.
 * --savestate-rewind E:D    the same every E frames with distance D for the whole run.
 * --savestate-inplace E     every E frames: save, load a corrupted copy, restore the save
 *                           (round-trip check: the state hash afterwards must be unchanged).
 * --ss-hash FILE            per-frame complete state hash (final timeline) at exit.
 * --ss-stats                where vblanks happen (safe points, host contexts).        */
typedef struct { u64 n, m; } TestPair;
static TestPair s_tests[64];
static int s_test_n, s_test_i, s_test_retry;
static u64 s_rw_every, s_rw_dist, s_rw_next;
static u64 s_inplace_every;
static const char *s_hash_path;
static u64 *s_fhash; static u64 s_fhash_cap, s_fhash_max;

/* active rewind (one at a time) */
enum { RW_IDLE, RW_FIRST, RW_SECOND };
static int s_rw = RW_IDLE;
static Snapshot s_snap;
static u64 s_rw_from, s_rw_target, s_rw_at;     /* saved frame, earliest restore, restored at */
#define RW_MAX 1024                              /* frames of history per rewind */
static u64 s_h1[RW_MAX];
static u64 s_m1[RW_MAX][NMOD];
static u64 s_c1[RW_MAX][RAM_CHUNKS];
static struct { u64 rewinds, frames_cmp, mismatches, abandoned, inplace, inplace_bad; } s_tr;
static bool s_resim;
static int s_fail;

bool ss_resimulating(void) { return s_resim; }
static bool s_det_audio;
bool ss_deterministic_audio(void) { return s_det_audio || s_test_n || s_rw_every || s_inplace_every; }
void ss_set_deterministic_audio(bool on) { s_det_audio = on; }

static void record_final_hash(u64 f, u64 h) {
    if (!s_hash_path) return;
    if (f >= s_fhash_cap) {
        u64 c = s_fhash_cap ? s_fhash_cap : 65536;
        while (c <= f) c *= 2;
        s_fhash = realloc(s_fhash, c * sizeof *s_fhash);
        memset(s_fhash + s_fhash_cap, 0, (c - s_fhash_cap) * sizeof *s_fhash);
        s_fhash_cap = c;
    }
    s_fhash[f] = h;
    if (f > s_fhash_max) s_fhash_max = f;
}

static void report_mismatch(u64 f, u64 idx) {
    u64 m[NMOD], c[RAM_CHUNKS];
    hash_modules(m);
    char what[512] = "";
    for (int i = 0; i < NMOD; i++)
        if (m[i] != s_m1[idx][i]) { strncat(what, " ", sizeof what - strlen(what) - 1); strncat(what, s_modules[i].name, sizeof what - strlen(what) - 1); }
    hash_ram_chunks(c);
    char ram[256] = "";
    for (int i = 0; i < RAM_CHUNKS; i++)
        if (c[i] != s_c1[idx][i]) {
            char b[24]; snprintf(b, sizeof b, " %08x", 0x80000000u + i * 65536);
            strncat(ram, b, sizeof ram - strlen(ram) - 1);
        }
    fprintf(stderr, "savestate: MISMATCH at frame %llu (saved at %llu): modules:%s; RAM chunks:%s\n",
            (unsigned long long)f, (unsigned long long)s_rw_from, what, ram[0] ? ram : " none");
}

/* State at the start of frame f (before the frontend's per-frame work and the inputs). */
static void rewind_step(u64 f) {
    if (s_rw == RW_IDLE) {
        bool want = false;
        u64 dist = 0;
        if (s_test_i < s_test_n && f >= s_tests[s_test_i].n) { want = true; dist = s_tests[s_test_i].m - s_tests[s_test_i].n; }
        else if (s_rw_every && f >= s_rw_next) { want = true; dist = s_rw_dist; }
        if (!want || !ss_safe_point()) return;
        if (dist >= RW_MAX / 2) dist = RW_MAX / 2;
        if (!ss_save(&s_snap)) return;
        s_rw = RW_FIRST; s_rw_from = f; s_rw_target = f + (dist ? dist : 1);
        if (s_rw_every) s_rw_next = f + s_rw_every;
    }
    u64 idx = f - s_rw_from;
    if (s_rw == RW_FIRST) {
        if (idx < RW_MAX) {
            s_h1[idx] = ss_hash_all();
            hash_modules(s_m1[idx]);
            hash_ram_chunks(s_c1[idx]);
        }
        if (f >= s_rw_target && idx < RW_MAX && ss_context_matches(&s_snap) && ss_safe_point()) {
            s_rw_at = f;
            if (!ss_restore(&s_snap)) { s_rw = RW_IDLE; return; }
            s_rw = RW_SECOND;
            s_tr.rewinds++;
            s_resim = true;
            idx = 0; f = s_rw_from;
        } else {
            /* the saved host context did not recur (a one-off point, e.g. inside a load):
             * give up on this one; a --savestate-test point is retried from a later frame */
            u64 dist = s_rw_target - s_rw_from, slack = dist > 60 ? dist : 60;
            if (f >= s_rw_target + slack || idx + 1 >= RW_MAX) {
                s_tr.abandoned++;
                s_rw = RW_IDLE;
                if (s_test_i < s_test_n) {
                    if (++s_test_retry > 8) { fprintf(stderr, "savestate-test: no recurring context near %llu\n", (unsigned long long)s_tests[s_test_i].n); s_test_i++; s_test_retry = 0; }
                    else {
                        fprintf(stderr, "savestate-test: context of frame %llu did not recur by %llu, retrying from %llu\n",
                                (unsigned long long)s_rw_from, (unsigned long long)f, (unsigned long long)(f + 1));
                        s_tests[s_test_i].n = f + 1; s_tests[s_test_i].m = f + 1 + dist;
                    }
                }
            }
            return;
        }
    }
    if (s_rw == RW_SECOND) {
        u64 h = ss_hash_all();
        s_tr.frames_cmp++;
        if (h != s_h1[idx]) {
            if (s_tr.mismatches++ < 5) report_mismatch(f, idx);
            s_fail = 1;
        }
        if (f == s_rw_at) {
            if (s_test_i < s_test_n) {
                fprintf(stderr, "savestate-test: saved %llu, restored at %llu, %llu frames re-run: %s\n",
                        (unsigned long long)s_rw_from, (unsigned long long)s_rw_at,
                        (unsigned long long)(s_rw_at - s_rw_from), s_fail ? "MISMATCH" : "identical");
                s_test_i++; s_test_retry = 0;
            }
            s_rw = RW_IDLE;
            s_resim = false;
        }
    }
}

/* Round-trip check: save, scribble over every visited byte, load the save back. */
static void inplace_step(u64 f) {
    if (!s_inplace_every || f % s_inplace_every || !ss_safe_point()) return;
    static Snapshot a;
    static u8 *junk; static size_t junk_cap;
    u64 h0 = ss_hash_all();
    if (!ss_save(&a)) return;
    if (junk_cap < a.len) { junk = realloc(junk, a.len); junk_cap = a.len; }
    for (size_t i = 0; i < a.len; i++) junk[i] = a.buf[i] ^ 0xa5;
    /* nothing runs between the two loads, so corrupted host pointers are never used */
    StateIO lj = { .mode = SS_LOAD, .buf = junk, .cap = a.len };
    ss_visit_all(&lj);
    bool ok = ss_restore(&a) && ss_hash_all() == h0;
    s_tr.inplace++;
    if (!ok) { s_tr.inplace_bad++; s_fail = 1; if (s_tr.inplace_bad < 5) fprintf(stderr, "savestate: in-place round trip MISMATCH at frame %llu\n", (unsigned long long)f); }
}

/* Hooks owned by the replay / netplay module (replay.c). */
void replay_vblank_begin(u64 frame);
void replay_vblank_end(u64 frame);
int replay_parse_arg(int argc, char **argv, int i);
void replay_usage(void);
void replay_finish(void);

void ss_vblank_begin(void) {
    capture_ctx(&s_ctx);
    note_point();
    u64 f = *s_frame + 1;
    if (s_test_n || s_rw_every) rewind_step(f);
    inplace_step(*s_frame + 1);
    replay_vblank_begin(*s_frame + 1);
    if (s_hash_path) record_final_hash(*s_frame + 1, ss_hash_all());
    if (s_hash_path && getenv("SS_DEBUG_MODULES") && (u64)atoll(getenv("SS_DEBUG_MODULES")) == *s_frame + 1) {
        u64 m[NMOD];
        hash_modules(m);
        for (int i = 0; i < NMOD; i++) fprintf(stderr, "ss-module %s %016llx\n", s_modules[i].name, (unsigned long long)m[i]);
    }
    s_ctx.valid = false;
}

void ss_vblank_end(void) {
    replay_vblank_end(*s_frame);
}

static bool parse_pairs(const char *s) {
    while (*s && s_test_n < 64) {
        unsigned long long n, m;
        int k = 0;
        if (sscanf(s, "%llu:%llu%n", &n, &m, &k) != 2 || m <= n) return false;
        s_tests[s_test_n++] = (TestPair){n, m};
        s += k;
        if (*s == ',') s++;
    }
    return true;
}

int ss_parse_arg(int argc, char **argv, int i) {
    const char *a = argv[i];
    bool has = i + 1 < argc;
    if (!strcmp(a, "--savestate-test") && has) {
        if (!parse_pairs(argv[i + 1])) { fprintf(stderr, "--savestate-test N:M[,N:M...] (M > N)\n"); exit(2); }
        return 2;
    }
    if (!strcmp(a, "--savestate-rewind") && has) {
        unsigned long long e, d;
        if (sscanf(argv[i + 1], "%llu:%llu", &e, &d) != 2 || !e || !d) { fprintf(stderr, "--savestate-rewind EVERY:DIST\n"); exit(2); }
        s_rw_every = e; s_rw_dist = d; s_rw_next = e;
        return 2;
    }
    if (!strcmp(a, "--savestate-inplace") && has) { s_inplace_every = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--ss-hash") && has) { s_hash_path = argv[i + 1]; return 2; }
    if (!strcmp(a, "--ss-stats") && has) { s_point_stats = true; s_point_from = strtoull(argv[i + 1], NULL, 0); return 2; }
    return replay_parse_arg(argc, argv, i);
}

void ss_usage(void) {
    fprintf(stderr,
        "  --savestate-test N:M,...  save at frame N, restore at frame >= M, re-run: hashes must match\n"
        "  --savestate-rewind E:D    the same every E frames, D frames back, for the whole run\n"
        "  --savestate-inplace E     every E frames: save, corrupt, restore (round trip)\n"
        "  --ss-hash FILE            per-frame complete emulated-state hash at exit\n"
        "  --ss-stats F              report vblank safe points / host contexts (from frame F) at exit\n");
    replay_usage();
}

void ss_finish(void) {
    replay_finish();
    if (s_st_save.n)
        fprintf(stderr, "savestate: %llu saves, avg %.3f ms, max %.3f ms, %.2f MB; %llu loads, avg %.3f ms, max %.3f ms\n",
                (unsigned long long)s_st_save.n, s_st_save.ns / (double)s_st_save.n / 1e6, s_st_save.max_ns / 1e6,
                s_st_save.bytes / (double)s_st_save.n / 1048576.0, (unsigned long long)s_st_load.n,
                s_st_load.n ? s_st_load.ns / (double)s_st_load.n / 1e6 : 0.0, s_st_load.max_ns / 1e6);
    if (s_test_n || s_rw_every)
        fprintf(stderr, "savestate: %llu rewinds, %llu frames compared, %llu mismatches, %llu abandoned (no matching context)%s\n",
                (unsigned long long)s_tr.rewinds, (unsigned long long)s_tr.frames_cmp,
                (unsigned long long)s_tr.mismatches, (unsigned long long)s_tr.abandoned,
                s_test_i < s_test_n ? " INCOMPLETE" : "");
    if (s_test_i < s_test_n) s_fail = 1;
    if (s_inplace_every)
        fprintf(stderr, "savestate: %llu in-place round trips, %llu bad\n",
                (unsigned long long)s_tr.inplace, (unsigned long long)s_tr.inplace_bad);
    if (s_point_stats || s_test_n || s_rw_every) {
        fprintf(stderr, "savestate: %llu vblanks: %llu safe, %llu in an interrupt handler, %llu in a C helper\n",
                (unsigned long long)s_pt.vblanks, (unsigned long long)s_pt.safe,
                (unsigned long long)s_pt.in_exc, (unsigned long long)s_pt.unsafe_helper);
        if (s_point_stats)
            fprintf(stderr, "savestate: %d distinct host contexts from frame %llu (%llu vblanks beyond the first 64)\n",
                    s_ctx_n, (unsigned long long)s_point_from, (unsigned long long)s_ctx_other);
        for (int k = 0; k < s_ctx_n && k < 12; k++) {
            int best = k;
            for (int i = k + 1; i < s_ctx_n; i++) if (s_ctxs[i].n > s_ctxs[best].n) best = i;
            CtxStat t = s_ctxs[k]; s_ctxs[k] = s_ctxs[best]; s_ctxs[best] = t;
            fprintf(stderr, "savestate:   context %08x: %llu vblanks, guest depth %u, innermost guest function %08x\n",
                    s_ctxs[k].key, (unsigned long long)s_ctxs[k].n, s_ctxs[k].depth, s_ctxs[k].guest_top);
        }
    }
    if (s_hash_path) {
        FILE *f = fopen(s_hash_path, "w");
        if (f) {
            for (u64 i = 1; i <= s_fhash_max; i++) fprintf(f, "%llu %016llx\n", (unsigned long long)i, (unsigned long long)s_fhash[i]);
            fclose(f);
        }
    }
}

int replay_failed(void);
int ss_failed(void) { return s_fail || replay_failed(); }
