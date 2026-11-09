/* Smoothness statistics (see smooth.h). */
#include "smooth.h"
#include "hires.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VBLANK_NS 16666667.0

static uint64_t s_from = 1;
static uint64_t s_frame;          /* last presented frame */

/* presented-frame intervals (us) */
static uint32_t *s_iv;
static size_t s_iv_n, s_iv_cap;
static uint64_t s_last_present;
static uint64_t s_held, s_held_extra, s_held2;
static uint64_t s_held_by[4];     /* rollback, stall, sync, slow host */
static uint64_t s_work_sum, s_work_n, s_over_budget;
static double s_load_recent;      /* EMA of work / budget */

/* events since the last present */
static uint64_t s_ev_stall, s_ev_resim, s_ev_sync;

/* netplay */
static uint64_t s_stalls, s_stall_ns, s_stall_max;
static uint64_t s_syncs, s_sync_ns;
static uint64_t s_rb, s_rb_depth[16];   /* depth histogram: 1..14, 15 = >= 15 */
static uint32_t *s_rb_ns;
static size_t s_rb_n, s_rb_cap;
static uint64_t s_rb_late;              /* re-simulation made the frame miss its vblank */
static uint64_t s_pops, s_pop_frames;

/* audio */
static uint64_t s_aud_frames, s_aud_under, s_aud_low, s_aud_repeat;

/* Scale governor (see smooth.h). Units are governor ticks: presented frames (windowed) or vblanks
 * (headless with --fake-load), i.e. ~1/60 s each. The numbers and why:
 *  - load = this host's own work per frame / 16.7 ms (time spent waiting for the peer or slowing
 *    down for it excluded), averaged with an EMA over 30 ticks (0.5 s), so a single rollback or
 *    renderer switch is only a small bump;
 *  - step down: the average above 90% during a netplay session (a late frame there drags the
 *    opponent too) or 95% offline (only frames above 100% are late; conservative, so normal play
 *    does not flap), without a break for 1 s (netplay) or 2 s (offline);
 *  - step up: the average below 60% for 3 s without a break, and the next step's cost estimated
 *    below 80%. The estimate scales the whole load by the step's pixel ratio (1.33x .. 2x per
 *    step): the hi-res share of the work cannot be measured apart from emulation and present, so
 *    this is an upper bound. 80% keeps a margin to the 90% step-down line;
 *  - warm-up: the first 3 s after the governor starts (program start, Auto chosen, a netplay
 *    session starting or ending) and the first 1 s after any render-scale change are not measured
 *    at all (pipeline creation, the new target's first frames, Wine/D3D12 shader compilation);
 *  - back-off: a scale given up for being too slow is not tried again for 10 s; if it is too slow
 *    again within 30 s of being raised to, that wait doubles each time (20 s, 40 s, .. 5 min at
 *    most), so a light scene (menus) followed by a heavy one (fights) cannot flip the scale every
 *    match. A warm-up stutter can therefore never pin the scale down for long. */
static const int k_ladder[][2] = { {1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 4}, {4, 4}, {4, 6} };
#define NLADDER ((int)(sizeof k_ladder / sizeof k_ladder[0]))
#define GOV_EMA 30
#define GOV_WARM_START 180
#define GOV_WARM_CHANGE 60
#define GOV_DOWN_NET 0.90
#define GOV_DOWN_OFF 0.95
#define GOV_HOLD_NET 60
#define GOV_HOLD_OFF 120
#define GOV_UP_BELOW 0.60
#define GOV_UP_HOLD 180
#define GOV_UP_PREDICT 0.80
#define GOV_BACKOFF 600u
#define GOV_BACKOFF_MAX 18000u
#define GOV_PROBATION 1800
static bool s_gov_off;            /* --no-scale-governor */
static bool s_gov_net;            /* a netplay session is running */
static bool s_gov_allowed;        /* main.c: resolution Auto (smooth_governor_allow) */
static int s_ceil_sx, s_ceil_sy;  /* what Auto picks without the governor */
static bool s_gov_changed;
static int s_gov_sx, s_gov_sy;    /* current caps, 0 = none */
static uint64_t s_tick, s_warm_until = GOV_WARM_START;
static int s_seen_sx = -1, s_seen_sy = -1;
static bool s_gov_seeded;
static double s_gov_load;         /* EMA of the load after warm-up */
static int s_over, s_under;       /* ticks in a row above the step-down / below the step-up line */
static uint64_t s_up_tick;        /* tick of the last step up (0 = none since the last step down) */
static uint64_t s_bo_until[NLADDER + 1];   /* back-off per ladder entry; [NLADDER]: anything above it */
static uint32_t s_bo_wait[NLADDER + 1];
static int s_gov_steps, s_gov_ups;
static uint64_t s_np;              /* presents so far (frame numbers restart with a session) */

/* --fake-load F-G:PCT[@SxxSy],... (test hook: load the governor sees on frames F..G; with @SxxSy
 * only while the render scale is SxxSy, so a test can make one scale too slow and another fine) */
typedef struct { uint64_t a, b; double pct; int sx, sy; } FakeLoad;
static FakeLoad s_fake[32];
static int s_fake_n;

static bool counting(uint64_t f) { return f >= s_from; }

static void gov_restart(uint64_t warm) {
    if (s_warm_until < s_tick + warm) s_warm_until = s_tick + warm;
    s_over = s_under = 0;
    s_gov_seeded = false;
}

static void gov_set_caps(int sx, int sy) {
    if (sx == s_gov_sx && sy == s_gov_sy) return;
    s_gov_sx = sx; s_gov_sy = sy;
    s_gov_changed = true;
}

void smooth_governor(bool on) {
    if (on == s_gov_net) return;
    s_gov_net = on;
    gov_restart(GOV_WARM_START);
}

void smooth_governor_allow(bool allowed, int ceil_sx, int ceil_sy) {
    s_ceil_sx = ceil_sx; s_ceil_sy = ceil_sy;
    if (allowed == s_gov_allowed) return;
    s_gov_allowed = allowed;
    if (!allowed) gov_set_caps(0, 0);   /* a fixed choice is never capped */
    gov_restart(GOV_WARM_START);
}

bool smooth_scale_cap(int *sx, int *sy) {
    if (!s_gov_changed) return false;
    s_gov_changed = false;
    *sx = s_gov_sx; *sy = s_gov_sy;
    return true;
}

static int ladder_index(int sx, int sy) {
    for (int k = 0; k < NLADDER; k++)
        if (k_ladder[k][0] == sx && k_ladder[k][1] == sy) return k;
    return NLADDER;
}

/* one step down: the next smaller ladder entry that fits inside the current scale */
static void governor_down(int cx, int cy, uint64_t frame) {
    for (int k = NLADDER - 1; k >= 0; k--)
        if (k_ladder[k][0] * k_ladder[k][1] < cx * cy && k_ladder[k][0] <= cx && k_ladder[k][1] <= cy) {
            const int i = ladder_index(cx, cy);
            const bool retry = s_up_tick && s_tick - s_up_tick < GOV_PROBATION;
            uint32_t w = s_bo_wait[i];
            w = retry && w ? (w * 2 > GOV_BACKOFF_MAX ? GOV_BACKOFF_MAX : w * 2) : GOV_BACKOFF;
            s_bo_wait[i] = w;
            s_bo_until[i] = s_tick + w;
            s_up_tick = 0;
            gov_set_caps(k_ladder[k][0], k_ladder[k][1]);
            s_gov_steps++;
            fprintf(stderr, "smooth: this computer is too slow for render scale %dx%d (%.0f%% of the frame budget used): "
                    "lowering it to %dx%d%s at frame %llu (%dx%d not tried again for %u s)\n", cx, cy, 100 * s_gov_load,
                    s_gov_sx, s_gov_sy, s_gov_net ? " for the netplay session" : "", (unsigned long long)frame, cx, cy,
                    w / 60);
            return;
        }
}

/* one step up: the next larger ladder entry not above what Auto would pick (or that itself) */
static void governor_up(int cx, int cy, uint64_t frame) {
    int nx = 0, ny = 0;
    for (int k = 0; k < NLADDER; k++)
        if (k_ladder[k][0] * k_ladder[k][1] > cx * cy && k_ladder[k][0] >= cx && k_ladder[k][1] >= cy &&
            k_ladder[k][0] <= s_ceil_sx && k_ladder[k][1] <= s_ceil_sy) { nx = k_ladder[k][0]; ny = k_ladder[k][1]; break; }
    if (!nx && s_ceil_sx * s_ceil_sy > cx * cy && s_ceil_sx >= cx && s_ceil_sy >= cy) { nx = s_ceil_sx; ny = s_ceil_sy; }
    if (!nx) return;                                   /* already at what Auto picks */
    const int i = ladder_index(nx, ny);
    if (s_tick < s_bo_until[i]) return;                /* too slow there recently: wait */
    const double expect = s_gov_load * (double)(nx * ny) / (double)(cx * cy);
    if (expect >= GOV_UP_PREDICT) return;
    if (nx == s_ceil_sx && ny == s_ceil_sy) gov_set_caps(0, 0);
    else gov_set_caps(nx, ny);
    s_up_tick = s_tick;
    s_gov_ups++;
    fprintf(stderr, "smooth: render scale %dx%d uses %.0f%% of the frame budget (about %.0f%% expected at %dx%d): "
            "raising it to %dx%d at frame %llu\n", cx, cy, 100 * s_gov_load, 100 * expect, nx, ny, nx, ny,
            (unsigned long long)frame);
}

static void governor_tick(double load, uint64_t frame) {
    s_tick++;
    int cx, cy;
    if (s_gov_off || !s_gov_allowed || !hires_get_scale(&cx, &cy) || cx < 1 || cy < 1) {
        if (s_gov_sx || s_gov_sy) gov_set_caps(0, 0);
        s_over = s_under = 0;
        s_gov_seeded = false;
        return;
    }
    if (cx != s_seen_sx || cy != s_seen_sy) {          /* any scale change: a new target, warm up again */
        s_seen_sx = cx; s_seen_sy = cy;
        gov_restart(GOV_WARM_CHANGE);
    }
    if (s_tick < s_warm_until) return;
    if (!s_gov_seeded) { s_gov_load = load; s_gov_seeded = true; }
    else s_gov_load += (load - s_gov_load) / GOV_EMA;
    const double down = s_gov_net ? GOV_DOWN_NET : GOV_DOWN_OFF;
    const int hold = s_gov_net ? GOV_HOLD_NET : GOV_HOLD_OFF;
    s_over = s_gov_load > down ? s_over + 1 : 0;
    s_under = s_gov_load < GOV_UP_BELOW ? s_under + 1 : 0;
    if (s_over >= hold) { s_over = 0; governor_down(cx, cy, frame); }
    else if (s_under >= GOV_UP_HOLD) { s_under = 0; governor_up(cx, cy, frame); }
}

static double fake_load(uint64_t frame) {
    double p = 0;
    int cx = 0, cy = 0;
    if (s_fake_n && !hires_get_scale(&cx, &cy)) cx = cy = 0;
    for (int i = 0; i < s_fake_n; i++)
        if (frame >= s_fake[i].a && frame <= s_fake[i].b && (!s_fake[i].sx || (s_fake[i].sx == cx && s_fake[i].sy == cy)))
            p += s_fake[i].pct;
    return p / 100.0;
}
bool smooth_fake_load_given(void) { return s_fake_n > 0; }
void smooth_headless_frame(uint64_t frame) {
    if (s_fake_n) governor_tick(fake_load(frame), frame);
}

uint64_t smooth_from(void) { return s_from; }
double smooth_load(void) { return s_load_recent; }

static void push(uint32_t **a, size_t *n, size_t *cap, uint32_t v) {
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 4096;
        uint32_t *p = realloc(*a, c * sizeof *p);
        if (!p) return;
        *a = p; *cap = c;
    }
    (*a)[(*n)++] = v;
}

void smooth_present(uint64_t now, uint64_t work_ns, uint64_t frame) {
    s_frame = frame;
    s_np++;
    /* load: our own work, without the time spent waiting for the peer or slowing down for it */
    uint64_t wait = s_ev_stall + s_ev_sync;
    work_ns = work_ns > wait ? work_ns - wait : 0;
    double load = work_ns / VBLANK_NS;
    s_load_recent += (load - s_load_recent) / 30;
    governor_tick(load + fake_load(frame), frame);
    if (!counting(frame)) { s_last_present = now; s_ev_stall = s_ev_resim = s_ev_sync = 0; return; }
    s_work_sum += work_ns; s_work_n++;
    if (work_ns > VBLANK_NS) s_over_budget++;
    if (s_last_present) {
        uint64_t iv = now - s_last_present;
        push(&s_iv, &s_iv_n, &s_iv_cap, (uint32_t)(iv / 1000 > 0xffffffffu ? 0xffffffffu : iv / 1000));
        if (iv > 1.5 * VBLANK_NS) {
            s_held++;
            uint64_t extra = (uint64_t)llround(iv / VBLANK_NS) - 1;
            if (extra < 1) extra = 1;
            s_held_extra += extra;
            if (extra >= 2) s_held2++;
            /* what held it: the largest contributor if it explains a good part of the overrun */
            uint64_t over = iv - (uint64_t)VBLANK_NS;
            uint64_t m = s_ev_resim; int k = 0;
            if (s_ev_stall > m) { m = s_ev_stall; k = 1; }
            if (s_ev_sync > m) { m = s_ev_sync; k = 2; }
            if (m * 2 < over) k = 3;
            s_held_by[k]++;
            if (k == 0) s_rb_late++;
        }
    }
    s_last_present = now;
    s_ev_stall = s_ev_resim = s_ev_sync = 0;
}

void smooth_audio(uint32_t queued, uint32_t frame_bytes, bool repeat) {
    if (!counting(s_frame)) return;
    s_aud_frames++;
    if (queued == 0) s_aud_under++;
    else if (queued < frame_bytes) s_aud_low++;
    if (repeat) s_aud_repeat++;
}

void smooth_stall(uint64_t ns) {
    s_ev_stall += ns;
    if (!counting(s_frame + 1)) return;
    s_stalls++; s_stall_ns += ns;
    if (ns > s_stall_max) s_stall_max = ns;
}

void smooth_sync(uint64_t ns) {
    s_ev_sync += ns;
    if (!counting(s_frame + 1)) return;
    s_syncs++; s_sync_ns += ns;
}

void smooth_rollback(uint64_t depth, uint64_t resim_ns, uint64_t shown_wrong) {
    s_ev_resim += resim_ns;
    if (!counting(s_frame + 1)) return;
    s_rb++;
    s_rb_depth[depth >= 15 ? 15 : depth]++;
    push(&s_rb_ns, &s_rb_n, &s_rb_cap, (uint32_t)(resim_ns / 1000));
    if (shown_wrong) { s_pops++; s_pop_frames += shown_wrong; }
}

int smooth_parse_arg(int argc, char **argv, int i) {
    if (!strcmp(argv[i], "--smooth-from") && i + 1 < argc) { s_from = strtoull(argv[i + 1], NULL, 10); return 2; }
    if (!strcmp(argv[i], "--no-scale-governor")) { s_gov_off = true; return 1; }
    if (!strcmp(argv[i], "--fake-load") && i + 1 < argc) {   /* F-G:PCT,... (governor test hook) */
        const char *p = argv[i + 1];
        while (*p && s_fake_n < 32) {
            char *e;
            FakeLoad f = { strtoull(p, &e, 10), 0, 0, 0, 0 };
            f.b = *e == '-' ? strtoull(e + 1, &e, 10) : f.a;
            if (*e != ':') break;
            f.pct = strtod(e + 1, &e);
            if (*e == '@') {
                f.sx = (int)strtol(e + 1, &e, 10);
                f.sy = *e == 'x' ? (int)strtol(e + 1, &e, 10) : f.sx;
            }
            s_fake[s_fake_n++] = f;
            if (*e != ',') break;
            p = e + 1;
        }
        return 2;
    }
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static double pct(uint32_t *v, size_t n, double p) {   /* v sorted; ms */
    if (!n) return 0;
    size_t k = (size_t)(p * (double)(n - 1) + 0.5);
    return v[k] / 1000.0;
}

void smooth_report(void) {
    if (!s_iv_n && !s_stalls && !s_rb) return;
    qsort(s_iv, s_iv_n, sizeof *s_iv, cmp_u32);
    qsort(s_rb_ns, s_rb_n, sizeof *s_rb_ns, cmp_u32);
    double mean = 0, sd = 0;
    for (size_t k = 0; k < s_iv_n; k++) mean += s_iv[k] / 1000.0;
    if (s_iv_n) mean /= (double)s_iv_n;
    for (size_t k = 0; k < s_iv_n; k++) { double d = s_iv[k] / 1000.0 - mean; sd += d * d; }
    if (s_iv_n > 1) sd = sqrt(sd / (double)(s_iv_n - 1));
    double rb_avg = 0;
    for (size_t k = 0; k < s_rb_n; k++) rb_avg += s_rb_ns[k] / 1000.0;
    if (s_rb_n) rb_avg /= (double)s_rb_n;
    double held_pct = s_iv_n ? 100.0 * (double)s_held / (double)s_iv_n : 0;
    double busy = s_work_n ? 100.0 * (double)s_work_sum / (double)s_work_n / VBLANK_NS : 0;
    double over = s_work_n ? 100.0 * (double)s_over_budget / (double)s_work_n : 0;
    char hist[256] = "";
    size_t hl = 0;
    for (int d = 1; d < 16; d++)
        if (s_rb_depth[d] && hl < sizeof hist - 24)
            hl += (size_t)snprintf(hist + hl, sizeof hist - hl, "%s%d%s:%llu", hl ? " " : "", d, d == 15 ? "+" : "",
                                   (unsigned long long)s_rb_depth[d]);
    fprintf(stderr,
            "smooth: from frame %llu: %zu presents, interval p50 %.1f p95 %.1f p99 %.1f max %.1f ms (sd %.2f ms); "
            "held >1 vblank %llu (%.2f%%, %llu extra vblanks, %llu held >= 2 extra) by rollback %llu / stall %llu / "
            "sync %llu / slow host %llu; host busy %.0f%% of the budget (over budget %.1f%% of frames); "
            "stalls %llu (%.0f ms, max %.1f ms); sync slowdown %llu frames (%.0f ms); rollbacks %llu depth [%s] "
            "re-sim avg %.2f p95 %.2f max %.2f ms, %llu made a frame late; corrections %llu (%.1f frames shown "
            "before correction on average); audio underruns %llu, low %llu, repeated %llu of %llu frames; "
            "render scale lowered %d times, raised %d times\n",
            (unsigned long long)s_from, s_iv_n + (s_iv_n ? 1 : 0), pct(s_iv, s_iv_n, 0.5), pct(s_iv, s_iv_n, 0.95),
            pct(s_iv, s_iv_n, 0.99), s_iv_n ? s_iv[s_iv_n - 1] / 1000.0 : 0, sd,
            (unsigned long long)s_held, held_pct, (unsigned long long)s_held_extra, (unsigned long long)s_held2,
            (unsigned long long)s_held_by[0], (unsigned long long)s_held_by[1], (unsigned long long)s_held_by[2],
            (unsigned long long)s_held_by[3], busy, over,
            (unsigned long long)s_stalls, s_stall_ns / 1e6, s_stall_max / 1e6, (unsigned long long)s_syncs,
            s_sync_ns / 1e6, (unsigned long long)s_rb, hist, rb_avg, pct(s_rb_ns, s_rb_n, 0.95),
            s_rb_n ? s_rb_ns[s_rb_n - 1] / 1000.0 : 0, (unsigned long long)s_rb_late, (unsigned long long)s_pops,
            s_pops ? (double)s_pop_frames / (double)s_pops : 0, (unsigned long long)s_aud_under,
            (unsigned long long)s_aud_low, (unsigned long long)s_aud_repeat, (unsigned long long)s_aud_frames,
            s_gov_steps, s_gov_ups);
    fprintf(stderr,
            "smooth-json: {\"from\": %llu, \"presents\": %zu, \"iv_p50\": %.2f, \"iv_p95\": %.2f, \"iv_p99\": %.2f, "
            "\"iv_max\": %.2f, \"iv_sd\": %.3f, \"held\": %llu, \"held_pct\": %.3f, \"held_extra\": %llu, \"held2\": %llu, "
            "\"held_rollback\": %llu, \"held_stall\": %llu, \"held_sync\": %llu, \"held_slow\": %llu, \"busy_pct\": %.1f, "
            "\"over_budget_pct\": %.2f, \"stalls\": %llu, \"stall_ms\": %.1f, \"stall_max_ms\": %.2f, \"sync_frames\": %llu, "
            "\"sync_ms\": %.1f, \"rollbacks\": %llu, \"resim_avg\": %.3f, \"resim_p95\": %.3f, \"resim_max\": %.3f, "
            "\"rollback_late\": %llu, \"corrections\": %llu, \"correction_frames\": %llu, \"audio_under\": %llu, "
            "\"audio_low\": %llu, \"audio_repeat\": %llu, \"audio_frames\": %llu, \"scale_steps\": %d, \"scale_ups\": %d, \"depth\": [",
            (unsigned long long)s_from, s_iv_n + (s_iv_n ? 1 : 0), pct(s_iv, s_iv_n, 0.5), pct(s_iv, s_iv_n, 0.95),
            pct(s_iv, s_iv_n, 0.99), s_iv_n ? s_iv[s_iv_n - 1] / 1000.0 : 0, sd, (unsigned long long)s_held, held_pct,
            (unsigned long long)s_held_extra, (unsigned long long)s_held2, (unsigned long long)s_held_by[0],
            (unsigned long long)s_held_by[1], (unsigned long long)s_held_by[2], (unsigned long long)s_held_by[3], busy,
            over, (unsigned long long)s_stalls, s_stall_ns / 1e6, s_stall_max / 1e6, (unsigned long long)s_syncs,
            s_sync_ns / 1e6, (unsigned long long)s_rb, rb_avg, pct(s_rb_ns, s_rb_n, 0.95),
            s_rb_n ? s_rb_ns[s_rb_n - 1] / 1000.0 : 0, (unsigned long long)s_rb_late, (unsigned long long)s_pops,
            (unsigned long long)s_pop_frames, (unsigned long long)s_aud_under, (unsigned long long)s_aud_low,
            (unsigned long long)s_aud_repeat, (unsigned long long)s_aud_frames, s_gov_steps, s_gov_ups);
    for (int d = 0; d < 16; d++) fprintf(stderr, "%s%llu", d ? ", " : "", (unsigned long long)s_rb_depth[d]);
    fprintf(stderr, "]}\n");
}
