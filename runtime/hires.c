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
#include <limits.h>
#include <stdatomic.h>
#include "platform.h"

#define VRAM_W 1024
#define VRAM_H 512

static const u16 *s_vram;              /* 1x VRAM (texture/CLUT source) */
static u16 *s_t;                       /* target pixels (HIRES_FMT_15), else NULL */
static u32 *s_t32;                     /* target pixels (HIRES_FMT_RGBA8), else NULL */
#define HAVE_T (s_t || s_t32)
static int s_tex_filter;               /* HIRES_TEX_* (hires_set_tex_filter) */
static int s_Sx, s_Sy;                 /* current horizontal / vertical scale; 0 = none */
static int s_rx = 0, s_ry = 0, s_rw = 512, s_rh = 480;   /* tracked 1x region (in use) */
static int s_cfg_rx = 0, s_cfg_ry = 0, s_cfg_rw = 512, s_cfg_rh = 480;   /* requested */
static int s_tw, s_th;                 /* target size in pixels */
static int s_ox, s_oy;                 /* hi-res coordinates of target pixel (0,0) */
static int s_gx;                       /* guard band in use: virtual 1x columns each side */
static int s_pend_gx;                  /* guard band for the next configure() */
static int s_cfg_gx;                   /* requested via hires_set_guard() */
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

/* ---- full-precision pixels (HIRES_FMT_RGBA8) -------------------------------------------------
 * R | G << 8 | B << 16, channels on the PS1's 8-bit scale (15-bit c = c << 3), A = 0xFF << 24 for
 * the mask bit. Semi-transparency clamps like the hardware, in 8-bit units. */
#define A32 0xFF000000u
static inline u32 c15to32(u32 c)
{
    return ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | ((c & 0x8000) ? A32 : 0);
}

static inline u32 blend32(int mode, u32 b, u32 f)
{
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
    int fr = f & 255, fg = (f >> 8) & 255, fb = (f >> 16) & 255;
    int r, g, bl;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
    case 1: r = imin(br + fr, 255); g = imin(bg + fg, 255); bl = imin(bb + fb, 255); break;
    case 2: r = imax(br - fr, 0); g = imax(bg - fg, 0); bl = imax(bb - fb, 0); break;
    default: r = imin(br + (fr >> 2), 255); g = imin(bg + (fg >> 2), 255); bl = imin(bb + (fb >> 2), 255); break;
    }
    return (u32)r | ((u32)g << 8) | ((u32)bl << 16) | (f & A32);
}

/* ---- filtered texels ---------------------------------------------------------------------------
 * A texel sample is a weighted combination of CLUT-looked-up texels: channels in 5.8 fixed point
 * (a single texel t gives (t & 31) << 8), the semi-transparency bit by weighted majority. Colour
 * 0000 is transparent: it contributes no colour (the rest is renormalised, so no black fringes)
 * and the sample is transparent when the opaque texels weigh less than half. Integer only, the
 * same as both shaders. */
typedef struct { int r, g, b; bool stp; } Tex8;

static inline Tex8 tex8(u16 t)
{
    Tex8 o = { (t & 31) << 8, ((t >> 5) & 31) << 8, ((t >> 10) & 31) << 8, (t & 0x8000) != 0 };
    return o;
}

/* weights w[] sum to 65536 */
static inline bool tex_combine(const u16 *t, const u32 *w, int n, Tex8 *o)
{
    u32 cov = 0, sr = 0, sg = 0, sb = 0, ss = 0;
    for (int k = 0; k < n; k++) {
        if (!t[k] || !w[k]) continue;
        cov += w[k];
        sr += w[k] * (t[k] & 31u);
        sg += w[k] * ((t[k] >> 5) & 31u);
        sb += w[k] * ((t[k] >> 10) & 31u);
        if (t[k] & 0x8000) ss += w[k];
    }
    if (cov < 32768) return false;
    if (cov == 65536) {   /* all taps opaque (the common case): the same rounding as a shift */
        o->r = (int)((sr + 128) >> 8); o->g = (int)((sg + 128) >> 8); o->b = (int)((sb + 128) >> 8);
    } else {
        o->r = (int)((sr * 256 + (cov >> 1)) / cov);
        o->g = (int)((sg * 256 + (cov >> 1)) / cov);
        o->b = (int)((sb * 256 + (cov >> 1)) / cov);
    }
    o->stp = ss * 2 >= cov;
    return true;
}

/* sampled texel modulated by the vertex colour (r, g, b 0..255; 128 = 1.0), 15-bit output.
 * For a single texel this is exactly shade_tex(). */
static inline u16 tex8_out15(Tex8 t, int r, int g, int b, const int RAW, const int DITHER, int d)
{
    int tr, tg, tb;
    if (RAW) { tr = t.r >> 8; tg = t.g >> 8; tb = t.b >> 8; }
    else if (DITHER) {
        tr = clamp255(((t.r * r) >> 12) + d) >> 3;
        tg = clamp255(((t.g * g) >> 12) + d) >> 3;
        tb = clamp255(((t.b * b) >> 12) + d) >> 3;
    } else {
        tr = imin((t.r * r) >> 15, 31); tg = imin((t.g * g) >> 15, 31); tb = imin((t.b * b) >> 15, 31);
    }
    return (u16)(tr | (tg << 5) | (tb << 10) | (t.stp ? 0x8000 : 0));
}

/* the same in full precision: the 8-bit value the PS1 truncates to 5 bits (>> 3 of it = tex8_out15) */
static inline u32 tex8_out32(Tex8 t, int r, int g, int b, const int RAW)
{
    int tr, tg, tb;
    if (RAW) { tr = t.r >> 5; tg = t.g >> 5; tb = t.b >> 5; }
    else { tr = imin((t.r * r) >> 12, 255); tg = imin((t.g * g) >> 12, 255); tb = imin((t.b * b) >> 12, 255); }
    return (u32)tr | ((u32)tg << 8) | ((u32)tb << 16) | (t.stp ? A32 : 0);
}

/* One axis of a filtered sample: texel index i (texel centres at integers) and fraction f (1/256
 * texel) towards i + 1, clamped so the footprint stays inside [lo, hi], the range the 1x draw
 * sampled: the sample point is clamped to [lo, hi] and i + 1 to hi. */
static inline void clamp_axis(int *i, int *f, int lo, int hi)
{
    if (*i < lo) { *i = lo; *f = 0; }
    else if (*i >= hi) { *i = hi; *f = 0; }
}

typedef struct { int ku, kv, kq; u16 t[5]; bool cond; } TCache;   /* last footprint fetched */
#define TCACHE_INIT { INT_MIN, INT_MIN, -1, { 0 }, false }

static inline bool samp_bilinear(const HiresPrim *p, int depth, int iu, int fu, int iv, int fv,
                                 int uhi, int vhi, TCache *c, Tex8 *o)
{
    if (iu != c->ku || iv != c->kv) {
        c->ku = iu; c->kv = iv;
        int iu1 = iu < uhi ? iu + 1 : uhi, iv1 = iv < vhi ? iv + 1 : vhi;
        c->t[0] = tex_fetch(p, depth, (u32)iu, (u32)iv);
        c->t[1] = tex_fetch(p, depth, (u32)iu1, (u32)iv);
        c->t[2] = tex_fetch(p, depth, (u32)iu, (u32)iv1);
        c->t[3] = tex_fetch(p, depth, (u32)iu1, (u32)iv1);
    }
    const u32 w[4] = { (u32)((256 - fu) * (256 - fv)), (u32)(fu * (256 - fv)), (u32)((256 - fu) * fv), (u32)(fu * fv) };
    return tex_combine(c->t, w, 4, o);
}

/* Edge-aware 2D filter (xBR-style, for pixel art at 1:1): the nearest texel E, except in the corner
 * of E the sample lies towards, cut off by the line through the midpoints of E's two edges there
 * (|dx| + |dy| > 1/2 texel), which takes the colour of the horizontal neighbour H when H equals the
 * vertical neighbour V and the pattern is a real diagonal edge, Scale2x's rule: H == V, H != E,
 * H != the opposite horizontal neighbour, V != the opposite vertical one. The cut is anti-aliased
 * over about one hi-res pixel. Neighbours are clamped to the 1x-sampled range like bilinear. */
static inline bool samp_xbr(const HiresPrim *p, int depth, int iu, int fu, int iv, int fv,
                            int ulo, int uhi, int vlo, int vhi, int Sx, int Sy, TCache *c, Tex8 *o)
{
    int ie = iu, dx = fu, je = iv, dy = fv;
    if (fu >= 128) { ie = iu + 1; dx = fu - 256; }
    if (fv >= 128) { je = iv + 1; dy = fv - 256; }
    const int hx = dx >= 0 ? 1 : -1, hy = dy >= 0 ? 1 : -1;
    const int q = (hx > 0) | ((hy > 0) << 1);
    if (ie != c->ku || je != c->kv || q != c->kq) {
        c->ku = ie; c->kv = je; c->kq = q;
        const int ih = imin(imax(ie + hx, ulo), uhi), iho = imin(imax(ie - hx, ulo), uhi);
        const int jv = imin(imax(je + hy, vlo), vhi), jvo = imin(imax(je - hy, vlo), vhi);
        c->t[0] = tex_fetch(p, depth, (u32)ie, (u32)je);
        c->t[1] = tex_fetch(p, depth, (u32)ih, (u32)je);
        c->t[2] = tex_fetch(p, depth, (u32)ie, (u32)jv);
        c->t[3] = tex_fetch(p, depth, (u32)iho, (u32)je);
        c->t[4] = tex_fetch(p, depth, (u32)ie, (u32)jvo);
        c->cond = c->t[1] == c->t[2] && c->t[1] != c->t[0] && c->t[1] != c->t[3] && c->t[2] != c->t[4];
    }
    u32 w[2] = { 65536, 0 };
    if (c->cond) {
        const int d8 = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) - 128;
        const int a = imin(imax(((d8 * (Sx + Sy)) >> 1) + 128, 0), 256);
        w[0] = (u32)(256 - a) * 256; w[1] = (u32)a * 256;
    }
    return tex_combine(c->t, w, 2, o);
}

/* Rect texel coordinate on one axis at hi-res pixel X (scale S): texel t0 + d*(k - k0) is centred
 * on 1x pixel k (d = +-1), so the sample point (X + 0.5)/S - 0.5 maps to
 * num / (2S) with num = 2S*t0 + d*(2X + 1 - S - 2S*k0). */
static inline void rect_axis(int t0, int d, int k0, int X, int S, int *i, int *f)
{
    const int D = 2 * S, num = D * t0 + d * (2 * X + 1 - S - D * k0);
    int q = num / D;
    if (num % D != 0 && num < 0) q--;
    *i = q;
    *f = ((num - q * D) * 256) / D;
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

#define HIRES_MAX_THREADS 15
#define RING 64
#define SPIN_ITERS 40000

typedef struct {
    BandFn fn;
    const void *ctx;
    int ylo, chunk_rows, nchunks;
    _Atomic u64 ctr;                  /* (job tag << 32) | (chunk count << 16) | next chunk index */
    atomic_int done;
} PoolJob;

static struct {
    plat_thread th[HIRES_MAX_THREADS];
    int nth;
    PoolJob ring[RING];
    atomic_uint gen;
    plat_mutex mu;
    plat_cond cv;
    atomic_bool quit;
} P = { .mu = PLAT_MUTEX_INIT, .cv = PLAT_COND_INIT };

static inline void cpu_relax(void) { plat_cpu_relax(); }

/* Claim and run chunks of job `g`. Safe against stale workers: a chunk can only be claimed
 * while the slot still holds job g (tag in the counter), and a claimed chunk keeps the
 * dispatcher from reusing the slot until it is done.
 * The chunk count lives in the same atomic word as the tag and the index: it used to be read
 * from the slot's plain field, so a worker 64 jobs behind (preempted on a loaded machine) could
 * read the next job's count while the counter still carried the old tag, win the CAS just before
 * the dispatcher published the new counter, run a chunk of the new job and add to `done` after
 * the reset: `done` then never equalled the chunk count and par_rows spun forever. */
static void run_chunks(unsigned g)
{
    PoolJob *j = &P.ring[g % RING];
    for (;;) {
        u64 c = atomic_load_explicit(&j->ctr, memory_order_acquire);
        for (;;) {
            if ((unsigned)(c >> 32) != g || (unsigned)(c & 0xffff) >= (unsigned)((c >> 16) & 0xffff))
                return;
            if (atomic_compare_exchange_weak_explicit(&j->ctr, &c, c + 1, memory_order_acq_rel,
                                                      memory_order_acquire))
                break;
        }
        int idx = (int)(c & 0xffff);
        int y0 = j->ylo + idx * j->chunk_rows;
        j->fn(j->ctx, y0, y0 + j->chunk_rows);   /* the last chunk may pass yhi: BandFns clip */
        flush_counters();
        atomic_fetch_add_explicit(&j->done, 1, memory_order_release);
    }
}

static void worker_main(void *arg)
{
    (void)arg;
    plat_thread_boost();
    unsigned seen = atomic_load(&P.gen);
    for (;;) {
        unsigned g;
        long spins = 0;
        while ((g = atomic_load_explicit(&P.gen, memory_order_acquire)) == seen) {
            if (atomic_load_explicit(&P.quit, memory_order_relaxed)) return;
            if (++spins < SPIN_ITERS) { cpu_relax(); continue; }
            plat_mutex_lock(&P.mu);
            while (atomic_load(&P.gen) == seen && !atomic_load(&P.quit)) plat_cond_wait(&P.cv, &P.mu);
            plat_mutex_unlock(&P.mu);
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
    plat_mutex_lock(&P.mu);
    atomic_store(&P.quit, true);
    plat_cond_broadcast(&P.cv);
    plat_mutex_unlock(&P.mu);
    for (int i = 0; i < P.nth; i++) plat_thread_join(P.th[i]);
    P.nth = 0;
    atomic_store(&P.quit, false);
}

void hires_set_threads(int n)
{
    if (n < 0) n = plat_perf_cpus() - 1;   /* auto: performance cores - 1 */
    if (n > HIRES_MAX_THREADS) n = HIRES_MAX_THREADS;
    if (n < 0) n = 0;
    if (n == P.nth) return;
    stop_workers();
    for (int i = 0; i < n; i++)
        if (!plat_thread_start(&P.th[i], worker_main, NULL)) break;
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
    atomic_store_explicit(&j->ctr, ((u64)g << 32) | ((u64)nchunks << 16), memory_order_release);
    plat_mutex_lock(&P.mu);
    atomic_store_explicit(&P.gen, g, memory_order_release);
    plat_cond_broadcast(&P.cv);
    plat_mutex_unlock(&P.mu);
    run_chunks(g);
    while (atomic_load_explicit(&j->done, memory_order_acquire) != nchunks) cpu_relax();
}

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

typedef struct { HiresPrim p; HiresVtx v[3]; bool persp, precise, xbr; } TriArgs;

/* PGXP perspective-correct texturing (see be_tri) */
static bool s_persp = true;
static atomic_ullong s_tris_all, s_tris_persp;
static atomic_ullong s_filt_tris, s_filt_2d, s_filt_rects;   /* --tex-filter: textured tris / of them 2D / rects */
void hires_get_filter_stats(u64 *tris, u64 *tris_2d, u64 *rects)
{
    *tris = atomic_load(&s_filt_tris);
    *tris_2d = atomic_load(&s_filt_2d);
    *rects = atomic_load(&s_filt_rects);
}
void hires_set_perspective(bool enable) { s_persp = enable; }
bool hires_get_perspective(void) { return s_persp; }
void hires_get_persp_stats(u64 *tris, u64 *persp)
{
    *tris = atomic_load(&s_tris_all);
    *persp = atomic_load(&s_tris_persp);
}

static inline int ccol_clamp(s64 c, int lo, int hi) { return c < lo ? lo : (c > hi ? hi : (int)c); }

static inline __attribute__((always_inline)) void
tri_impl(const TriArgs *ta, int by0, int by1,
         const int TEX, const int DEPTH, const int RAW, const int SEMI,
         const int GOUR, const int DITHER, const int FILT, const int HP)
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
    s64 gdx[5] = { 0 }, gdy[5] = { 0 };
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
        gdx[i] = (s32)dx;
        gdy[i] = (s32)dy;
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
    /* Exact span-start evaluation: E = base + gdx*(P - ax) + gdy*(Q - ay) with the sample point
     * P = (2xs+1-Sx)/(2Sx), Q = (2y+1-Sy)/(2Sy) (1x pixel units) and ax = X1/65536, taken as one
     * rational over D = 4*Sx*Sy*65536 and floored exactly. A double evaluation rounds exact-integer
     * values down for scales like Sy=6 (1/6 is inexact) and picks the neighbouring texel; the GPU
     * backend evaluates the same rational, so both agree bit for bit. */
    const s64 eD = (s64)4 * Sx * Sy * 65536;
    __int128 e0[5];
    for (int i = 0; i < 5; i++)
        e0[i] = (__int128)base[i] * eD - (__int128)gdx[i] * a->X1 * (4 * Sx * Sy) -
                (__int128)gdy[i] * a->Y1 * (4 * Sx * Sy);
#define EXACT_AT(I, XS, Y) floor_div128(e0[I] + (__int128)gdx[I] * ((s64)2 * (XS) + 1 - Sx) * (2 * Sy * 65536) \
        + (__int128)gdy[I] * ((s64)2 * (Y) + 1 - Sy) * (2 * Sx * 65536), eD)

    const bool long_left = denom > 0;
    const s64 long_step = floor_div128(((__int128)(c->Xh - a->Xh)) << 32, c->Yh - a->Yh);

    const int cx1 = imax(p->clip_x1 * Sx, s_ox);
    const int cx2 = imin((p->clip_x2 + 1) * Sx, s_ox + s_tw) - 1;
    const int cy1 = imax(imax(p->clip_y1 * Sy, s_oy), by0);
    const int cy2 = imin(imin((p->clip_y2 + 1) * Sy, s_oy + s_th) - 1, by1 - 1);
    /* at 1x1 the sample positions equal the 1x GPU's (which wraps rather than clamps), so the
     * clamp is skipped to stay bit-identical; it only matters for sub-pixel sample offsets */
    const bool do_clamp = Sx > 1 || Sy > 1 || ta->precise;   /* PGXP moves samples off 1x's too */
    const bool XBR = FILT && ta->xbr;
    const bool mc = p->mask_check;
    const u16 mo = p->mask_or;
    const u32 mo32 = p->mask_or ? A32 : 0;
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
                /* colours are kept signed and unwrapped (s64) so they can be clamped; see CCOL */
                s64 r = base[0], g = base[1], bb = base[2];
                s64 U = 0, V = 0;
                if (GOUR) {
                    r = EXACT_AT(0, xs, y);
                    g = EXACT_AT(1, xs, y);
                    bb = EXACT_AT(2, xs, y);
                }
                if (TEX && !PERSP) {   /* true (unwrapped) fixed-point values, so the clamp sees real excursions */
                    U = EXACT_AT(3, xs, y);
                    V = EXACT_AT(4, xs, y);
                }
                double pq0 = 0, pu = 0, pv = 0;
                if (PERSP) {
                    const double P = ((double)xs + 0.5) / Sx - 0.5, Q = ((double)y + 0.5) / Sy - 0.5;
                    pq0 = pq[0] + pdx[0] * (P - ax) + pdy[0] * (Q - ay);
                    pu = pq[1] + pdx[1] * (P - ax) + pdy[1] * (Q - ay);
                    pv = pq[2] + pdx[2] * (P - ax) + pdy[2] * (Q - ay);
                }
                const size_t row_off = (size_t)(y - s_oy) * s_tw + (xs - s_ox);
                u16 *dst = HP ? NULL : s_t + row_off;
                u32 *dst32 = HP ? s_t32 + row_off : NULL;
                const int y1 = y / Sy;
                int lu = -1, lv = -1;
                u16 lt = 0;
                TCache tc = TCACHE_INIT;
                u32 n_clamped = 0, n_escaped = 0;
                s64 pus = 0, pvs = 0;   /* PERSP: affine step inside the current 8-pixel run */
                for (int x = xs; x < xe; x++) {
                    u16 out = 0; u32 out32 = 0; bool blend = false, ok;
                    const int px = x - xs;
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
                    if (!(mc && (HP ? (dst32[px] & 0x80000000u) : (dst[px] & 0x8000)))) {
                        if (TEX && FILT) {
                            /* filtered: sample point without the +0.5 rounding bias, footprint
                             * clamped to the 1x-sampled range (clamp_axis) */
                            const s64 su = U - (1 << (FRAC - 1)), sv = V - (1 << (FRAC - 1));
                            int iu = (int)(su >> FRAC), fu = (int)((su >> 16) & 255);
                            int iv = (int)(sv >> FRAC), fv = (int)((sv >> 16) & 255);
                            n_clamped += (iu < umin) | (iu > umax) | (iv < vmin) | (iv > vmax) |
                                         (iu == umax && fu) | (iv == vmax && fv);
                            clamp_axis(&iu, &fu, umin, umax);
                            clamp_axis(&iv, &fv, vmin, vmax);
                            __asm__ volatile("" : "+r"(iu), "+r"(iv), "+r"(fu), "+r"(fv));
                            n_escaped += (iu < umin) | (iu > umax) | (iv < vmin) | (iv > vmax) |
                                         (iu == umax && fu) | (iv == vmax && fv);
                            Tex8 tx;
                            ok = XBR ? samp_xbr(p, DEPTH, iu, fu, iv, fv, umin, umax, vmin, vmax, Sx, Sy, &tc, &tx)
                                     : samp_bilinear(p, DEPTH, iu, fu, iv, fv, umax, vmax, &tc, &tx);
                            if (ok) {
                                if (HP) out32 = tex8_out32(tx, (int)CCOL(r, 0), (int)CCOL(g, 1), (int)CCOL(bb, 2), RAW);
                                else out = tex8_out15(tx, (int)CCOL(r, 0), (int)CCOL(g, 1), (int)CCOL(bb, 2), RAW, DITHER,
                                                      DITHER ? s_dither[y1 & 3][(x / Sx) & 3] : 0);
                                blend = SEMI && tx.stp;
                            }
                        } else if (TEX) {
                            int ui = (int)(U >> FRAC), vi = (int)(V >> FRAC);
                            if (do_clamp) {
                                n_clamped += (ui < umin) | (ui > umax) | (vi < vmin) | (vi > vmax);
                                ui = ui < umin ? umin : (ui > umax ? umax : ui);
                                vi = vi < vmin ? vmin : (vi > vmax ? vmax : vi);
                                __asm__ volatile("" : "+r"(ui), "+r"(vi));   /* check what is really fetched */
                                n_escaped += (ui < umin) | (ui > umax) | (vi < vmin) | (vi > vmax);
                            }
                            if (ui != lu || vi != lv) { lu = ui; lv = vi; lt = tex_fetch(p, DEPTH, (u32)ui, (u32)vi); }
                            if (HP) {
                                ok = lt != 0;
                                if (ok) {
                                    out32 = tex8_out32(tex8(lt), (int)CCOL(r, 0), (int)CCOL(g, 1), (int)CCOL(bb, 2), RAW);
                                    blend = SEMI && (lt & 0x8000);
                                }
                            } else
                                ok = shade_tex(p, lt, DITHER ? x / Sx : 0, y1, CCOL(r, 0), CCOL(g, 1), CCOL(bb, 2),
                                               RAW, SEMI, DITHER, &out, &blend);
                        } else if (HP) {
                            ok = true;
                            out32 = CCOL(r, 0) | (CCOL(g, 1) << 8) | (CCOL(bb, 2) << 16);
                            blend = SEMI;
                        } else {
                            ok = shade(p, DITHER ? x / Sx : 0, y1, CCOL(r, 0), CCOL(g, 1), CCOL(bb, 2), 0, 0,
                                       0, DEPTH, RAW, SEMI, DITHER, &out, &blend);
                        }
                        if (ok) {
                            if (HP) {
                                if (blend) out32 = blend32(blendm, dst32[px], out32);
                                dst32[px] = out32 | mo32;
                            } else {
                                if (blend) out = blend_px(blendm, dst[px], out);
                                dst[px] = out | mo;
                            }
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

/* Instantiations: H0 = 15-bit target, H1 = full precision (never dithered). Nearest texturing is
 * specialised on every flag; filtered texturing (one generic variant per format) is heavier per
 * pixel anyway. */
#define TRI_FN(name, TEX, DEPTH, RAW, SEMI, GOUR, DITHER)                                  \
    static void name##_h0(const void *ctx, int y0, int y1)                                 \
    { tri_impl((const TriArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI, GOUR, DITHER, 0, 0); } \
    static void name##_h1(const void *ctx, int y0, int y1)                                 \
    { tri_impl((const TriArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI, GOUR, 0, 0, 1); }

TRI_FN(tri_u000, 0, 0, 0, 0, 0, 0)
TRI_FN(tri_u001, 0, 0, 0, 0, 0, 1)
TRI_FN(tri_u010, 0, 0, 0, 0, 1, 0)
TRI_FN(tri_u011, 0, 0, 0, 0, 1, 1)
TRI_FN(tri_u100, 0, 0, 0, 1, 0, 0)
TRI_FN(tri_u101, 0, 0, 0, 1, 0, 1)
TRI_FN(tri_u110, 0, 0, 0, 1, 1, 0)
TRI_FN(tri_u111, 0, 0, 0, 1, 1, 1)
#define TRI_UNTEX_TABLE(H)                                                                 \
    { tri_u000_##H, tri_u001_##H, tri_u010_##H, tri_u011_##H, tri_u100_##H, tri_u101_##H,  \
      tri_u110_##H, tri_u111_##H }
static const BandFn s_tri_untex[2][8] = { TRI_UNTEX_TABLE(h0), TRI_UNTEX_TABLE(h1) };

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
#define TRI_TEX_TABLE(D, H)                                                                \
    { tri_t##D##_raw0_##H, tri_t##D##_raw1_##H, tri_t##D##_m000_##H, tri_t##D##_m001_##H,  \
      tri_t##D##_m010_##H, tri_t##D##_m011_##H, tri_t##D##_m100_##H, tri_t##D##_m101_##H,  \
      tri_t##D##_m110_##H, tri_t##D##_m111_##H }
static const BandFn s_tri_tex[2][3][10] = {
    { TRI_TEX_TABLE(0, h0), TRI_TEX_TABLE(1, h0), TRI_TEX_TABLE(2, h0) },
    { TRI_TEX_TABLE(0, h1), TRI_TEX_TABLE(1, h1), TRI_TEX_TABLE(2, h1) },
};
static void tri_filt_h0(const void *ctx, int y0, int y1)
{
    const TriArgs *ta = ctx;
    tri_impl(ta, y0, y1, 1, ta->p.depth, ta->p.raw, ta->p.semi, ta->p.gour, ta->p.dither, 1, 0);
}
static void tri_filt_h1(const void *ctx, int y0, int y1)
{
    const TriArgs *ta = ctx;
    tri_impl(ta, y0, y1, 1, ta->p.depth, ta->p.raw, ta->p.semi, ta->p.gour, 0, 1, 1);
}

/* Guard band: a draw area that reaches the region's left / right edge is extended gx columns
 * further out on that side (see HiresBackend.set_guard). No-op without a guard band. */
static inline void guard_clip(HiresPrim *p)
{
    if (!s_gx || p->clip_x2 < p->clip_x1) return;
    if (p->clip_x1 == s_rx) p->clip_x1 = s_rx - s_gx;
    if (p->clip_x2 == s_rx + s_rw - 1) p->clip_x2 = s_rx + s_rw - 1 + s_gx;
}

/* full-precision targets are never dithered (dither only exists to hide the 15-bit truncation) */
static inline bool dither_on(const HiresPrim *p)
{
    return p->dither && !s_t32 && ((s_Sx == 1 && s_Sy == 1) || s_dither_hi);
}

/* A 2D triangle for the texture filter (HIRES_TEX_XBR): no PGXP depth (so integer vertices) and
 * u/v mapped 1:1 to x/y, axis-aligned (flips allowed): the 1x GPU's u/v gradients are exactly
 * +-1 and 0. Sprites, HUD and text drawn as polygons. Shared by both backends. */
bool hires_tri_is_2d(const HiresVtx v[3])
{
    if (v[0].w > 0 || v[1].w > 0 || v[2].w > 0) return false;
    const HiresVtx *a = &v[0], *b = &v[1], *c = &v[2], *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    if (b->y < a->y) { t = a; a = b; b = t; }
    const s64 denom = ((s64)b->x - a->x) * ((s64)c->y - a->y) - ((s64)c->x - a->x) * ((s64)b->y - a->y);
    if (denom == 0) return false;
    s64 q[2][2];   /* [u/v][d/dx, d/dy], 12-bit fixed point like the 1x GPU */
    for (int i = 0; i < 2; i++) {
        const s32 av = (s32)(i ? a->v : a->u), bv = (s32)(i ? b->v : b->u), cv = (s32)(i ? c->v : c->u);
        s64 n_x = (s64)(bv - av) * ((s64)c->y - a->y) - (s64)(cv - av) * ((s64)b->y - a->y);
        s64 n_y = (s64)(cv - av) * ((s64)b->x - a->x) - (s64)(bv - av) * ((s64)c->x - a->x);
        q[i][0] = (s64)(((__int128)n_x << 28) / denom);
        q[i][1] = (s64)(((__int128)n_y << 28) / denom);
    }
    const s64 one = (s64)1 << 12;
    return (q[0][0] == one || q[0][0] == -one) && q[0][1] == 0 && q[1][0] == 0 && (q[1][1] == one || q[1][1] == -one);
}

static void be_tri(const HiresPrim *p, const HiresVtx v[3])
{
    if (!HAVE_T) return;
    const int H = s_t32 ? 1 : 0;
    TriArgs ta;
    ta.p = *p;
    guard_clip(&ta.p);
    ta.p.dither = dither_on(p);
    memcpy(ta.v, v, sizeof ta.v);
    ta.precise = v[0].w > 0 || v[1].w > 0 || v[2].w > 0;
    ta.xbr = p->tex && s_tex_filter == HIRES_TEX_XBR && hires_tri_is_2d(v);
    if (p->tex && s_tex_filter != HIRES_TEX_NEAREST) {
        atomic_fetch_add_explicit(&s_filt_tris, 1, memory_order_relaxed);
        if (ta.xbr) atomic_fetch_add_explicit(&s_filt_2d, 1, memory_order_relaxed);
    }
    /* equal depth on all three vertices: perspective-correct = affine, so take the affine path
     * (exact rationals, the same in hires_gpu.c). The perspective path's double/fixed-point
     * planes made exact texel-boundary ties (constant q, e.g. walls facing the camera) round
     * differently in the two backends: whole rows one texel apart in the widescreen guard band. */
    ta.persp = s_persp && p->tex && v[0].w > 0 && v[1].w > 0 && v[2].w > 0 &&
               !(v[0].w == v[1].w && v[1].w == v[2].w);
    atomic_fetch_add_explicit(&s_tris_all, 1, memory_order_relaxed);
    if (ta.persp) atomic_fetch_add_explicit(&s_tris_persp, 1, memory_order_relaxed);
    BandFn fn;
    if (p->tex) {
        atomic_fetch_add_explicit(&s_uv_tris, 1, memory_order_relaxed);
        if (p->has_uvr) atomic_fetch_add_explicit(&s_uv_tris_1x, 1, memory_order_relaxed);
    }
    if (!p->tex) {
        int idx = (p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (ta.p.dither ? 1 : 0);
        fn = s_tri_untex[H][idx];
    } else {
        note_tex_page(p);
        int d = p->depth;
        if (s_tex_filter != HIRES_TEX_NEAREST) fn = H ? tri_filt_h1 : tri_filt_h0;
        else if (p->raw) fn = s_tri_tex[H][d][p->semi ? 1 : 0];
        else fn = s_tri_tex[H][d][2 + ((p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (ta.p.dither ? 1 : 0))];
    }
    /* hi-res row / column extent of the triangle, intersected with clip and target */
    s32 miny = v[0].y, maxy = v[0].y, minx = v[0].x, maxx = v[0].x;
    for (int i = 1; i < 3; i++) {
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
    }
    const HiresPrim *cp = &ta.p;
    int ylo = imax(imax((int)(((s64)miny * s_Sy) >> 16), cp->clip_y1 * s_Sy), s_oy);
    int yhi = imin(imin((int)((((s64)maxy * s_Sy) >> 16) + 1), (cp->clip_y2 + 1) * s_Sy), s_oy + s_th);
    int xlo = imax(imax((int)(((s64)minx * s_Sx) >> 16), cp->clip_x1 * s_Sx), s_ox);
    int xhi = imin(imin((int)((((s64)maxx * s_Sx) >> 16) + 1), (cp->clip_x2 + 1) * s_Sx), s_ox + s_tw);
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
    /* filtered: unwrapped u/v range of the columns / rows drawn (the 1x-sampled range) */
    int ulo, uhi, vlo, vhi;
    bool xbr;
} RectArgs;

/* clip a rect to the draw area, region and band (1x rows / columns); false if nothing is left */
static inline bool rect_span(const RectArgs *ra, int by0, int by1, int *xs_, int *xe_, int *ys_, int *ye_)
{
    const HiresPrim *p = &ra->p;
    const int Sy = s_Sy;
    int xs = ra->x0, xe = ra->x0 + ra->w, ys = ra->y0, ye = ra->y0 + ra->h;
    if (xs < p->clip_x1) xs = p->clip_x1;
    if (xe > p->clip_x2 + 1) xe = p->clip_x2 + 1;
    if (ys < p->clip_y1) ys = p->clip_y1;
    if (ye > p->clip_y2 + 1) ye = p->clip_y2 + 1;
    xs = imax(xs, s_rx - s_gx); xe = imin(xe, s_rx + s_rw + s_gx);
    ys = imax(ys, s_ry); ye = imin(ye, s_ry + s_rh);
    /* restrict to the band (hi-res rows [by0, by1)) */
    ys = imax(ys, by0 / Sy); ye = imin(ye, (by1 + Sy - 1) / Sy);
    *xs_ = xs; *xe_ = xe; *ys_ = ys; *ye_ = ye;
    return xs < xe && ys < ye;
}

static inline __attribute__((always_inline)) void
rect_impl(const RectArgs *ra, int by0, int by1,
          const int TEX, const int DEPTH, const int RAW, const int SEMI, const int HP)
{
    const HiresPrim *p = &ra->p;
    const int Sx = s_Sx, Sy = s_Sy;
    const int x0 = ra->x0, y0 = ra->y0;
    int xs, xe, ys, ye;
    if (!rect_span(ra, by0, by1, &xs, &xe, &ys, &ye)) return;
    const u32 r = ra->r, g = ra->g, b = ra->b;
    u32 u0 = ra->u0;
    const u32 v0 = ra->v0;
    int du = ra->flipx ? -1 : 1, dv = ra->flipy ? -1 : 1;
    if (TEX && ra->flipx) u0 |= 1;
    u8 ucol[VRAM_W];
    for (int x = xs; x < xe; x++) ucol[x - xs] = (u8)((u0 + (u32)(du * (x - x0))) & 0xFF);
    u16 outs[VRAM_W];
    u32 outs32[HP ? VRAM_W : 1];
    u8 vis[VRAM_W];
    const int cnt = xe - xs;
    const u32 mo32 = p->mask_or ? A32 : 0;
    for (int y = ys; y < ye; y++) {
        u32 v = (v0 + (u32)(dv * (y - y0))) & 0xFF;
        for (int i = 0; i < cnt; i++) {
            bool blend = false, ok;
            if (HP) {
                u32 o = 0;
                if (TEX) {
                    u16 t = tex_fetch(p, DEPTH, ucol[i], v);
                    ok = t != 0;
                    if (ok) { o = tex8_out32(tex8(t), (int)r, (int)g, (int)b, RAW); blend = SEMI && (t & 0x8000); }
                } else { ok = true; o = r | (g << 8) | (b << 16); blend = SEMI; }
                outs32[i] = o;
            } else {
                u16 o = 0;
                ok = shade(p, 0, 0, r, g, b, ucol[i], v, TEX, DEPTH, RAW, SEMI, 0, &o, &blend);
                outs[i] = o;
            }
            vis[i] = ok ? (blend ? 2 : 1) : 0;
        }
        int sy0 = imax(0, by0 - y * Sy), sy1 = imin(Sy, by1 - y * Sy);
        for (int sy = sy0; sy < sy1; sy++) {
            const size_t off = (size_t)(y * Sy + sy - s_oy) * s_tw + (xs * Sx - s_ox);
            if (HP) {
                u32 *dst = s_t32 + off;
                for (int i = 0; i < cnt; i++) {
                    if (!vis[i]) { dst += Sx; continue; }
                    for (int sx = 0; sx < Sx; sx++, dst++) {
                        if (p->mask_check && (*dst & A32)) continue;
                        *dst = (vis[i] == 2 ? blend32(p->blend, *dst, outs32[i]) : outs32[i]) | mo32;
                    }
                }
            } else {
                u16 *dst = s_t + off;
                for (int i = 0; i < cnt; i++) {
                    if (!vis[i]) { dst += Sx; continue; }
                    for (int sx = 0; sx < Sx; sx++, dst++) store_px(p, dst, outs[i], vis[i] == 2);
                }
            }
        }
    }
}

/* Filtered textured rect: per hi-res pixel, texel coordinates from rect_axis (the sample point
 * relative to the 1x texel centres), footprint clamped to the u/v range of the drawn columns/rows. */
static inline __attribute__((always_inline)) void
rect_filt_impl(const RectArgs *ra, int by0, int by1, const int HP)
{
    const HiresPrim *p = &ra->p;
    const int Sx = s_Sx, Sy = s_Sy;
    int xs, xe, ys, ye;
    if (!rect_span(ra, by0, by1, &xs, &xe, &ys, &ye)) return;
    const int u0 = (int)(ra->u0 | (ra->flipx ? 1u : 0u)), v0 = (int)ra->v0;
    const int du = ra->flipx ? -1 : 1, dv = ra->flipy ? -1 : 1;
    const int depth = p->depth;
    const bool RAW = p->raw, SEMI = p->semi;
    const int r = (int)ra->r, g = (int)ra->g, b = (int)ra->b;
    const u32 mo32 = p->mask_or ? A32 : 0;
    const int X0 = xs * Sx, X1 = xe * Sx;
    const int Y0 = imax(ys * Sy, by0), Y1 = imin(ye * Sy, by1);
    static _Thread_local int iu_col[(VRAM_W + 2 * HIRES_MAX_GUARD) * HIRES_MAX_RENDER_SCALE];
    static _Thread_local u8 fu_col[(VRAM_W + 2 * HIRES_MAX_GUARD) * HIRES_MAX_RENDER_SCALE];
    if (Y0 >= Y1) return;
    u32 n_clamped = 0, n_escaped = 0;
    for (int X = X0; X < X1; X++) {
        int iu, fu;
        rect_axis(u0, du, ra->x0, X, Sx, &iu, &fu);
        n_clamped += (iu < ra->ulo) | (iu > ra->uhi) | (iu == ra->uhi && fu);
        clamp_axis(&iu, &fu, ra->ulo, ra->uhi);
        __asm__ volatile("" : "+r"(iu), "+r"(fu));
        n_escaped += (iu < ra->ulo) | (iu > ra->uhi) | (iu == ra->uhi && fu);
        iu_col[X - X0] = iu; fu_col[X - X0] = (u8)fu;
    }
    for (int Y = Y0; Y < Y1; Y++) {
        int iv, fv;
        rect_axis(v0, dv, ra->y0, Y, Sy, &iv, &fv);
        clamp_axis(&iv, &fv, ra->vlo, ra->vhi);
        TCache tc = TCACHE_INIT;
        const size_t off = (size_t)(Y - s_oy) * s_tw + (X0 - s_ox);
        for (int X = X0; X < X1; X++) {
            const int k = X - X0;
            if (p->mask_check && (HP ? (s_t32[off + k] & A32) : (s_t[off + k] & 0x8000))) continue;
            Tex8 tx;
            bool ok = ra->xbr ? samp_xbr(p, depth, iu_col[k], fu_col[k], iv, fv, ra->ulo, ra->uhi, ra->vlo, ra->vhi, Sx, Sy, &tc, &tx)
                              : samp_bilinear(p, depth, iu_col[k], fu_col[k], iv, fv, ra->uhi, ra->vhi, &tc, &tx);
            if (!ok) continue;
            const bool blend = SEMI && tx.stp;
            if (HP) {
                u32 o = RAW ? tex8_out32(tx, r, g, b, 1) : tex8_out32(tx, r, g, b, 0);
                u32 *d = &s_t32[off + k];
                *d = (blend ? blend32(p->blend, *d, o) : o) | mo32;
            } else {
                u16 o = RAW ? tex8_out15(tx, r, g, b, 1, 0, 0) : tex8_out15(tx, r, g, b, 0, 0, 0);
                u16 *d = &s_t[off + k];
                *d = (blend ? blend_px(p->blend, *d, o) : o) | p->mask_or;
            }
        }
    }
    tl_samples += (u64)(X1 - X0) * (u64)(Y1 - Y0);
    tl_clamped += n_clamped;
    tl_escaped += n_escaped;
}

#define RECT_FN(name, TEX, DEPTH, RAW, SEMI)                                                   \
    static void name##_h0(const void *ctx, int y0, int y1)                                     \
    { rect_impl((const RectArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI, 0); }                    \
    static void name##_h1(const void *ctx, int y0, int y1)                                     \
    { rect_impl((const RectArgs *)ctx, y0, y1, TEX, DEPTH, RAW, SEMI, 1); }
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
#define RECT_TABLE(H)                                                                     \
    { { rect_t0_00_##H, rect_t0_01_##H, rect_t0_10_##H, rect_t0_11_##H },                 \
      { rect_t1_00_##H, rect_t1_01_##H, rect_t1_10_##H, rect_t1_11_##H },                 \
      { rect_t2_00_##H, rect_t2_01_##H, rect_t2_10_##H, rect_t2_11_##H } }
static const BandFn s_rect_tex[2][3][4] = { RECT_TABLE(h0), RECT_TABLE(h1) };
static void rect_filt_h0(const void *ctx, int y0, int y1) { rect_filt_impl(ctx, y0, y1, 0); }
static void rect_filt_h1(const void *ctx, int y0, int y1) { rect_filt_impl(ctx, y0, y1, 1); }

/* Filtered rects: the unwrapped u/v range of the columns/rows drawn (rect within the draw area),
 * which is what the 1x rect samples there. Shared with hires_gpu.c. */
void hires_rect_uv_range(const HiresPrim *p, int x, int y, int w, int h, u32 u0, u32 v0, bool fx, bool fy,
                         int *ulo, int *uhi, int *vlo, int *vhi)
{
    int xs = imax(x, p->clip_x1), xe = imin(x + w, p->clip_x2 + 1);
    int ys = imax(y, p->clip_y1), ye = imin(y + h, p->clip_y2 + 1);
    if (xe <= xs) xe = xs + 1;
    if (ye <= ys) ye = ys + 1;
    const int uu0 = (int)(u0 | (fx ? 1u : 0u)), du = fx ? -1 : 1, dv = fy ? -1 : 1;
    const int ua = uu0 + du * (xs - x), ub = uu0 + du * (xe - 1 - x);
    const int va = (int)v0 + dv * (ys - y), vb = (int)v0 + dv * (ye - 1 - y);
    *ulo = imin(ua, ub); *uhi = imax(ua, ub);
    *vlo = imin(va, vb); *vhi = imax(va, vb);
}

static void be_rect(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g, u32 b,
                    u32 u0, u32 v0, bool fx, bool fy)
{
    if (!HAVE_T) return;
    const int H = s_t32 ? 1 : 0;
    RectArgs ra = { *p, x, y, w, h, r, g, b, u0, v0, fx, fy, 0, 0, 0, 0, false };
    if (s_gx) {
        /* an untextured rect over the whole draw-area width (fade, flash) spans the guard band */
        bool full = !p->tex && x <= p->clip_x1 && x + w >= p->clip_x2 + 1;
        guard_clip(&ra.p);
        if (full) { ra.x0 = x = ra.p.clip_x1; ra.w = w = ra.p.clip_x2 + 1 - x; }
    }
    p = &ra.p;
    BandFn fn;
    if (!p->tex) {
        if (H) fn = p->semi ? rect_u1_h1 : rect_u0_h1;
        else fn = p->semi ? rect_u1_h0 : rect_u0_h0;
    } else if (s_tex_filter != HIRES_TEX_NEAREST) {
        note_tex_page(p);
        hires_rect_uv_range(p, x, y, w, h, u0, v0, fx, fy, &ra.ulo, &ra.uhi, &ra.vlo, &ra.vhi);
        ra.xbr = s_tex_filter == HIRES_TEX_XBR;
        atomic_fetch_add_explicit(&s_filt_rects, 1, memory_order_relaxed);
        fn = H ? rect_filt_h1 : rect_filt_h0;
    } else {
        note_tex_page(p);
        fn = s_rect_tex[H][p->depth][(p->raw ? 2 : 0) | (p->semi ? 1 : 0)];
    }
    int ys = imax(imax(y, p->clip_y1), s_ry), ye = imin(imin(y + h, p->clip_y2 + 1), s_ry + s_rh);
    int xs = imax(imax(x, p->clip_x1), s_rx - s_gx), xe = imin(imin(x + w, p->clip_x2 + 1), s_rx + s_rw + s_gx);
    if (ys >= ye || xs >= xe) return;
    mark_dirty(ys, ye - 1);
    int ylo = ys * s_Sy, yhi = ye * s_Sy;
    if ((s64)(yhi - ylo) * (xe - xs) * s_Sx >= s_par_threshold) par_rows(fn, &ra, ylo, yhi);
    else { fn(&ra, ylo, yhi); flush_counters(); }
}

static inline u32 lclamp(s32 v, u32 c0, u32 c1, int sh)
{
    const s32 a = (s32)((c0 >> sh) & 0xFF), b = (s32)((c1 >> sh) & 0xFF);
    const s32 lo = a < b ? a : b, hi = a < b ? b : a;
    return (u32)(v < lo ? lo : (v > hi ? hi : v));
}

/* ---- lines ----------------------------------------------------------------- */

static void be_line(const HiresPrim *p0, int x0, int y0, u32 c0, int x1, int y1, u32 c1)
{
    if (!HAVE_T) return;
    HiresPrim pl = *p0;
    guard_clip(&pl);
    const HiresPrim *p = &pl;
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
            const size_t i_ = (size_t)(py_ - s_oy) * s_tw + (px_ - s_ox);                       \
            if (s_t32) {                                                                        \
                u32 *d_ = s_t32 + i_;                                                           \
                if (!(p->mask_check && (*d_ & A32))) {                                          \
                    u32 c_ = (u32)(CR) | ((u32)(CG) << 8) | ((u32)(CB) << 16);                  \
                    if (p->semi) c_ = blend32(p->blend, *d_, c_);                               \
                    *d_ = c_ | (p->mask_or ? A32 : 0);                                          \
                }                                                                               \
            } else {                                                                            \
                u16 *d_ = s_t + i_;                                                             \
                if (!(p->mask_check && (*d_ & 0x8000)) &&                                       \
                    shade(p, px_ / Sx, py_ / Sy, (CR), (CG), (CB), 0, 0, 0, 0, 0, p->semi, dith,\
                          &o_, &bl_)) {                                                         \
                    if (bl_) o_ = blend_px(p->blend, *d_, o_);                                  \
                    *d_ = o_ | p->mask_or;                                                      \
                }                                                                               \
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
        /* the Sa - 1 steps past the end extrapolate the gradient: keep it between the end
         * colours (no-op at 1x1, where the line stops at the end; it used to wrap below 0) */
        u32 cr = lclamp(r >> 12, c0, c1, 0), cg = lclamp(g >> 12, c0, c1, 8), cb = lclamp(b >> 12, c0, c1, 16);
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
    const size_t off = (size_t)(y * s_Sy - s_oy) * s_tw + (x0 * s_Sx - s_ox);
    const int n = (x1 - x0) * s_Sx;
    if (s_t32) {
        u32 *row0 = s_t32 + off;
        const u32 c32 = c15to32(c);
        for (int i = 0; i < n; i++) row0[i] = c32;
        for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)n * sizeof(u32));
        return;
    }
    u16 *row0 = s_t + off;
    for (int i = 0; i < n; i++) row0[i] = c;
    for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)n * sizeof(u16));
}

static void be_fill(int x, int y, int w, int h, u16 c)
{
    if (!HAVE_T) return;
    mark_rows_wrapped(y, h);
    for (int j = 0; j < h; j++) {
        int row = (y + j) & 511;
        if (row < s_ry || row >= s_ry + s_rh) continue;
        int xa = x, xb = x + w;
        if (xb <= VRAM_W) {
            int a = imax(xa, s_rx), b = imin(xb, s_rx + s_rw);
            /* a row cleared across the whole region also clears the guard band */
            if (s_gx && xa <= s_rx && xb >= s_rx + s_rw) { a -= s_gx; b += s_gx; }
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

/* is target pixel i (either format) mask-bit protected? */
static inline bool tmasked(size_t i) { return s_t32 ? (s_t32[i] & A32) != 0 : (s_t[i] & 0x8000) != 0; }

static void be_put(int x, int y, u16 v, bool mc, bool ms)
{
    if (!HAVE_T) return;
    x &= 1023; y &= 511;
    if (!in_region(x, y)) return;
    mark_dirty(y, y);
    const u16 val = v | (ms ? 0x8000 : 0);
    const u32 val32 = c15to32(val);
    for (int sy = 0; sy < s_Sy; sy++) {
        const size_t off = (size_t)(y * s_Sy + sy - s_oy) * s_tw + (x * s_Sx - s_ox);
        for (int sx = 0; sx < s_Sx; sx++) {
            if (mc && tmasked(off + sx)) continue;
            if (s_t32) s_t32[off + sx] = val32;
            else s_t[off + sx] = val;
        }
    }
}

static void be_copy(int sx, int sy, int dx, int dy, int wd, int ht, bool mc, bool ms)
{
    if (!HAVE_T) return;
    const int Sx = s_Sx, Sy = s_Sy;
    /* skip when the destination can't touch the region at all */
    bool any = false;
    for (int j = 0; j < ht && !any; j++) {
        int drow = (dy + j) & 511;
        if (drow >= s_ry && drow < s_ry + s_rh) any = true;
    }
    if (!any) return;
    mark_rows_wrapped(dy, ht);
    /* one 1x row of the source, at the target scale; u32 = either format's pixel */
    static u32 buf[(size_t)HIRES_MAX_RENDER_SCALE * VRAM_W * HIRES_MAX_RENDER_SCALE];
    const size_t stride = (size_t)wd * Sx;
    const bool hp = s_t32 != NULL;
    for (int j = 0; j < ht; j++) {
        int srow = (sy + j) & 511, drow = (dy + j) & 511;
        if (drow < s_ry || drow >= s_ry + s_rh) continue;
        for (int i = 0; i < wd; i++) {
            int scol = (sx + i) & 1023;
            bool in = in_region(scol, srow);
            for (int a = 0; a < Sy; a++)
                for (int b = 0; b < Sx; b++) {
                    const size_t si = (size_t)(srow * Sy + a - s_oy) * s_tw + (scol * Sx + b - s_ox);
                    const u16 vv = s_vram[srow * VRAM_W + scol];
                    buf[a * stride + (size_t)i * Sx + b] = in ? (hp ? s_t32[si] : s_t[si]) : (hp ? c15to32(vv) : vv);
                }
        }
        for (int i = 0; i < wd; i++) {
            int dcol = (dx + i) & 1023;
            if (!in_region(dcol, drow)) continue;
            for (int a = 0; a < Sy; a++) {
                const size_t di = (size_t)(drow * Sy + a - s_oy) * s_tw + (dcol * Sx - s_ox);
                for (int b = 0; b < Sx; b++) {
                    if (mc && tmasked(di + b)) continue;
                    const u32 val = buf[a * stride + (size_t)i * Sx + b];
                    if (hp) s_t32[di + b] = val | (ms ? A32 : 0);
                    else s_t[di + b] = (u16)val | (ms ? 0x8000 : 0);
                }
            }
        }
    }
}

static const u16 *s_seed_src;   /* the picture for the configure() in progress (hires_seed_vram) */
const u16 *hires_seed_vram(const u16 *vram1x) { return s_seed_src ? s_seed_src : vram1x; }

static void seed(void)
{
    const u16 *pic = hires_seed_vram(s_vram);
    for (int y = 0; y < s_rh; y++) {
        const u16 *src = &pic[(s_ry + y) * VRAM_W + s_rx];
        if (s_t32) {
            u32 *row0 = s_t32 + (size_t)y * s_Sy * s_tw;
            memset(row0, 0, (size_t)s_tw * sizeof(u32));   /* the guard band starts black */
            u32 *real = row0 + (size_t)s_gx * s_Sx;
            for (int x = 0; x < s_rw; x++) {
                const u32 c = c15to32(src[x]);
                for (int i = 0; i < s_Sx; i++) real[x * s_Sx + i] = c;
            }
            for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)s_tw * sizeof(u32));
            continue;
        }
        u16 *row0 = s_t + (size_t)y * s_Sy * s_tw;
        memset(row0, 0, (size_t)s_tw * sizeof(u16));   /* the guard band starts black */
        u16 *real = row0 + (size_t)s_gx * s_Sx;
        for (int x = 0; x < s_rw; x++)
            for (int i = 0; i < s_Sx; i++) real[x * s_Sx + i] = src[x];
        for (int j = 1; j < s_Sy; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)s_tw * sizeof(u16));
    }
}

static void be_destroy(void)
{
    free(s_t);
    free(s_t32);
    s_t = NULL;
    s_t32 = NULL;
    s_Sx = s_Sy = 0;
    s_gx = 0; g_hires_guard = 0;
}

static bool be_configure(const u16 *vram1x, int sx, int sy, int rx, int ry, int rw, int rh)
{
    if (sx < 1 || sx > HIRES_MAX_RENDER_SCALE || sy < 1 || sy > HIRES_MAX_RENDER_SCALE || rw <= 0 || rh <= 0 ||
        rx < 0 || ry < 0 || rx + rw > VRAM_W || ry + rh > VRAM_H)
        return false;
    const int gx = s_pend_gx;
    const bool hp = hires_target_fmt() == HIRES_FMT_RGBA8;
    const size_t npx = (size_t)(rw + 2 * gx) * sx * rh * sy;
    void *nt = malloc(npx * (hp ? sizeof(u32) : sizeof(u16)));
    if (!nt) return false;
    free(s_t);
    free(s_t32);
    s_t = hp ? NULL : nt;
    s_t32 = hp ? nt : NULL;
    s_vram = vram1x;
    s_Sx = sx; s_Sy = sy;
    s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
    s_gx = gx;
    g_hires_guard = gx;
    s_tw = (rw + 2 * gx) * sx; s_th = rh * sy;
    s_ox = (rx - gx) * sx; s_oy = ry * sy;
    memset(s_fb_tex_seen, 0, sizeof s_fb_tex_seen);
    seed();
    s_epoch++;
    mark_dirty(0, VRAM_H);
    return true;
}

static bool be_view(int x, int y, int w, int h, HiresView *out)
{
    if (!HAVE_T || w <= 0 || h <= 0) return false;
    if (x < s_rx - s_gx || y < s_ry || x + w > s_rx + s_rw + s_gx || y + h > s_ry + s_rh) return false;
    const size_t off = (size_t)(y * s_Sy - s_oy) * s_tw + (x * s_Sx - s_ox);
    out->pixels = s_t ? s_t + off : NULL;
    out->px32 = s_t32 ? s_t32 + off : NULL;
    out->fmt = s_t32 ? HIRES_FMT_RGBA8 : HIRES_FMT_15;
    out->stride = s_tw;
    out->w = w * s_Sx;
    out->h = h * s_Sy;
    out->scale_x = s_Sx;
    out->scale_y = s_Sy;
    out->ssaa = 1;
    out->gpu = NULL;
    out->gx = x * s_Sx - s_ox;
    out->gy = y * s_Sy - s_oy;
    return true;
}

static void be_set_guard(int gx) { s_pend_gx = gx < 0 ? 0 : (gx > HIRES_MAX_GUARD ? HIRES_MAX_GUARD : gx); }

static const HiresBackend s_soft = {
    "software", be_configure, be_tri, be_rect, be_line, be_fill, be_copy, be_put, be_view, be_view, be_destroy,
    be_set_guard,
};

/* ---- control --------------------------------------------------------------------- */

static const HiresBackend *s_be = &s_soft;   /* primary: presented, viewed, verified */
static const HiresBackend *s_shadow;         /* compare mode: fed the same commands */
static const HiresBackend *s_want_be = &s_soft, *s_want_shadow;
static int s_base_sx, s_base_sy;             /* requested (base) scale of the allocated target */
static int s_cur_sx, s_cur_sy;               /* render scale (base x ssaa) of the allocated target(s); 0 = off */
static int s_cur_gx;                         /* guard band of the allocated target(s) */
static int s_cur_fmt, s_cur_ssaa = 1;        /* format / supersampling of the allocated target(s) */
static int s_cfg_ssaa = 1;                   /* hires_set_ssaa() */
static int s_cfg_ssaa_cap;                   /* hires_set_ssaa_cap(): max render Sx*Sy, 0 = none */
static bool s_cfg_full;                      /* hires_set_colour() */
static int s_pend_fmt;                       /* format for the configure() in progress */

/* tee: forwards every command to the primary and the shadow backend */
static bool tee_configure(const u16 *v, int sx, int sy, int rx, int ry, int rw, int rh)
{ return s_be->configure(v, sx, sy, rx, ry, rw, rh) && s_shadow->configure(v, sx, sy, rx, ry, rw, rh); }
static void tee_tri(const HiresPrim *p, const HiresVtx v[3]) { s_be->tri(p, v); s_shadow->tri(p, v); }
static void tee_rect(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g, u32 b,
                     u32 u0, u32 v0, bool fx, bool fy)
{ s_be->rect(p, x, y, w, h, r, g, b, u0, v0, fx, fy); s_shadow->rect(p, x, y, w, h, r, g, b, u0, v0, fx, fy); }
static void tee_line(const HiresPrim *p, int x0, int y0, u32 c0, int x1, int y1, u32 c1)
{ s_be->line(p, x0, y0, c0, x1, y1, c1); s_shadow->line(p, x0, y0, c0, x1, y1, c1); }
static void tee_fill(int x, int y, int w, int h, u16 c) { s_be->fill(x, y, w, h, c); s_shadow->fill(x, y, w, h, c); }
static void tee_copy(int sx, int sy, int dx, int dy, int w, int h, bool mc, bool ms)
{ s_be->copy(sx, sy, dx, dy, w, h, mc, ms); s_shadow->copy(sx, sy, dx, dy, w, h, mc, ms); }
static void tee_put(int x, int y, u16 v, bool mc, bool ms) { s_be->put(x, y, v, mc, ms); s_shadow->put(x, y, v, mc, ms); }
static bool tee_view(int x, int y, int w, int h, HiresView *o)
{
    HiresView sv;
    s_shadow->view(x, y, w, h, &sv);   /* lets the shadow flush its frame */
    return s_be->view(x, y, w, h, o);
}
static bool tee_read(int x, int y, int w, int h, HiresView *o) { return s_be->read(x, y, w, h, o); }
static void tee_destroy(void) { s_be->destroy(); s_shadow->destroy(); }
static const HiresBackend s_tee = {
    "tee", tee_configure, tee_tri, tee_rect, tee_line, tee_fill, tee_copy, tee_put, tee_view, tee_read, tee_destroy,
};

void hires_set_backend(const HiresBackend *be, const HiresBackend *shadow)
{
    s_want_be = be ? be : &s_soft;
    s_want_shadow = shadow;
}

const HiresBackend *hires_software_backend(void) { return &s_soft; }

/* ---- render-quality settings ---- */

static const char *const s_filter_names[] = { "nearest", "bilinear", "xbr" };
void hires_set_tex_filter(int mode) { s_tex_filter = mode < 0 || mode > HIRES_TEX_XBR ? HIRES_TEX_NEAREST : mode; }
int hires_get_tex_filter(void) { return s_tex_filter; }
const char *hires_tex_filter_name(int mode) { return s_filter_names[mode < 0 || mode > 2 ? 0 : mode]; }
int hires_tex_filter_from_name(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (!strcmp(s, s_filter_names[i])) return i;
    return -1;
}
void hires_set_colour(bool full) { s_cfg_full = full; }
bool hires_get_colour(void) { return s_cfg_full; }
void hires_set_ssaa(int n) { s_cfg_ssaa = n < 1 ? 1 : (n > HIRES_MAX_SSAA ? HIRES_MAX_SSAA : n); }
int hires_get_ssaa(void) { return s_cfg_ssaa; }
void hires_set_ssaa_cap(int cap) { s_cfg_ssaa_cap = cap > 0 ? cap : 0; }
int hires_ssaa(void) { return g_hires ? s_cur_ssaa : 1; }
int hires_target_fmt(void) { return s_pend_fmt; }
bool hires_get_render_scale(int *sx, int *sy)
{
    if (sx) *sx = g_hires ? s_cur_sx : 0;
    if (sy) *sy = g_hires ? s_cur_sy : 0;
    return g_hires != NULL;
}

/* Largest supersampling factor <= the requested one that keeps the render target within the
 * limits (see hires.h); the region and guard band are the ones about to be configured. */
static int ssaa_for(int sx, int sy, int gx)
{
    int n = s_cfg_ssaa;
    for (; n > 1; n--) {
        const s64 w = (s64)(s_cfg_rw + 2 * gx) * sx * n, h = (s64)s_cfg_rh * sy * n;
        if (sx * n <= HIRES_MAX_RENDER_SCALE && sy * n <= HIRES_MAX_RENDER_SCALE && w <= HIRES_MAX_RENDER_DIM &&
            h <= HIRES_MAX_RENDER_DIM && w * h <= HIRES_MAX_RENDER_PX &&
            (!s_cfg_ssaa_cap || sx * n * sy * n <= s_cfg_ssaa_cap))
            break;
    }
    return n;
}

static void off_state(void)
{
    g_hires = NULL;
    s_cur_sx = s_cur_sy = 0;
    s_base_sx = s_base_sy = 0;
    s_cur_gx = 0; g_hires_guard = 0;
    s_cur_ssaa = 1;
}

/* The picture to seed a new target with. Without the display copy (gpu.c skip rule) the 1x VRAM
 * has no drawn pixels in the framebuffer region, so a target reallocated between frames (resize,
 * resolution / renderer / SSAA / colour change) would show a black screen until the game redrew
 * the displayed buffer (4 vblanks in a fight). keep_picture() samples the old target at each 1x
 * pixel's centre into a copy of the 1x VRAM: the same 1x picture the seed used to have. Valid only
 * while no primitive has been sent since (the same moment between frames). */
static u16 *s_seed;
static u64 s_seed_stamp;
static void keep_picture(void)
{
    free(s_seed);
    s_seed = NULL;
    if (!g_hires || gpu_display_copy() || !s_be || !s_be->read) return;
    HiresView v;
    if (!s_be->read(s_rx, s_ry, s_rw, s_rh, &v) || (!v.pixels && !v.px32)) return;
    u16 *t = malloc((size_t)1024 * 512 * sizeof(u16));
    if (!t) return;
    memcpy(t, gpu_vram(), (size_t)1024 * 512 * sizeof(u16));
    const int cx = v.scale_x / 2, cy = v.scale_y / 2;
    for (int y = 0; y < s_rh; y++) {
        const size_t row = (size_t)(y * v.scale_y + cy) * v.stride;
        u16 *d = t + (size_t)((s_ry + y) & 511) * 1024;
        for (int x = 0; x < s_rw; x++) {
            const size_t i = row + (size_t)(x * v.scale_x + cx);
            u16 c;
            if (v.pixels) c = v.pixels[i];
            else {
                const u32 p = v.px32[i];
                c = (u16)(((p & 0xFF) >> 3) | (((p >> 8) & 0xFF) >> 3 << 5) | (((p >> 16) & 0xFF) >> 3 << 10) |
                          ((p >> 24) ? 0x8000 : 0));
            }
            d[(s_rx + x) & 1023] = c;
        }
    }
    s_seed = t;
    s_seed_stamp = gpu_prim_count();
}
static const u16 *seed_vram(void)
{
    return s_seed && s_seed_stamp == gpu_prim_count() ? s_seed : gpu_display_vram();
}
static void drop_seed(void) { free(s_seed); s_seed = NULL; }

bool hires_set_scale(int sx, int sy)
{
    if (sx <= 0 || sy <= 0) sx = sy = 0;
    if (sx > HIRES_MAX_SCALE) sx = HIRES_MAX_SCALE;
    if (sy > HIRES_MAX_SCALE) sy = HIRES_MAX_SCALE;
    if (sx == 0) {
        if (g_hires) { keep_picture(); g_hires->destroy(); }
        off_state();
        return true;
    }
    const int fmt = s_cfg_full ? HIRES_FMT_RGBA8 : HIRES_FMT_15;
    /* guard band: only if every backend in use supports it (else widescreen stays 4:3) */
    int gx = s_cfg_gx;
    if (!s_want_be->set_guard || (s_want_shadow && !s_want_shadow->set_guard)) gx = 0;
    const int n = ssaa_for(sx, sy, gx);
    if (g_hires && s_base_sx == sx && s_base_sy == sy && s_rx == s_cfg_rx && s_ry == s_cfg_ry &&
        s_rw == s_cfg_rw && s_rh == s_cfg_rh && s_be == s_want_be && s_shadow == s_want_shadow &&
        s_cur_gx == gx && s_cur_fmt == fmt && s_cur_ssaa == n)
        return true;
    if (g_hires) keep_picture();
    if (g_hires && (s_be != s_want_be || s_shadow != s_want_shadow)) { g_hires->destroy(); g_hires = NULL; }
    s_be = s_want_be;
    s_shadow = s_want_shadow;
    const HiresBackend *be = s_shadow ? &s_tee : s_be;
    /* the software backend's region statics double as the control layer's record of it */
    s_rx = s_cfg_rx; s_ry = s_cfg_ry; s_rw = s_cfg_rw; s_rh = s_cfg_rh;
    if (s_be->set_guard) s_be->set_guard(gx);
    if (s_shadow && s_shadow->set_guard) s_shadow->set_guard(gx);
    s_pend_fmt = fmt;
    s_seed_src = seed_vram();   /* textures keep coming from the emulated VRAM; this is the picture only */
    const bool ok = be->configure(gpu_vram(), sx * n, sy * n, s_cfg_rx, s_cfg_ry, s_cfg_rw, s_cfg_rh);
    s_seed_src = NULL;
    drop_seed();
    if (!ok) {
        off_state();
        return false;
    }
    s_base_sx = sx; s_base_sy = sy;
    s_cur_sx = sx * n; s_cur_sy = sy * n;
    s_cur_gx = gx; g_hires_guard = gx;
    s_cur_fmt = fmt;
    s_cur_ssaa = n;
    g_hires = be;
    return true;
}

bool hires_reconfigure(void)
{
    if (!g_hires) return true;
    return hires_set_scale(s_base_sx, s_base_sy);
}

/* Re-seed the target from the current 1x VRAM (after a save state is loaded). */
void hires_reseed(void)
{
    if (!g_hires) return;
    const HiresBackend *be = g_hires;
    s_pend_fmt = s_cur_fmt;
    s_seed_src = gpu_display_vram();
    const bool ok = be->configure(gpu_vram(), s_cur_sx, s_cur_sy, s_rx, s_ry, s_rw, s_rh);
    s_seed_src = NULL;
    if (!ok) off_state();
}

bool hires_get_scale(int *sx, int *sy)
{
    if (sx) *sx = g_hires ? s_base_sx : 0;
    if (sy) *sy = g_hires ? s_base_sy : 0;
    return g_hires != NULL;
}

void hires_set_region(int rx, int ry, int rw, int rh)
{
    s_cfg_rx = rx; s_cfg_ry = ry; s_cfg_rw = rw; s_cfg_rh = rh;
}

void hires_set_dither(bool enable) { s_dither_hi = enable; }
bool hires_get_dither(void) { return s_dither_hi; }

void hires_set_guard(int gx) { s_cfg_gx = gx < 0 ? 0 : (gx > HIRES_MAX_GUARD ? HIRES_MAX_GUARD : gx); }
int hires_guard(void) { return g_hires ? s_cur_gx : 0; }

static bool fin_view(bool ok, HiresView *v)
{
    if (ok) v->ssaa = s_cur_ssaa;
    return ok;
}

bool hires_get_view(const GpuDisplay *d, HiresView *v)
{
    if (!g_hires || !d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) return false;
    return fin_view(g_hires->view(d->x, d->y, d->w, d->h, v), v);
}

bool hires_get_view_wide(const GpuDisplay *d, int extra, HiresView *v)
{
    if (!g_hires || !d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) return false;
    extra = imax(0, imin(extra, s_cur_gx));
    return fin_view(g_hires->view(d->x - extra, d->y, d->w + 2 * extra, d->h, v), v);
}

bool hires_read_view(const GpuDisplay *d, HiresView *v)
{
    if (!g_hires || !d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) return false;
    return fin_view(g_hires->read(d->x, d->y, d->w, d->h, v), v);
}

bool hires_read_view_wide(const GpuDisplay *d, int extra, HiresView *v)
{
    if (!g_hires || !d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) return false;
    extra = imax(0, imin(extra, s_cur_gx));
    return fin_view(g_hires->read(d->x - extra, d->y, d->w + 2 * extra, d->h, v), v);
}

/* 8-bit channels of a target pixel of either format (15-bit c -> c << 3) */
static inline u32 px8(const HiresView *v, size_t i)
{
    if (v->fmt == HIRES_FMT_RGBA8) return v->px32[i] & 0xFFFFFF;
    const u32 p = v->pixels[i];
    return ((p & 0x7c00) << 9) | ((p & 0x03e0) << 6) | ((p & 0x001f) << 3);
}

void hires_view_to_rgba(const HiresView *v, u32 *dst, size_t dst_pitch_px, int y0, int y1)
{
    const int n = v->ssaa > 1 ? v->ssaa : 1;
    const int ow = v->w / n, oh = v->h / n;
    if (y1 > oh) y1 = oh;
    for (int y = y0; y < y1; y++) {
        u32 *d = dst + (size_t)y * dst_pitch_px;
        if (n == 1 && v->fmt == HIRES_FMT_15) {
            const u16 *src = v->pixels + (size_t)y * v->stride;
            for (int x = 0; x < ow; x++) {   /* plain arithmetic: auto-vectorizes */
                u32 p = src[x];
                d[x] = 0xff000000u | ((p & 0x7c00) << 9) | ((p & 0x03e0) << 6) | ((p & 0x001f) << 3);
            }
            continue;
        }
        if (n == 1) {
            const u32 *src = v->px32 + (size_t)y * v->stride;
            for (int x = 0; x < ow; x++) d[x] = src[x] | 0xff000000u;
            continue;
        }
        /* box average of n x n target pixels, rounded: the same in hires_gpu.c's present pass */
        const u32 nn = (u32)(n * n), half = nn / 2;
        for (int x = 0; x < ow; x++) {
            u32 sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < n; j++) {
                const size_t row = (size_t)(y * n + j) * v->stride + (size_t)x * n;
                for (int i = 0; i < n; i++) {
                    const u32 p = px8(v, row + i);
                    sr += p & 255; sg += (p >> 8) & 255; sb += (p >> 16) & 255;
                }
            }
            d[x] = 0xff000000u | ((sr + half) / nn) | (((sg + half) / nn) << 8) | (((sb + half) / nn) << 16);
        }
    }
}

void hires_pick_scale(int pw, int ph, int dw, int dh, int max_x, int max_y, int *sx, int *sy)
{
    hires_pick_scale_ar(pw, ph, dw, dh, 4.0 / 3.0, max_x, max_y, sx, sy);
}

void hires_pick_scale_ar(int pw, int ph, int dw, int dh, double ar, int max_x, int max_y, int *sx, int *sy)
{
    if (max_x < 1) max_x = 1;
    if (max_x > HIRES_MAX_SCALE) max_x = HIRES_MAX_SCALE;
    if (max_y < 1) max_y = 1;
    if (max_y > HIRES_MAX_SCALE) max_y = HIRES_MAX_SCALE;
    if (dw < 1) dw = 512;
    if (dh < 1) dh = 240;
    int vw = 1, vh = 1;
    if (pw >= 1 && ph >= 1) {                    /* letterbox viewport of aspect ar */
        if ((s64)pw * 3 > (s64)ph * 4 && ar == 4.0 / 3.0) { vh = ph; vw = ph * 4 / 3; }
        else if (ar == 4.0 / 3.0) { vw = pw; vh = pw * 3 / 4; }
        else if ((double)pw > (double)ph * ar) { vh = ph; vw = (int)(ph * ar); }
        else { vw = pw; vh = (int)(pw / ar); }
    }
    /* never upsample at present: at least one target pixel per viewport pixel on each axis */
    *sx = imin(imax((vw + dw - 1) / dw, 1), max_x);
    *sy = imin(imax((vh + dh - 1) / dh, 1), max_y);
}

bool hires_verify_possible(void)
{
    return g_hires && s_cur_sx == 1 && s_cur_sy == 1 && s_cur_fmt == HIRES_FMT_15 && s_tex_filter == HIRES_TEX_NEAREST;
}

u32 hires_verify_1x(int *first_x, int *first_y)
{
    HiresView v;
    if (!hires_verify_possible()) return 0;
    if (!g_hires->read(s_rx, s_ry, s_rw, s_rh, &v)) return 0;
    const u16 *vram = gpu_display_vram();
    u32 bad = 0;
    for (int y = 0; y < s_rh; y++)
        for (int x = 0; x < s_rw; x++)
            if (v.pixels[(size_t)y * v.stride + x] != vram[(s_ry + y) * VRAM_W + s_rx + x]) {
                if (!bad) { if (first_x) *first_x = s_rx + x; if (first_y) *first_y = s_ry + y; }
                bad++;
            }
    return bad;
}

static inline void cmp_px(HiresCompare *c, int x, int y, int dr, int dg, int db)
{
    /* channel differences on the 8-bit scale (a 15-bit step = 8) */
    if (!c->diff) { c->first_x = x; c->first_y = y; }
    c->diff++;
    int m = dr > dg ? dr : dg;
    if (db > m) m = db;
    if (m > 8) c->diff_big++;
    if (m > 16) c->diff_big2++;
    if (m > 32) c->diff_big4++;
    c->sum_abs += (u64)(dr + dg + db);
    c->sum_sq += (u64)(dr * dr + dg * dg + db * db);
}

bool hires_compare(HiresCompare *c)
{
    memset(c, 0, sizeof *c);
    c->first_x = c->first_y = -1;
    if (!g_hires || !s_shadow) return false;
    HiresView a, b;
    /* the whole target, guard band included */
    const int x0 = s_rx - s_cur_gx, w = s_rw + 2 * s_cur_gx;
    if (!s_shadow->read(x0, s_ry, w, s_rh, &b)) return false;
    if (!s_be->read(x0, s_ry, w, s_rh, &a)) return false;
    if (a.fmt != b.fmt) return false;
    if (a.fmt == HIRES_FMT_RGBA8) {
        for (int y = 0; y < a.h; y++) {
            const u32 *pa = a.px32 + (size_t)y * a.stride, *pb = b.px32 + (size_t)y * b.stride;
            if (!memcmp(pa, pb, (size_t)a.w * 4)) continue;
            for (int x = 0; x < a.w; x++) {
                u32 p = pa[x], q = pb[x];
                if (p == q) continue;
                if (((p ^ q) & 0xFFFFFF) == 0) { c->mask_diff++; continue; }
                cmp_px(c, x, y, abs((int)(p & 255) - (int)(q & 255)), abs((int)((p >> 8) & 255) - (int)((q >> 8) & 255)),
                       abs((int)((p >> 16) & 255) - (int)((q >> 16) & 255)));
            }
        }
        c->pixels = (u64)a.w * a.h;
        return true;
    }
    for (int y = 0; y < a.h; y++) {
        const u16 *pa = a.pixels + (size_t)y * a.stride, *pb = b.pixels + (size_t)y * b.stride;
        if (!memcmp(pa, pb, (size_t)a.w * 2)) continue;
        for (int x = 0; x < a.w; x++) {
            u16 p = pa[x], q = pb[x];
            if (p == q) continue;
            if (((p ^ q) & 0x7FFF) == 0) { c->mask_diff++; continue; }
            cmp_px(c, x, y, 8 * abs((p & 31) - (q & 31)), 8 * abs(((p >> 5) & 31) - ((q >> 5) & 31)),
                   8 * abs(((p >> 10) & 31) - ((q >> 10) & 31)));
        }
    }
    c->pixels = (u64)a.w * a.h;
    return true;
}

static u32 *view_rgba(const HiresView *v)
{
    u32 *o = malloc((size_t)v->w * v->h * 4);
    if (!o) return NULL;
    for (int y = 0; y < v->h; y++)
        for (int x = 0; x < v->w; x++) {
            u32 r, g, b;
            if (v->fmt == HIRES_FMT_RGBA8) {
                u32 p = v->px32[(size_t)y * v->stride + x];
                r = p & 255; g = (p >> 8) & 255; b = (p >> 16) & 255;
            } else {
                u16 p = v->pixels[(size_t)y * v->stride + x];
                r = (p & 31) << 3; g = ((p >> 5) & 31) << 3; b = ((p >> 10) & 31) << 3;
                r |= r >> 5; g |= g >> 5; b |= b >> 5;
            }
            o[(size_t)y * v->w + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    return o;
}

bool hires_compare_images(u32 **primary, u32 **shadow, int *w, int *h)
{
    *primary = *shadow = NULL;
    if (!g_hires || !s_shadow) return false;
    HiresView a, b;
    const int x0 = s_rx - s_cur_gx, ww = s_rw + 2 * s_cur_gx;
    if (!s_shadow->read(x0, s_ry, ww, s_rh, &b)) return false;
    *shadow = view_rgba(&b);
    if (!s_be->read(x0, s_ry, ww, s_rh, &a)) { free(*shadow); *shadow = NULL; return false; }
    *primary = view_rgba(&a);
    if (!*primary || !*shadow || a.w != b.w || a.h != b.h) {
        free(*primary); free(*shadow); *primary = *shadow = NULL;
        return false;
    }
    *w = a.w; *h = a.h;
    return true;
}
