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

/* scale governor (netplay): see smooth.h */
static bool s_gov_off;            /* --no-scale-governor */
static bool s_gov_on;             /* a netplay session is running */
static bool s_gov_changed;
static int s_gov_sx, s_gov_sy;    /* current caps, 0 = none */
static uint64_t s_gov_next;       /* frame from which the next step may be taken */
static double s_gov_load;         /* EMA of load since the governor (re)started */
static int s_gov_steps;
static uint64_t s_np;              /* presents so far (frame numbers restart with a session) */

static bool counting(uint64_t f) { return f >= s_from; }

void smooth_governor(bool on) {
    if (s_gov_off || on == s_gov_on) return;
    s_gov_on = on;
    s_gov_load = 0;
    s_gov_next = s_np + 120;
    if (!on && (s_gov_sx || s_gov_sy)) { s_gov_sx = s_gov_sy = 0; s_gov_changed = true; }
}

bool smooth_scale_cap(int *sx, int *sy) {
    if (!s_gov_changed) return false;
    s_gov_changed = false;
    *sx = s_gov_sx; *sy = s_gov_sy;
    return true;
}

/* one step down from the current scale: the next smaller of these (pixels per 1x pixel) */
static void governor_step(void) {
    static const int steps[][2] = { {4, 6}, {4, 4}, {3, 4}, {3, 3}, {2, 3}, {2, 2}, {1, 2}, {1, 1} };
    int cx, cy;
    if (!hires_get_scale(&cx, &cy) || cx * cy <= 1) return;
    for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++)
        if (steps[k][0] * steps[k][1] < cx * cy && steps[k][0] <= cx && steps[k][1] <= cy) {
            s_gov_sx = steps[k][0]; s_gov_sy = steps[k][1];
            s_gov_changed = true;
            s_gov_steps++;
            fprintf(stderr, "smooth: this computer is too slow for render scale %dx%d (%.0f%% of the frame budget used): "
                    "lowering it to %dx%d for the netplay session\n", cx, cy, 100 * s_gov_load, s_gov_sx, s_gov_sy);
            return;
        }
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
    if (s_gov_on) {
        /* the frame budget is blown on average (not a single rollback spike): one scale step down,
         * then give the average time to settle before the next */
        s_gov_load += (load - s_gov_load) / 30;
        if (s_np >= s_gov_next && s_gov_load > 0.9) { governor_step(); s_gov_next = s_np + 150; s_gov_load = 0.6; }
    }
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
            "render scale lowered %d times\n",
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
            s_gov_steps);
    fprintf(stderr,
            "smooth-json: {\"from\": %llu, \"presents\": %zu, \"iv_p50\": %.2f, \"iv_p95\": %.2f, \"iv_p99\": %.2f, "
            "\"iv_max\": %.2f, \"iv_sd\": %.3f, \"held\": %llu, \"held_pct\": %.3f, \"held_extra\": %llu, \"held2\": %llu, "
            "\"held_rollback\": %llu, \"held_stall\": %llu, \"held_sync\": %llu, \"held_slow\": %llu, \"busy_pct\": %.1f, "
            "\"over_budget_pct\": %.2f, \"stalls\": %llu, \"stall_ms\": %.1f, \"stall_max_ms\": %.2f, \"sync_frames\": %llu, "
            "\"sync_ms\": %.1f, \"rollbacks\": %llu, \"resim_avg\": %.3f, \"resim_p95\": %.3f, \"resim_max\": %.3f, "
            "\"rollback_late\": %llu, \"corrections\": %llu, \"correction_frames\": %llu, \"audio_under\": %llu, "
            "\"audio_low\": %llu, \"audio_repeat\": %llu, \"audio_frames\": %llu, \"scale_steps\": %d, \"depth\": [",
            (unsigned long long)s_from, s_iv_n + (s_iv_n ? 1 : 0), pct(s_iv, s_iv_n, 0.5), pct(s_iv, s_iv_n, 0.95),
            pct(s_iv, s_iv_n, 0.99), s_iv_n ? s_iv[s_iv_n - 1] / 1000.0 : 0, sd, (unsigned long long)s_held, held_pct,
            (unsigned long long)s_held_extra, (unsigned long long)s_held2, (unsigned long long)s_held_by[0],
            (unsigned long long)s_held_by[1], (unsigned long long)s_held_by[2], (unsigned long long)s_held_by[3], busy,
            over, (unsigned long long)s_stalls, s_stall_ns / 1e6, s_stall_max / 1e6, (unsigned long long)s_syncs,
            s_sync_ns / 1e6, (unsigned long long)s_rb, rb_avg, pct(s_rb_ns, s_rb_n, 0.95),
            s_rb_n ? s_rb_ns[s_rb_n - 1] / 1000.0 : 0, (unsigned long long)s_rb_late, (unsigned long long)s_pops,
            (unsigned long long)s_pop_frames, (unsigned long long)s_aud_under, (unsigned long long)s_aud_low,
            (unsigned long long)s_aud_repeat, (unsigned long long)s_aud_frames, s_gov_steps);
    for (int d = 0; d < 16; d++) fprintf(stderr, "%s%llu", d ? ", " : "", (unsigned long long)s_rb_depth[d]);
    fprintf(stderr, "]}\n");
}
