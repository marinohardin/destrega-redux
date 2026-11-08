/* Software high-resolution render target (see hires.h).
 *
 * Rules are the PS1 GPU's (same top-left fill rule, same colour/texture math,
 * same mask-bit and semi-transparency semantics), evaluated on a grid S times
 * finer than the 1x VRAM. At S = 1 the output is bit-identical to gpu.c's
 * framebuffer region (checked by the --hires-verify headless option).
 *
 *  - Coverage: edges are scaled by S and the same ceil() fill rule is applied
 *    to the scaled geometry, so a 1x w*h rect covers exactly (S*w)*(S*h) pixels.
 *  - Attributes (colour, u, v) are evaluated from the 1x plane equations at the
 *    centre of each hi-res pixel: P = (X + 0.5)/S - 0.5, which is X at S = 1.
 *  - Textures and CLUTs are fetched from the 1x VRAM (they are 1x assets).
 *  - Dither is off at S > 1 unless enabled (it then uses the 1x pixel position).
 *  - Lines are S pixels thick.
 *  - u/v are clamped before the texture-window mask to the range the 1x GPU actually sampled
 *    for that triangle (gpu.c records it; the vertex range if 1x sampled nothing): a hi-res
 *    pixel centre may fall outside the polygon, and the 1x GPU never draws a triangle's last
 *    column/row, so the vertex range still contains texels (often a neighbouring tile's border)
 *    that 1x never reads. Skipped at 1x1 (bit-identical there).
 *  - Large primitives are rasterized in row chunks on a worker pool (fork-join per
 *    primitive, so command order is preserved); only this path is threaded.
 */
#include "hires.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#endif

#define VRAM_W 1024
#define VRAM_H 512

static const u16 *s_vram;              /* 1x VRAM (texture/CLUT source) */
static u16 *s_t;                       /* target pixels */
static int s_Sx, s_Sy;                 /* current horizontal / vertical scale; 0 = none */
static int s_rx = 0, s_ry = 0, s_rw = 512, s_rh = 480;   /* tracked 1x region (in use) */
static int s_cfg_rx = 0, s_cfg_ry = 0, s_cfg_rw = 512, s_cfg_rh = 480;   /* requested */
static int s_tw, s_th;                 /* target size in pixels */
static int s_ox, s_oy;                 /* hi-res coordinates of target pixel (0,0) */
static bool s_dither_hi;

static const s8 s_dither[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline s64 floor_div128(__int128 a, s64 b)
{
    __int128 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return (s64)q;
}

/* ---- framebuffer-sampling diagnostics ------------------------------------ */

static bool s_fb_tex_seen[96];

static void note_tex_page(const HiresPrim *p)
{
    int pw = 64 << p->depth;
    if ((int)p->tp_x + pw <= s_rx || (int)p->tp_x >= s_rx + s_rw) return;
    if ((int)p->tp_y + 256 <= s_ry || (int)p->tp_y >= s_ry + s_rh) return;
    int key = (int)((p->tp_x / 64) & 15) + 16 * (int)((p->tp_y / 256) & 1) + 32 * p->depth;
    if (s_fb_tex_seen[key]) return;
    s_fb_tex_seen[key] = true;
    rt_log("hires: textured primitive samples the framebuffer region (page x=%u y=%u, %s); "
           "sampling the 1x VRAM there", p->tp_x, p->tp_y,
           p->depth == 0 ? "4bpp" : p->depth == 1 ? "8bpp" : "15bpp");
}

/* ---- pixel pipeline ------------------------------------------------------- */

static inline u16 tex_fetch(const HiresPrim *p, int depth, u32 u, u32 v)
{
    u = (u & p->tw_and_u) | p->tw_or_u;
    v = (v & p->tw_and_v) | p->tw_or_v;
    const u16 *row = &s_vram[((p->tp_y + v) & 511) * VRAM_W];
    if (depth == 0) {
        u16 w = row[(p->tp_x + (u >> 2)) & 1023];
        u32 idx = (w >> ((u & 3) * 4)) & 0xF;
        return s_vram[p->clut_y * VRAM_W + ((p->clut_x + idx) & 1023)];
    } else if (depth == 1) {
        u16 w = row[(p->tp_x + (u >> 1)) & 1023];
        u32 idx = (w >> ((u & 1) * 8)) & 0xFF;
        return s_vram[p->clut_y * VRAM_W + ((p->clut_x + idx) & 1023)];
    } else {
        return row[(p->tp_x + u) & 1023];
    }
}

static inline u16 blend_px(int mode, u16 b, u16 f)
{
    int br = b & 0x1F, bg = (b >> 5) & 0x1F, bb = (b >> 10) & 0x1F;
    int fr = f & 0x1F, fg = (f >> 5) & 0x1F, fb = (f >> 10) & 0x1F;
    int r, g, bl;
    switch (mode) {
    case 0:
        r = (br + fr) >> 1; g = (bg + fg) >> 1; bl = (bb + fb) >> 1;
        break;
    case 1:
        r = br + fr; g = bg + fg; bl = bb + fb;
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (bl > 31) bl = 31;
        break;
    case 2:
        r = br - fr; g = bg - fg; bl = bb - fb;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (bl < 0) bl = 0;
        break;
    default:
        r = br + (fr >> 2); g = bg + (fg >> 2); bl = bb + (fb >> 2);
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (bl > 31) bl = 31;
        break;
    }
    return (u16)(r | (g << 5) | (bl << 10) | (f & 0x8000));
}

/* Colour of a textured pixel from an already fetched texel t. False for a transparent texel. */
static inline __attribute__((always_inline)) bool
shade_tex(const HiresPrim *p, u16 t, int x1, int y1, u32 r, u32 g, u32 b,
          const int RAW, const int SEMI, const int DITHER, u16 *out, bool *blend)
{
    if (t == 0) return false;
    if (RAW) {
        *out = t;
    } else {
        int tr = t & 0x1F, tg = (t >> 5) & 0x1F, tb = (t >> 10) & 0x1F;
        if (DITHER) {
            int d = s_dither[y1 & 3][x1 & 3];
            tr = clamp255((int)((tr * r) >> 4) + d) >> 3;
            tg = clamp255((int)((tg * g) >> 4) + d) >> 3;
            tb = clamp255((int)((tb * b) >> 4) + d) >> 3;
        } else {
            tr = (int)((tr * r) >> 7); if (tr > 31) tr = 31;
            tg = (int)((tg * g) >> 7); if (tg > 31) tg = 31;
            tb = (int)((tb * b) >> 7); if (tb > 31) tb = 31;
        }
        *out = (u16)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
    }
    *blend = SEMI && (t & 0x8000);
    (void)p;
    return true;
}

/* Compute the colour of one pixel. Returns false for a transparent texel.
 * (x1, y1) is the 1x pixel the sample belongs to (dither pattern position). */
static inline __attribute__((always_inline)) bool
shade(const HiresPrim *p, int x1, int y1, u32 r, u32 g, u32 b, u32 u, u32 v,
      const int TEX, const int DEPTH, const int RAW, const int SEMI, const int DITHER,
      u16 *out, bool *blend)
{
    if (TEX) return shade_tex(p, tex_fetch(p, DEPTH, u, v), x1, y1, r, g, b, RAW, SEMI, DITHER, out, blend);
    int cr, cg, cb;
    if (DITHER) {
        int d = s_dither[y1 & 3][x1 & 3];
        cr = clamp255((int)r + d) >> 3;
        cg = clamp255((int)g + d) >> 3;
        cb = clamp255((int)b + d) >> 3;
    } else {
        cr = (int)r >> 3; cg = (int)g >> 3; cb = (int)b >> 3;
    }
    *out = (u16)(cr | (cg << 5) | (cb << 10));
    *blend = SEMI;
    return true;
}

static inline void store_px(const HiresPrim *p, u16 *dst, u16 out, bool blend)
{
    if (p->mask_check && (*dst & 0x8000)) return;
    if (blend) out = blend_px(p->blend, *dst, out);
    *dst = out | p->mask_or;
}

/* ---- worker pool, counters, dirty tracking ----------------------------------------- */

typedef void (*BandFn)(const void *ctx, int y0, int y1);   /* rows [y0, y1) in target coordinates */

static atomic_ullong s_uv_samples, s_uv_clamped, s_uv_escaped, s_uv_tris, s_uv_tris_1x;
static _Thread_local u64 tl_samples, tl_clamped, tl_escaped;

static void flush_counters(void)
{
    if (tl_samples) { atomic_fetch_add_explicit(&s_uv_samples, tl_samples, memory_order_relaxed); tl_samples = 0; }
    if (tl_clamped) { atomic_fetch_add_explicit(&s_uv_clamped, tl_clamped, memory_order_relaxed); tl_clamped = 0; }
    if (tl_escaped) { atomic_fetch_add_explicit(&s_uv_escaped, tl_escaped, memory_order_relaxed); tl_escaped = 0; }
}

void hires_get_uv_stats(HiresUvStats *st)
{
    st->samples = atomic_load(&s_uv_samples);
    st->clamped = atomic_load(&s_uv_clamped);
    st->escaped = atomic_load(&s_uv_escaped);
    st->tris = atomic_load(&s_uv_tris);
    st->tris_1x_range = atomic_load(&s_uv_tris_1x);
}

u64 g_hires_stat_inline_px, g_hires_stat_par_px, g_hires_stat_par_jobs, g_hires_stat_inline_jobs;
static int s_par_threshold = 4000;   /* hi-res pixels (bbox) below which a primitive runs inline */
void hires_set_parallel_threshold(int px) { s_par_threshold = px; }

static u64 s_epoch;                   /* bumped when the target is (re)allocated */
static int s_dirty_y0 = 1 << 30, s_dirty_y1 = -1;   /* 1x rows drawn since the last clear */
static inline void mark_dirty(int y0, int y1)
{
    if (y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
}
static inline void mark_rows_wrapped(int y, int h)
{
    int a = y & 511, b = (y + h - 1) & 511;
    if (b < a || h > 511) { a = 0; b = 511; }
    mark_dirty(a, b);
}
u64 hires_epoch(void) { return s_epoch; }
bool hires_dirty_rows(int y0, int y1) { return s_dirty_y1 >= y0 && s_dirty_y0 < y1; }
void hires_clear_dirty(void) { s_dirty_y0 = 1 << 30; s_dirty_y1 = -1; }

#ifndef _WIN32
#define HIRES_MAX_THREADS 15
#define RING 64
#define SPIN_ITERS 40000

typedef struct {
    BandFn fn;
    const void *ctx;
    int ylo, chunk_rows, nchunks;
    _Atomic u64 ctr;                  /* (job tag << 32) | next chunk index */
    atomic_int done;
} PoolJob;

static struct {
    pthread_t th[HIRES_MAX_THREADS];
    int nth;
    PoolJob ring[RING];
    atomic_uint gen;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    atomic_bool quit;
} P = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

/* Claim and run chunks of job `g`. Safe against stale workers: a chunk can only be claimed
 * while the slot still holds job g (tag in the counter), and a claimed chunk keeps the
 * dispatcher from reusing the slot until it is done. */
static void run_chunks(unsigned g)
{
    PoolJob *j = &P.ring[g % RING];
    for (;;) {
        u64 c = atomic_load_explicit(&j->ctr, memory_order_acquire);
        for (;;) {
            if ((unsigned)(c >> 32) != g || (unsigned)c >= (unsigned)j->nchunks) return;
            if (atomic_compare_exchange_weak_explicit(&j->ctr, &c, c + 1, memory_order_acq_rel,
                                                      memory_order_acquire))
                break;
        }
        int idx = (int)(unsigned)c;
        int y0 = j->ylo + idx * j->chunk_rows;
        int y1 = y0 + j->chunk_rows;
        int yend = j->ylo + j->chunk_rows * j->nchunks;   /* callers pass exact ranges; clamp below */
        (void)yend;
        j->fn(j->ctx, y0, y1);
        flush_counters();
        atomic_fetch_add_explicit(&j->done, 1, memory_order_release);
    }
}

static void *worker_main(void *arg)
{
    (void)arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    unsigned seen = atomic_load(&P.gen);
    for (;;) {
        unsigned g;
        long spins = 0;
        while ((g = atomic_load_explicit(&P.gen, memory_order_acquire)) == seen) {
            if (atomic_load_explicit(&P.quit, memory_order_relaxed)) return NULL;
            if (++spins < SPIN_ITERS) { cpu_relax(); continue; }
            pthread_mutex_lock(&P.mu);
            while (atomic_load(&P.gen) == seen && !atomic_load(&P.quit)) pthread_cond_wait(&P.cv, &P.mu);
            pthread_mutex_unlock(&P.mu);
            spins = 0;
        }
        /* process every job published since we last looked (the latest is normally the only
         * one with chunks left) */
        for (unsigned k = seen + 1; k != g + 1; k++) run_chunks(k);
        seen = g;
    }
}

static void stop_workers(void)
{
    if (!P.nth) return;
    pthread_mutex_lock(&P.mu);
    atomic_store(&P.quit, true);
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    for (int i = 0; i < P.nth; i++) pthread_join(P.th[i], NULL);
    P.nth = 0;
    atomic_store(&P.quit, false);
}

void hires_set_threads(int n)
{
    if (n < 0) {   /* auto: performance cores - 1 */
        long cores = 0;
#ifdef __APPLE__
        int v = 0; size_t sz = sizeof v;
        if (sysctlbyname("hw.perflevel0.logicalcpu", &v, &sz, NULL, 0) == 0) cores = v;
#endif
        if (cores <= 0) cores = sysconf(_SC_NPROCESSORS_ONLN);
        n = (int)cores - 1;
    }
    if (n > HIRES_MAX_THREADS) n = HIRES_MAX_THREADS;
    if (n < 0) n = 0;
    if (n == P.nth) return;
    stop_workers();
    for (int i = 0; i < n; i++)
        if (pthread_create(&P.th[i], NULL, worker_main, NULL) != 0) break;
        else P.nth++;
}

int hires_threads(void) { return P.nth; }

static void par_rows(BandFn fn, const void *ctx, int ylo, int yhi)
{
    int rows = yhi - ylo;
    if (rows <= 0) return;
    if (P.nth == 0 || rows < 8) { fn(ctx, ylo, yhi); flush_counters(); return; }
    int parts = (P.nth + 1) * 4;
    int chunk_rows = (rows + parts - 1) / parts;
    if (chunk_rows < 4) chunk_rows = 4;
    int nchunks = (rows + chunk_rows - 1) / chunk_rows;
    unsigned g = atomic_load_explicit(&P.gen, memory_order_relaxed) + 1;
    PoolJob *j = &P.ring[g % RING];
    j->fn = fn; j->ctx = ctx; j->ylo = ylo; j->chunk_rows = chunk_rows; j->nchunks = nchunks;
    /* the last chunk may extend past yhi: BandFn implementations clip to their own bounds */
    atomic_store_explicit(&j->done, 0, memory_order_relaxed);
    atomic_store_explicit(&j->ctr, (u64)g << 32, memory_order_release);
    pthread_mutex_lock(&P.mu);
    atomic_store_explicit(&P.gen, g, memory_order_release);
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    run_chunks(g);
    while (atomic_load_explicit(&j->done, memory_order_acquire) != nchunks) cpu_relax();
}
#else
void hires_set_threads(int n) { (void)n; }
int hires_threads(void) { return 0; }
static void par_rows(BandFn fn, const void *ctx, int ylo, int yhi) { fn(ctx, ylo, yhi); flush_counters(); }
#endif

typedef struct { void (*fn)(void *, int, int); void *ctx; } UserJob;
static void user_band(const void *c, int y0, int y1)
{
    const UserJob *u = c;
    u->fn(u->ctx, y0, y1);
}
/* Public: run fn(ctx, y0, y1) over [0, rows) on the pool (fn must clip y1 to rows). */
void hires_parallel_rows(void (*fn)(void *ctx, int y0, int y1), void *ctx, int rows)
{
    UserJob u = { fn, ctx };
    par_rows(user_band, &u, 0, rows);
}

/* ---- triangles ------------------------------------------------------------- */

#define FRAC 24

typedef struct {
    s32 X1, Y1;      /* 1x, 16.16 */
    s64 Xh, Yh;      /* hi-res, 16.16 */
    const HiresVtx *v;
} HV;

typedef struct { HiresPrim p; HiresVtx v[3]; bool persp, precise; } TriArgs;

/* PGXP perspective-correct texturing (see be_tri) */
static bool s_persp = true;
static atomic_ullong s_tris_all, s_tris_persp;
void hires_set_perspective(bool enable) { s_persp = enable; }
void hires_get_persp_stats(u64 *tris, u64 *persp)
{
    *tris = atomic_load(&s_tris_all);
    *persp = atomic_load(&s_tris_persp);
}

static inline int ccol_clamp(s64 c, int lo, int hi) { return c < lo ? lo : (c > hi ? hi : (int)c); }

static inline __attribute__((always_inline)) void
tri_impl(const TriArgs *ta, int by0, int by1,
         const int TEX, const int DEPTH, const int RAW, const int SEMI,
         const int GOUR, const int DITHER)
{
    const HiresPrim *p = &ta->p;
    const HiresVtx *v0 = &ta->v[0], *v1 = &ta->v[1], *v2 = &ta->v[2];
    const int Sx = s_Sx, Sy = s_Sy;
    HV h0 = { v0->x, v0->y, (s64)v0->x * Sx, (s64)v0->y * Sy, v0 };
    HV h1 = { v1->x, v1->y, (s64)v1->x * Sx, (s64)v1->y * Sy, v1 };
    HV h2 = { v2->x, v2->y, (s64)v2->x * Sx, (s64)v2->y * Sy, v2 };
    HV *a = &h0, *b = &h1, *c = &h2, *t;
    if (b->Y1 < a->Y1) { t = a; a = b; b = t; }
    if (c->Y1 < b->Y1) { t = b; b = c; c = t; }
    if (b->Y1 < a->Y1) { t = a; a = b; b = t; }

    if (c->Y1 == a->Y1) return;
    const s64 denom = (s64)((s64)b->X1 - a->X1) * ((s64)c->Y1 - a->Y1) -
                      (s64)((s64)c->X1 - a->X1) * ((s64)b->Y1 - a->Y1);
    if (denom == 0) return;

    /* Attribute plane gradients per 1x pixel: identical to gpu.c (12-bit quotient << 12). */
    double gdx[5] = { 0 }, gdy[5] = { 0 };
    u32 gstep_x[5] = { 0 };   /* per hi-res pixel along x */
    u32 base[5];
    const u32 av[5] = { a->v->r, a->v->g, a->v->b, a->v->u, a->v->v };
    const u32 bv[5] = { b->v->r, b->v->g, b->v->b, b->v->u, b->v->v };
    const u32 cv[5] = { c->v->r, c->v->g, c->v->b, c->v->u, c->v->v };
    const int n_attr = TEX ? 5 : 3;
    for (int i = 0; i < 5; i++) base[i] = (av[i] << FRAC) + (1u << (FRAC - 1));
    if (!GOUR) { base[0] = av[0] << FRAC; base[1] = av[1] << FRAC; base[2] = av[2] << FRAC; }
    for (int i = 0; i < n_attr; i++) {
        if ((i < 3 && !GOUR) || (i >= 3 && !TEX)) continue;
        s64 n_x = (s64)((s32)bv[i] - (s32)av[i]) * ((s64)c->Y1 - a->Y1) -
                  (s64)((s32)cv[i] - (s32)av[i]) * ((s64)b->Y1 - a->Y1);
        s64 n_y = (s64)((s32)cv[i] - (s32)av[i]) * ((s64)b->X1 - a->X1) -
                  (s64)((s32)bv[i] - (s32)av[i]) * ((s64)c->X1 - a->X1);
        u32 dx = (u32)(s32)(s64)(((__int128)n_x << 28) / denom) << 12;
        u32 dy = (u32)(s32)(s64)(((__int128)n_y << 28) / denom) << 12;
        gdx[i] = (double)(s32)dx;
        gdy[i] = (double)(s32)dy;
        gstep_x[i] = (u32)((s32)dx / Sx);
    }
    const s64 ustep = (s64)(s32)gstep_x[3], vstep = (s64)(s32)gstep_x[4];
    /* Gouraud colour clamp. A hi-res pixel centre can lie outside every 1x sample position of
     * the triangle, and thin slivers have steep colour gradients, so plain interpolation there
     * extrapolates far past the colours the 1x draw produced; the 1x rasterizer's u32 colour then
     * wraps (a near-black additive sliver came out as saturated yellow-white lines). Clamp to the
     * per-channel range the 1x draw actually produced, taken from the wrapped 8-bit value exactly
     * as 1x computes it (so S=1 stays bit-identical: every hi-res sample is a 1x sample). If 1x
     * covered nothing, clamp the unwrapped value to the vertex colour range instead. */
    const s64 gs[3] = { (s64)(s32)gstep_x[0], (s64)(s32)gstep_x[1], (s64)(s32)gstep_x[2] };
    int cmin[3], cmax[3];
    const bool c1x = GOUR && p->has_cr;
    for (int i = 0; i < 3; i++) {
        if (c1x) { cmin[i] = p->cr_min[i]; cmax[i] = p->cr_max[i]; continue; }
        int lo = (int)av[i], hi = (int)av[i];
        if ((int)bv[i] < lo) lo = (int)bv[i];
        if ((int)bv[i] > hi) hi = (int)bv[i];
        if ((int)cv[i] < lo) lo = (int)cv[i];
        if ((int)cv[i] > hi) hi = (int)cv[i];
        cmin[i] = lo; cmax[i] = hi;
    }
#define CCOL(VAL, I) ((u32)(!GOUR ? (int)((VAL) >> FRAC) \
    : ccol_clamp(c1x ? (s64)(((u32)(VAL)) >> FRAC) : ((VAL) >> FRAC), cmin[I], cmax[I])))
    /* UV range samples are clamped to: what the 1x draw of this triangle sampled (see the file
     * comment), else the vertex range */
    int umin = 0, umax = 255, vmin = 0, vmax = 255;
    if (TEX && p->has_uvr) {
        umin = p->uvr_umin; umax = p->uvr_umax; vmin = p->uvr_vmin; vmax = p->uvr_vmax;
    } else if (TEX) {
        umin = (int)av[3]; umax = umin; vmin = (int)av[4]; vmax = vmin;
        const u32 *o[2] = { bv, cv };
        for (int k = 0; k < 2; k++) {
            if ((int)o[k][3] < umin) umin = (int)o[k][3];
            if ((int)o[k][3] > umax) umax = (int)o[k][3];
            if ((int)o[k][4] < vmin) vmin = (int)o[k][4];
            if ((int)o[k][4] > vmax) vmax = (int)o[k][4];
        }
    }
    const double ax = (double)a->X1 / 65536.0, ay = (double)a->Y1 / 65536.0;
    /* Perspective-correct u/v (PGXP depth on all three vertices): q = 1/w, u*q and v*q are
     * linear in screen space; per pixel u = (u*q)/q. Same sample positions as the affine path,
     * exact (not 12-bit truncated) gradients. The result is still clamped to the UV range the
     * 1x draw sampled: perspective u/v at a point inside the triangle are a convex combination
     * of the vertex u/v, so the clamp only bites outside it / at the far edges, as for affine. */
    const bool PERSP = TEX && ta->persp;
    double pq[3] = { 0 }, pdx[3] = { 0 }, pdy[3] = { 0 };   /* q, u*q, v*q at a; gradients */
    if (PERSP) {
        const HV *vv[3] = { a, b, c };
        double wmin = a->v->w;
        if (b->v->w < wmin) wmin = b->v->w;
        if (c->v->w < wmin) wmin = c->v->w;
        double f[3][3], X[3], Y[3];
        for (int k = 0; k < 3; k++) {
            double q = wmin / (double)vv[k]->v->w;   /* normalised to <= 1 */
            f[k][0] = q; f[k][1] = (double)vv[k]->v->u * q; f[k][2] = (double)vv[k]->v->v * q;
            X[k] = (double)vv[k]->X1 / 65536.0; Y[k] = (double)vv[k]->Y1 / 65536.0;
        }
        double den = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        for (int i = 0; i < 3; i++) {
            pq[i] = f[0][i];
            pdx[i] = ((f[1][i] - f[0][i]) * (Y[2] - Y[0]) - (f[2][i] - f[0][i]) * (Y[1] - Y[0])) / den;
            pdy[i] = ((f[2][i] - f[0][i]) * (X[1] - X[0]) - (f[1][i] - f[0][i]) * (X[2] - X[0])) / den;
        }
    }
    const double pstep[3] = { pdx[0] / Sx, pdx[1] / Sx, pdx[2] / Sx };

    const bool long_left = denom > 0;
    const s64 long_step = floor_div128(((__int128)(c->Xh - a->Xh)) << 32, c->Yh - a->Yh);

    const int cx1 = imax(p->clip_x1 * Sx, s_ox);
    const int cx2 = imin((p->clip_x2 + 1) * Sx, s_ox + s_tw) - 1;
    const int cy1 = imax(imax(p->clip_y1 * Sy, s_oy), by0);
    const int cy2 = imin(imin((p->clip_y2 + 1) * Sy, s_oy + s_th) - 1, by1 - 1);
    /* at 1x1 the sample positions equal the 1x GPU's (which wraps rather than clamps), so the
     * clamp is skipped to stay bit-identical; it only matters for sub-pixel sample offsets */
    const bool do_clamp = Sx > 1 || Sy > 1 || ta->precise;   /* PGXP moves samples off 1x's too */
    const bool mc = p->mask_check;
    const u16 mo = p->mask_or;
    const int blendm = p->blend;

    for (int part = 0; part < 2; part++) {
        const HV *sa = part ? b : a, *sb = part ? c : b;
        s64 ys = (sa->Yh + 0xFFFF) >> 16, ye = (sb->Yh + 0xFFFF) >> 16;
        if (ys >= ye) continue;
        const s64 short_step = floor_div128(((__int128)(sb->Xh - sa->Xh)) << 32, sb->Yh - sa->Yh);
        int y_first = (int)(ys < cy1 ? cy1 : ys);
        int y_last = (int)(ye - 1 > cy2 ? cy2 : ye - 1);
        if (y_first > y_last) continue;
        s64 lx = (a->Xh << 16) + (s64)(((__int128)long_step * (((s64)y_first << 16) - a->Yh)) >> 16);
        s64 sx = (sa->Xh << 16) + (s64)(((__int128)short_step * (((s64)y_first << 16) - sa->Yh)) >> 16);
        for (int y = y_first; y <= y_last; y++) {
            s64 xl = long_left ? lx : sx, xr = long_left ? sx : lx;
            int xs = (int)((xl + 0xFFFFFFFFll) >> 32);
            int xe = (int)((xr + 0xFFFFFFFFll) >> 32);
            if (xs < cx1) xs = cx1;
            if (xe > cx2 + 1) xe = cx2 + 1;
            if (xs < xe) {
                /* attribute values at the first pixel of the span */
                const double P = ((double)xs + 0.5) / Sx - 0.5, Q = ((double)y + 0.5) / Sy - 0.5;
                /* colours are kept signed and unwrapped (s64) so they can be clamped; see CCOL */
                s64 r = base[0], g = base[1], bb = base[2];
                s64 U = 0, V = 0;
                if (GOUR) {
                    r = (s64)floor((double)base[0] + gdx[0] * (P - ax) + gdy[0] * (Q - ay));
                    g = (s64)floor((double)base[1] + gdx[1] * (P - ax) + gdy[1] * (Q - ay));
                    bb = (s64)floor((double)base[2] + gdx[2] * (P - ax) + gdy[2] * (Q - ay));
                }
                if (TEX && !PERSP) {   /* true (unwrapped) fixed-point values, so the clamp sees real excursions */
                    U = (s64)floor((double)base[3] + gdx[3] * (P - ax) + gdy[3] * (Q - ay));
                    V = (s64)floor((double)base[4] + gdx[4] * (P - ax) + gdy[4] * (Q - ay));
                }
                double pq0 = 0, pu = 0, pv = 0;
                if (PERSP) {
                    pq0 = pq[0] + pdx[0] * (P - ax) + pdy[0] * (Q - ay);
                    pu = pq[1] + pdx[1] * (P - ax) + pdy[1] * (Q - ay);
                    pv = pq[2] + pdx[2] * (P - ax) + pdy[2] * (Q - ay);
                }
                u16 *dst = s_t + (size_t)(y - s_oy) * s_tw + (xs - s_ox);
                const int y1 = y / Sy;
                int lu = -1, lv = -1;
                u16 lt = 0;
                u32 n_clamped = 0, n_escaped = 0;
                s64 pus = 0, pvs = 0;   /* PERSP: affine step inside the current 8-pixel run */
                for (int x = xs; x < xe; x++, dst++) {
                    u16 out; bool blend, ok;
                    if (PERSP && !((x - xs) & 7)) {
                        /* exact u/v every 8 hi-res pixels (2 1x pixels at 4x), linear in between;
                         * +0.5 texel: same rounding as the affine base */
                        int n = xe - x < 8 ? xe - x : 8;
                        double q1 = pq0 + n * pstep[0], u1 = pu + n * pstep[1], v1 = pv + n * pstep[2];
                        double iq0 = pq0 > 1e-9 ? 16777216.0 / pq0 : 0, iq1 = q1 > 1e-9 ? 16777216.0 / q1 : 0;
                        U = (s64)floor(pu * iq0) + (1 << (FRAC - 1));
                        V = (s64)floor(pv * iq0) + (1 << (FRAC - 1));
                        s64 U1 = (s64)floor(u1 * iq1) + (1 << (FRAC - 1));
                        s64 V1 = (s64)floor(v1 * iq1) + (1 << (FRAC - 1));
                        pus = (U1 - U) / n; pvs = (V1 - V) / n;
                        pq0 = q1; pu = u1; pv = v1;
                    }
                    if (!(mc && (*dst & 0x8000))) {
                        if (TEX) {
                            int ui = (int)(U >> FRAC), vi = (int)(V >> FRAC);
                            if (do_clamp) {
                                n_clamped += (ui < umin) | (ui > umax) | (vi < vmin) | (vi > vmax);
                                ui = ui < umin ? umin : (ui > umax ? umax : ui);
                                vi = vi < vmin ? vmin : (vi > vmax ? vmax : vi);
                                __asm__ volatile("" : "+r"(ui), "+r"(vi));   /* check what is really fetched */
                                n_escaped += (ui < umin) | (ui > umax) | (vi < vmin) | (vi > vmax);
                            }
                            if (ui != lu || vi != lv) { lu = ui; lv = vi; lt = tex_fetch(p, DEPTH, (u32)ui, (u32)vi); }
                            ok = shade_tex(p, lt, DITHER ? x / Sx : 0, y1, CCOL(r, 0), CCOL(g, 1), CCOL(bb, 2),
                                           RAW, SEMI, DITHER, &out, &blend);
                        } else {
                            ok = shade(p, DITHER ? x / Sx : 0, y1, CCOL(r, 0), CCOL(g, 1), CCOL(bb, 2), 0, 0,
                                       0, DEPTH, RAW, SEMI, DITHER, &out, &blend);
                        }
                        if (ok) {
                            if (blend) out = blend_px(blendm, *dst, out);
                            *dst = out | mo;
                        }
                    }
                    if (GOUR) { r += gs[0]; g += gs[1]; bb += gs[2]; }
                    if (PERSP) { U += pus; V += pvs; }
                    else if (TEX) { U += ustep; V += vstep; }
                }
                if (TEX) { tl_samples += (u64)(xe - xs); tl_clamped += n_clamped; tl_escaped += n_escaped; }
            }
            lx += long_step;
            sx += short_step;
        }
    }
}

#define TRI_FN(name, TEX, DEPTH, RAW, SEMI, GOUR, DITHER)                                  \
    static void name(const void *ctx, int y0, int y1)                                      \
    { tri_impl((const TriArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI, GOUR, DITHER); }

TRI_FN(tri_u000, 0, 0, 0, 0, 0, 0)
TRI_FN(tri_u001, 0, 0, 0, 0, 0, 1)
TRI_FN(tri_u010, 0, 0, 0, 0, 1, 0)
TRI_FN(tri_u011, 0, 0, 0, 0, 1, 1)
TRI_FN(tri_u100, 0, 0, 0, 1, 0, 0)
TRI_FN(tri_u101, 0, 0, 0, 1, 0, 1)
TRI_FN(tri_u110, 0, 0, 0, 1, 1, 0)
TRI_FN(tri_u111, 0, 0, 0, 1, 1, 1)
static const BandFn s_tri_untex[8] = {
    tri_u000, tri_u001, tri_u010, tri_u011, tri_u100, tri_u101, tri_u110, tri_u111,
};

#define TRI_TEX_DEPTH(D)                                   \
    TRI_FN(tri_t##D##_raw0, 1, D, 1, 0, 0, 0)              \
    TRI_FN(tri_t##D##_raw1, 1, D, 1, 1, 0, 0)              \
    TRI_FN(tri_t##D##_m000, 1, D, 0, 0, 0, 0)              \
    TRI_FN(tri_t##D##_m001, 1, D, 0, 0, 0, 1)              \
    TRI_FN(tri_t##D##_m010, 1, D, 0, 0, 1, 0)              \
    TRI_FN(tri_t##D##_m011, 1, D, 0, 0, 1, 1)              \
    TRI_FN(tri_t##D##_m100, 1, D, 0, 1, 0, 0)              \
    TRI_FN(tri_t##D##_m101, 1, D, 0, 1, 0, 1)              \
    TRI_FN(tri_t##D##_m110, 1, D, 0, 1, 1, 0)              \
    TRI_FN(tri_t##D##_m111, 1, D, 0, 1, 1, 1)
TRI_TEX_DEPTH(0)
TRI_TEX_DEPTH(1)
TRI_TEX_DEPTH(2)
#define TRI_TEX_TABLE(D)                                                          \
    { tri_t##D##_raw0, tri_t##D##_raw1, tri_t##D##_m000, tri_t##D##_m001,         \
      tri_t##D##_m010, tri_t##D##_m011, tri_t##D##_m100, tri_t##D##_m101,         \
      tri_t##D##_m110, tri_t##D##_m111 }
static const BandFn s_tri_tex[3][10] = {
    TRI_TEX_TABLE(0), TRI_TEX_TABLE(1), TRI_TEX_TABLE(2),
};

static inline bool dither_on(const HiresPrim *p) { return p->dither && ((s_Sx == 1 && s_Sy == 1) || s_dither_hi); }

static void be_tri(const HiresPrim *p, const HiresVtx v[3])
{
    if (!s_t) return;
    TriArgs ta;
    ta.p = *p;
    ta.p.dither = dither_on(p);
    memcpy(ta.v, v, sizeof ta.v);
    ta.precise = v[0].w > 0 || v[1].w > 0 || v[2].w > 0;
    ta.persp = s_persp && p->tex && v[0].w > 0 && v[1].w > 0 && v[2].w > 0;
    atomic_fetch_add_explicit(&s_tris_all, 1, memory_order_relaxed);
    if (ta.persp) atomic_fetch_add_explicit(&s_tris_persp, 1, memory_order_relaxed);
    BandFn fn;
    if (p->tex) {
        atomic_fetch_add_explicit(&s_uv_tris, 1, memory_order_relaxed);
        if (p->has_uvr) atomic_fetch_add_explicit(&s_uv_tris_1x, 1, memory_order_relaxed);
    }
    if (!p->tex) {
        int idx = (p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (ta.p.dither ? 1 : 0);
        fn = s_tri_untex[idx];
    } else {
        note_tex_page(p);
        int d = p->depth;
        if (p->raw) fn = s_tri_tex[d][p->semi ? 1 : 0];
        else fn = s_tri_tex[d][2 + ((p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (ta.p.dither ? 1 : 0))];
    }
    /* hi-res row / column extent of the triangle, intersected with clip and target */
    s32 miny = v[0].y, maxy = v[0].y, minx = v[0].x, maxx = v[0].x;
    for (int i = 1; i < 3; i++) {
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
    }
    int ylo = imax(imax((int)(((s64)miny * s_Sy) >> 16), p->clip_y1 * s_Sy), s_oy);
    int yhi = imin(imin((int)((((s64)maxy * s_Sy) >> 16) + 1), (p->clip_y2 + 1) * s_Sy), s_oy + s_th);
    int xlo = imax(imax((int)(((s64)minx * s_Sx) >> 16), p->clip_x1 * s_Sx), s_ox);
    int xhi = imin(imin((int)((((s64)maxx * s_Sx) >> 16) + 1), (p->clip_x2 + 1) * s_Sx), s_ox + s_tw);
    if (yhi <= ylo || xhi <= xlo) return;
    mark_dirty(ylo / s_Sy, (yhi - 1) / s_Sy);
    s64 area = (s64)(yhi - ylo) * (xhi - xlo);
    if (area >= s_par_threshold) { g_hires_stat_par_px += (u64)area; g_hires_stat_par_jobs++; par_rows(fn, &ta, ylo, yhi); }
    else { g_hires_stat_inline_px += (u64)area; g_hires_stat_inline_jobs++; fn(&ta, ylo, yhi); flush_counters(); }
}

/* ---- rectangles -------------------------------------------------------------- */

typedef struct {
    HiresPrim p;
    int x0, y0, w, h;
    u32 r, g, b, u0, v0;
    bool flipx, flipy;
} RectArgs;

static inline __attribute__((always_inline)) void
rect_impl(const RectArgs *ra, int by0, int by1,
          const int TEX, const int DEPTH, const int RAW, const int SEMI)
{
    const HiresPrim *p = &ra->p;
    const int Sx = s_Sx, Sy = s_Sy;
    const int x0 = ra->x0, y0 = ra->y0;
    int xs = x0, xe = x0 + ra->w, ys = y0, ye = y0 + ra->h;
    if (xs < p->clip_x1) xs = p->clip_x1;
    if (xe > p->clip_x2 + 1) xe = p->clip_x2 + 1;
    if (ys < p->clip_y1) ys = p->clip_y1;
    if (ye > p->clip_y2 + 1) ye = p->clip_y2 + 1;
    xs = imax(xs, s_rx); xe = imin(xe, s_rx + s_rw);
    ys = imax(ys, s_ry); ye = imin(ye, s_ry + s_rh);
    /* restrict to the band (hi-res rows [by0, by1)) */
    ys = imax(ys, by0 / Sy); ye = imin(ye, (by1 + Sy - 1) / Sy);
    if (xs >= xe || ys >= ye) return;
    const u32 r = ra->r, g = ra->g, b = ra->b;
    u32 u0 = ra->u0;
    const u32 v0 = ra->v0;
    int du = ra->flipx ? -1 : 1, dv = ra->flipy ? -1 : 1;
    if (TEX && ra->flipx) u0 |= 1;
    u8 ucol[VRAM_W];
    for (int x = xs; x < xe; x++) ucol[x - xs] = (u8)((u0 + (u32)(du * (x - x0))) & 0xFF);
    u16 outs[VRAM_W];
    u8 vis[VRAM_W];
    const int cnt = xe - xs;
    for (int y = ys; y < ye; y++) {
        u32 v = (v0 + (u32)(dv * (y - y0))) & 0xFF;
        for (int i = 0; i < cnt; i++) {
            bool blend;
            u16 o = 0;
            bool ok = shade(p, 0, 0, r, g, b, ucol[i], v, TEX, DEPTH, RAW, SEMI, 0, &o, &blend);
            outs[i] = o;
            vis[i] = ok ? (blend ? 2 : 1) : 0;
        }
        int sy0 = imax(0, by0 - y * Sy), sy1 = imin(Sy, by1 - y * Sy);
        for (int sy = sy0; sy < sy1; sy++) {
            u16 *dst = s_t + (size_t)(y * Sy + sy - s_oy) * s_tw + (xs * Sx - s_ox);
            for (int i = 0; i < cnt; i++) {
                if (!vis[i]) { dst += Sx; continue; }
                for (int sx = 0; sx < Sx; sx++, dst++) store_px(p, dst, outs[i], vis[i] == 2);
            }
        }
    }
}

#define RECT_FN(name, TEX, DEPTH, RAW, SEMI)                                                   \
    static void name(const void *ctx, int y0, int y1)                                          \
    { rect_impl((const RectArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI); }
RECT_FN(rect_u0, 0, 0, 0, 0)
RECT_FN(rect_u1, 0, 0, 0, 1)
#define RECT_DEPTH(D)                          \
    RECT_FN(rect_t##D##_00, 1, D, 0, 0)        \
    RECT_FN(rect_t##D##_01, 1, D, 0, 1)        \
    RECT_FN(rect_t##D##_10, 1, D, 1, 0)        \
    RECT_FN(rect_t##D##_11, 1, D, 1, 1)
RECT_DEPTH(0)
RECT_DEPTH(1)
RECT_DEPTH(2)
static const BandFn s_rect_tex[3][4] = {
    { rect_t0_00, rect_t0_01, rect_t0_10, rect_t0_11 },
    { rect_t1_00, rect_t1_01, rect_t1_10, rect_t1_11 },
    { rect_t2_00, rect_t2_01, rect_t2_10, rect_t2_11 },
};

static void be_rect(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g, u32 b,
                    u32 u0, u32 v0, bool fx, bool fy)
{
    if (!s_t) return;
    RectArgs ra = { *p, x, y, w, h, r, g, b, u0, v0, fx, fy };
    BandFn fn;
    if (!p->tex) fn = p->semi ? rect_u1 : rect_u0;
    else { note_tex_page(p); fn = s_rect_tex[p->depth][(p->raw ? 2 : 0) | (p->semi ? 1 : 0)]; }
    int ys = imax(imax(y, p->clip_y1), s_ry), ye = imin(imin(y + h, p->clip_y2 + 1), s_ry + s_rh);
    int xs = imax(imax(x, p->clip_x1), s_rx), xe = imin(imin(x + w, p->clip_x2 + 1), s_rx + s_rw);
    if (ys >= ye || xs >= xe) return;
    mark_dirty(ys, ye - 1);
    int ylo = ys * s_Sy, yhi = ye * s_Sy;
    if ((s64)(yhi - ylo) * (xe - xs) * s_Sx >= s_par_threshold) par_rows(fn, &ra, ylo, yhi);
    else fn(&ra, ylo, yhi);
}

/* ---- lines ----------------------------------------------------------------- */

static void be_line(const HiresPrim *p, int x0, int y0, u32 c0, int x1, int y1, u32 c1)
{
    if (!s_t) return;
    const int Sx = s_Sx, Sy = s_Sy;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    mark_dirty(imax((y0 < y1 ? y0 : y1) - 1, 0), (y0 > y1 ? y0 : y1) + 1);
    const bool gour = p->gour;
    if (!gour) c1 = c0;
    const bool dith = dither_on(p);
    const int k = adx > ady ? adx : ady;
    const bool xmajor = adx > ady;
    const int cx1 = imax(p->clip_x1 * Sx, s_ox), cx2 = imin((p->clip_x2 + 1) * Sx, s_ox + s_tw) - 1;
    const int cy1 = imax(p->clip_y1 * Sy, s_oy), cy2 = imin((p->clip_y2 + 1) * Sy, s_oy + s_th) - 1;

#define LINE_PLOT(PX, PY, CR, CG, CB)                                                          \
    do {                                                                                        \
        int px_ = (PX), py_ = (PY);                                                             \
        if (px_ >= cx1 && px_ <= cx2 && py_ >= cy1 && py_ <= cy2) {                             \
            u16 o_; bool bl_;                                                                   \
            u16 *d_ = s_t + (size_t)(py_ - s_oy) * s_tw + (px_ - s_ox);                         \
            if (!(p->mask_check && (*d_ & 0x8000)) &&                                           \
                shade(p, px_ / Sx, py_ / Sy, (CR), (CG), (CB), 0, 0, 0, 0, 0, p->semi, dith,    \
                      &o_, &bl_)) {                                                             \
                if (bl_) o_ = blend_px(p->blend, *d_, o_);                                      \
                *d_ = o_ | p->mask_or;                                                          \
            }                                                                                   \
        }                                                                                       \
    } while (0)

    if (k == 0) {
        for (int j = 0; j < Sy; j++)
            for (int i = 0; i < Sx; i++)
                LINE_PLOT(x0 * Sx + i, y0 * Sy + j, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF);
        return;
    }
    /* Thickness follows the scale of the axis perpendicular to the line's major axis
     * (x-major lines are Sy thick, y-major lines Sx thick). The major axis advances one
     * hi-res pixel per step; Sa is its scale. The line is extended by half a 1x pixel at
     * each end so that it covers whole 1x pixels like the 1x one. */
    const int Sa = xmajor ? Sx : Sy;
    /* centre of the first 1x pixel in hi-res coordinates, 32.32 */
    s64 xf = (((s64)x0 * Sx) << 32) + ((s64)Sx << 31);
    s64 yf = (((s64)y0 * Sy) << 32) + ((s64)Sy << 31);
    const int kh = k * Sa;
    s64 sx = (((s64)dx * Sx) << 32) / kh, sy = (((s64)dy * Sy) << 32) / kh;
    xf -= sx * (Sa - 1) / 2;
    yf -= sy * (Sa - 1) / 2;
    s32 r = (s32)((c0 & 0xFF) << 12), g = (s32)(((c0 >> 8) & 0xFF) << 12), b = (s32)(((c0 >> 16) & 0xFF) << 12);
    s32 dr = (s32)(((s32)(c1 & 0xFF) - (s32)(c0 & 0xFF)) * 4096 / kh);
    s32 dg = (s32)(((s32)((c1 >> 8) & 0xFF) - (s32)((c0 >> 8) & 0xFF)) * 4096 / kh);
    s32 db = (s32)(((s32)((c1 >> 16) & 0xFF) - (s32)((c0 >> 16) & 0xFF)) * 4096 / kh);
    if (dx < 0) xf -= 1;
    if (dy < 0) yf -= 1;
    r += 1 << 11; g += 1 << 11; b += 1 << 11;
    const int steps = kh + (Sa - 1);
    for (int i = 0; i <= steps; i++) {
        int x = (int)(xf >> 32), y = (int)(yf >> 32);
        u32 cr = (u32)(r >> 12), cg = (u32)(g >> 12), cb = (u32)(b >> 12);
        if (xmajor) {
            for (int j = 0; j < Sy; j++) LINE_PLOT(x, y - Sy / 2 + j, cr, cg, cb);
        } else {
            for (int j = 0; j < Sx; j++) LINE_PLOT(x - Sx / 2 + j, y, cr, cg, cb);
        }
        xf += sx; yf += sy;
        r += dr; g += dg; b += db;
    }
#undef LINE_PLOT
}

/* ---- VRAM operations ------------------------------------------------------------ */

/* Fill the 1x columns [x0, x1) of 1x row y (both inside the region) with c. */
static void fill_span(int x0, int x1, int y, u16 c)
{
    u16 *row0 = s_t + (size_t)(y * s_Sy - s_oy) * s_tw + (x0 * s_Sx - s_ox);
    const int n = (x1 - x0) * s_Sx;
    for (int i = 0; i < n; i++) row0[i] = c;
    for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)n * sizeof(u16));
}

static void be_fill(int x, int y, int w, int h, u16 c)
{
    if (!s_t) return;
    mark_rows_wrapped(y, h);
    for (int j = 0; j < h; j++) {
        int row = (y + j) & 511;
        if (row < s_ry || row >= s_ry + s_rh) continue;
        int xa = x, xb = x + w;
        if (xb <= VRAM_W) {
            int a = imax(xa, s_rx), b = imin(xb, s_rx + s_rw);
            if (a < b) fill_span(a, b, row, c);
        } else {
            int a = imax(xa, s_rx), b = imin(VRAM_W, s_rx + s_rw);
            if (a < b) fill_span(a, b, row, c);
            a = s_rx; b = imin(xb - VRAM_W, s_rx + s_rw);
            if (a < b) fill_span(a, b, row, c);
        }
    }
}

static inline bool in_region(int x, int y)
{
    return x >= s_rx && x < s_rx + s_rw && y >= s_ry && y < s_ry + s_rh;
}

static void be_put(int x, int y, u16 v, bool mc, bool ms)
{
    if (!s_t) return;
    x &= 1023; y &= 511;
    if (!in_region(x, y)) return;
    mark_dirty(y, y);
    const u16 val = v | (ms ? 0x8000 : 0);
    for (int sy = 0; sy < s_Sy; sy++) {
        u16 *d = s_t + (size_t)(y * s_Sy + sy - s_oy) * s_tw + (x * s_Sx - s_ox);
        for (int sx = 0; sx < s_Sx; sx++, d++) {
            if (mc && (*d & 0x8000)) continue;
            *d = val;
        }
    }
}

static void be_copy(int sx, int sy, int dx, int dy, int wd, int ht, bool mc, bool ms)
{
    if (!s_t) return;
    const int Sx = s_Sx, Sy = s_Sy;
    /* skip when the destination can't touch the region at all */
    bool any = false;
    for (int j = 0; j < ht && !any; j++) {
        int drow = (dy + j) & 511;
        if (drow >= s_ry && drow < s_ry + s_rh) any = true;
    }
    if (!any) return;
    mark_rows_wrapped(dy, ht);
    static u16 buf[(size_t)HIRES_MAX_SCALE * VRAM_W * HIRES_MAX_SCALE];
    const size_t stride = (size_t)wd * Sx;
    for (int j = 0; j < ht; j++) {
        int srow = (sy + j) & 511, drow = (dy + j) & 511;
        if (drow < s_ry || drow >= s_ry + s_rh) continue;
        for (int i = 0; i < wd; i++) {
            int scol = (sx + i) & 1023;
            bool in = in_region(scol, srow);
            for (int a = 0; a < Sy; a++)
                for (int b = 0; b < Sx; b++)
                    buf[a * stride + (size_t)i * Sx + b] = in
                        ? s_t[(size_t)(srow * Sy + a - s_oy) * s_tw + (scol * Sx + b - s_ox)]
                        : s_vram[srow * VRAM_W + scol];
        }
        for (int i = 0; i < wd; i++) {
            int dcol = (dx + i) & 1023;
            if (!in_region(dcol, drow)) continue;
            for (int a = 0; a < Sy; a++) {
                u16 *d = s_t + (size_t)(drow * Sy + a - s_oy) * s_tw + (dcol * Sx - s_ox);
                for (int b = 0; b < Sx; b++, d++) {
                    if (mc && (*d & 0x8000)) continue;
                    *d = buf[a * stride + (size_t)i * Sx + b] |(ms ? 0x8000 : 0);
                }
            }
        }
    }
}

static void seed(void)
{
    for (int y = 0; y < s_rh; y++) {
        u16 *row0 = s_t + (size_t)y * s_Sy * s_tw;
        const u16 *src = &s_vram[(s_ry + y) * VRAM_W + s_rx];
        for (int x = 0; x < s_rw; x++)
            for (int i = 0; i < s_Sx; i++) row0[x * s_Sx + i] = src[x];
        for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)s_tw * sizeof(u16));
    }
}

static void be_destroy(void)
{
    free(s_t);
    s_t = NULL;
    s_Sx = s_Sy = 0;
}

static bool be_configure(const u16 *vram1x, int sx, int sy, int rx, int ry, int rw, int rh)
{
    if (sx < 1 || sx > HIRES_MAX_SCALE || sy < 1 || sy > HIRES_MAX_SCALE || rw <= 0 || rh <= 0 ||
        rx < 0 || ry < 0 || rx + rw > VRAM_W || ry + rh > VRAM_H)
        return false;
    u16 *nt = malloc((size_t)rw * sx * rh * sy * sizeof(u16));
    if (!nt) return false;
    free(s_t);
    s_t = nt;
    s_vram = vram1x;
    s_Sx = sx; s_Sy = sy;
    s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
    s_tw = rw * sx; s_th = rh * sy;
    s_ox = rx * sx; s_oy = ry * sy;
    memset(s_fb_tex_seen, 0, sizeof s_fb_tex_seen);
    seed();
    s_epoch++;
    mark_dirty(0, VRAM_H);
    return true;
}

static bool be_view(int x, int y, int w, int h, HiresView *out)
{
    if (!s_t || w <= 0 || h <= 0) return false;
    if (x < s_rx || y < s_ry || x + w > s_rx + s_rw || y + h > s_ry + s_rh) return false;
    out->pixels = s_t + (size_t)(y * s_Sy - s_oy) * s_tw + (x * s_Sx - s_ox);
    out->stride = s_tw;
    out->w = w * s_Sx;
    out->h = h * s_Sy;
    out->scale_x = s_Sx;
    out->scale_y = s_Sy;
    return true;
}

static const HiresBackend s_soft = {
    "software", be_configure, be_tri, be_rect, be_line, be_fill, be_copy, be_put, be_view, be_destroy,
};

/* ---- control --------------------------------------------------------------------- */

bool hires_set_scale(int sx, int sy)
{
    if (sx <= 0 || sy <= 0) sx = sy = 0;
    if (sx > HIRES_MAX_SCALE) sx = HIRES_MAX_SCALE;
    if (sy > HIRES_MAX_SCALE) sy = HIRES_MAX_SCALE;
    if (sx == 0) {
        if (g_hires) g_hires->destroy();
        g_hires = NULL;
        return true;
    }
    if (g_hires && s_Sx == sx && s_Sy == sy && s_rx == s_cfg_rx && s_ry == s_cfg_ry &&
        s_rw == s_cfg_rw && s_rh == s_cfg_rh)
        return true;
    if (!s_soft.configure(gpu_vram(), sx, sy, s_cfg_rx, s_cfg_ry, s_cfg_rw, s_cfg_rh)) {
        g_hires = NULL;
        return false;
    }
    g_hires = &s_soft;
    return true;
}

bool hires_get_scale(int *sx, int *sy)
{
    if (sx) *sx = g_hires ? s_Sx : 0;
    if (sy) *sy = g_hires ? s_Sy : 0;
    return g_hires != NULL;
}

void hires_set_region(int rx, int ry, int rw, int rh)
{
    s_cfg_rx = rx; s_cfg_ry = ry; s_cfg_rw = rw; s_cfg_rh = rh;
}

void hires_set_dither(bool enable) { s_dither_hi = enable; }

bool hires_get_view(const GpuDisplay *d, HiresView *v)
{
    if (!g_hires || !d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) return false;
    return g_hires->view(d->x, d->y, d->w, d->h, v);
}

void hires_pick_scale(int pw, int ph, int dw, int dh, int max_x, int max_y, int *sx, int *sy)
{
    if (max_x < 1) max_x = 1;
    if (max_x > HIRES_MAX_SCALE) max_x = HIRES_MAX_SCALE;
    if (max_y < 1) max_y = 1;
    if (max_y > HIRES_MAX_SCALE) max_y = HIRES_MAX_SCALE;
    if (dw < 1) dw = 512;
    if (dh < 1) dh = 240;
    int vw = 1, vh = 1;
    if (pw >= 1 && ph >= 1) {                    /* 4:3 letterbox viewport */
        if ((s64)pw * 3 > (s64)ph * 4) { vh = ph; vw = ph * 4 / 3; }
        else { vw = pw; vh = pw * 3 / 4; }
    }
    /* never upsample at present: at least one target pixel per viewport pixel on each axis */
    *sx = imin(imax((vw + dw - 1) / dw, 1), max_x);
    *sy = imin(imax((vh + dh - 1) / dh, 1), max_y);
}

u32 hires_verify_1x(int *first_x, int *first_y)
{
    if (!g_hires || s_Sx != 1 || s_Sy != 1) return 0;
    u32 bad = 0;
    for (int y = 0; y < s_rh; y++)
        for (int x = 0; x < s_rw; x++)
            if (s_t[y * s_tw + x] != s_vram[(s_ry + y) * VRAM_W + s_rx + x]) {
                if (!bad) { if (first_x) *first_x = s_rx + x; if (first_y) *first_y = s_ry + y; }
                bad++;
            }
    return bad;
}
