/* PS1 GPU: register-level emulation with a software rasterizer.
 *
 * Semantics follow psx-spx (nocash). Rasterization rules (attribute
 * gradients, +0.5 bias, 24-bit fractional u32 interpolation) are modelled
 * after Mednafen's well-validated software GPU.
 *
 * Not emulated: texture cache (texture reads always see current VRAM),
 * interlaced "skip displayed field" drawing, timing/busy states (the GPU
 * always reports ready), the GPU FIFO depth.
 */
#include "psx.h"
#include "hires.h"
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

static u16 s_vram[VRAM_W * VRAM_H];

/* Optional presentation-only hi-res target (hires.c). Every drawing command is also
 * forwarded there; nothing in it is ever read back into the emulated machine. */
const HiresBackend *g_hires;

/* ---- state ------------------------------------------------------------- */

static u32 s_stat;            /* persistent GPUSTAT bits (ready bits computed on read) */
static u32 s_drawmode;        /* raw E1 value (bits 0..13) */
static u32 s_texwin;          /* raw E2 value (20 bits) */
static int s_clip_x1, s_clip_y1, s_clip_x2, s_clip_y2;
static u32 s_area_tl, s_area_br; /* raw E3/E4 values (for GP1 10h) */
static int s_off_x, s_off_y;
static u32 s_off_raw;
static bool s_mask_set, s_mask_check;
static bool s_tex_disable_allowed;
static u32 s_read_latch;

/* texture window precomputed */
static u32 s_tw_and_u, s_tw_or_u, s_tw_and_v, s_tw_or_v;

/* display */
static int s_disp_x, s_disp_y;
static u32 s_hrange, s_vrange;
static bool s_field;

/* command FIFO */
#define CMD_MAX 64
static u32 s_cmd[CMD_MAX];
static int s_cmd_len;
static int s_cmd_need;

enum { MODE_CMD, MODE_CPU2VRAM, MODE_POLYLINE };
static int s_mode;

/* VRAM transfers */
typedef struct {
    int x, y, w, h;
    int cx, cy;
    u32 remaining;   /* halfwords */
} Xfer;
static Xfer s_wr, s_rd;
static bool s_rd_active;

/* polyline state */
static bool s_pl_gouraud, s_pl_semi;
static int s_pl_x, s_pl_y;
static u32 s_pl_color;
static u32 s_pl_pending_color;
static bool s_pl_have_color;

/* ---- helpers ----------------------------------------------------------- */

static inline int sext11(u32 v) { return (int)((s32)(v << 21) >> 21); }

static inline u16 rgb24_to_15(u32 c)
{
    return (u16)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
}

static void update_texwin(void)
{
    u32 mx = s_texwin & 0x1F, my = (s_texwin >> 5) & 0x1F;
    u32 ox = (s_texwin >> 10) & 0x1F, oy = (s_texwin >> 15) & 0x1F;
    s_tw_and_u = ~(mx << 3) & 0xFF;
    s_tw_or_u = (ox & mx) << 3;
    s_tw_and_v = ~(my << 3) & 0xFF;
    s_tw_or_v = (oy & my) << 3;
}

static void set_drawmode_bits(u32 v, u32 mask)
{
    s_drawmode = (s_drawmode & ~mask) | (v & mask);
    /* GPUSTAT bits 0..10 mirror E1 0..10, bit 15 mirrors E1 bit 11 */
    s_stat = (s_stat & ~0x87FFu) | (s_drawmode & 0x7FF) | ((s_drawmode & 0x800) << 4);
}

static void reset_cmd_buffer(void)
{
    s_cmd_len = 0;
    s_cmd_need = 0;
    s_mode = MODE_CMD;
    s_rd_active = false;
}

/* dither matrix */
static const s8 s_dither[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

/* ---- primitive parameters ---------------------------------------------- */

typedef struct {
    int blend;          /* 0..3 */
    int depth;          /* 0=4bpp 1=8bpp 2=15bpp */
    u32 tp_x, tp_y;     /* texpage base */
    u32 clut_x, clut_y;
    bool dither;
    u16 mask_or;
    bool mask_check;
    u32 tw_and_u, tw_or_u, tw_and_v, tw_or_v;
} Prim;

static inline u16 tex_fetch(const Prim *p, int depth, u32 u, u32 v)
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

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Shade a pixel. Returns false if the pixel must not be written.
 * TEX: textured, RAW: raw texture, SEMI: semi-transparent command. */
static inline __attribute__((always_inline)) void
plot(const Prim *p, int x, int y, u32 r, u32 g, u32 b, u32 u, u32 v,
     const int TEX, const int DEPTH, const int RAW, const int SEMI, const int DITHER)
{
    u16 *dst = &s_vram[y * VRAM_W + x];
    if (p->mask_check && (*dst & 0x8000)) return;
    u16 out;
    bool do_blend;
    if (TEX) {
        u16 t = tex_fetch(p, DEPTH, u, v);
        if (t == 0) return;
        if (RAW) {
            out = t;
        } else {
            int tr = t & 0x1F, tg = (t >> 5) & 0x1F, tb = (t >> 10) & 0x1F;
            if (DITHER) {
                int d = s_dither[y & 3][x & 3];
                tr = clamp255((int)((tr * r) >> 4) + d) >> 3;
                tg = clamp255((int)((tg * g) >> 4) + d) >> 3;
                tb = clamp255((int)((tb * b) >> 4) + d) >> 3;
            } else {
                tr = (int)((tr * r) >> 7); if (tr > 31) tr = 31;
                tg = (int)((tg * g) >> 7); if (tg > 31) tg = 31;
                tb = (int)((tb * b) >> 7); if (tb > 31) tb = 31;
            }
            out = (u16)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
        }
        do_blend = SEMI && (t & 0x8000);
    } else {
        int cr, cg, cb;
        if (DITHER) {
            int d = s_dither[y & 3][x & 3];
            cr = clamp255((int)r + d) >> 3;
            cg = clamp255((int)g + d) >> 3;
            cb = clamp255((int)b + d) >> 3;
        } else {
            cr = (int)r >> 3; cg = (int)g >> 3; cb = (int)b >> 3;
        }
        out = (u16)(cr | (cg << 5) | (cb << 10));
        do_blend = SEMI;
    }
    if (do_blend) out = blend_px(p->blend, *dst, out);
    *dst = out | p->mask_or;
}

/* ---- triangles ----------------------------------------------------------- */

typedef struct {
    s32 x, y;
    u32 r, g, b;
    u32 u, v;
} Vtx;

typedef struct {
    u32 drdx, drdy, dgdx, dgdy, dbdx, dbdy;
    u32 dudx, dudy, dvdx, dvdy;
} Grad;

#define FRAC 24

/* UV range actually sampled by the 1x rasterizer for the current textured triangle (pre texture
 * window), recorded only while a hi-res target exists. The hi-res rasterizer clamps its u/v to
 * it: the 1x GPU never draws a triangle's last column/row, so the vertex UV range includes
 * texels (often a neighbouring tile's border) that 1x never reads. Along a span u,v are linear
 * in x, so the extremes are at its ends. Bookkeeping only; 1x output is unchanged. */
static bool s_uv_track;
static int s_trk_umin, s_trk_umax, s_trk_vmin, s_trk_vmax;

static void uv_track_span(u32 u, u32 v, u32 dudx, u32 dvdx, int n)
{
    u32 u1 = u + dudx * (u32)(n - 1), v1 = v + dvdx * (u32)(n - 1);
    int a = (int)(u >> FRAC), b = (int)(u1 >> FRAC);
    int c = (int)(v >> FRAC), d = (int)(v1 >> FRAC);
    /* a wrap of the 32-bit fixed value inside the span: the sampled values cover 0..255 */
    if (n > 1 && (((s32)dudx >= 0) ? u1 < u : u1 > u)) { a = 0; b = 255; }
    if (n > 1 && (((s32)dvdx >= 0) ? v1 < v : v1 > v)) { c = 0; d = 255; }
    if (a > b) { int t = a; a = b; b = t; }
    if (c > d) { int t = c; c = d; d = t; }
    if (a < s_trk_umin) s_trk_umin = a;
    if (b > s_trk_umax) s_trk_umax = b;
    if (c < s_trk_vmin) s_trk_vmin = c;
    if (d > s_trk_vmax) s_trk_vmax = d;
}

static inline __attribute__((always_inline)) void
span(const Prim *p, const Grad *gr, int y, int xs, int xe,
     u32 r, u32 g, u32 b, u32 u, u32 v,
     const int TEX, const int DEPTH, const int RAW, const int SEMI,
     const int GOUR, const int DITHER)
{
    const u32 drdx = gr->drdx, dgdx = gr->dgdx, dbdx = gr->dbdx, dudx = gr->dudx, dvdx = gr->dvdx;
    if (TEX && s_uv_track) uv_track_span(u, v, dudx, dvdx, xe - xs);
    for (int x = xs; x < xe; x++) {
        plot(p, x, y, r >> FRAC, g >> FRAC, b >> FRAC, u >> FRAC, v >> FRAC,
             TEX, DEPTH, RAW, SEMI, DITHER);
        if (GOUR) { r += drdx; g += dgdx; b += dbdx; }
        if (TEX) { u += dudx; v += dvdx; }
    }
}

static inline s64 floor_div(s64 a, s64 b)
{
    s64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static inline __attribute__((always_inline)) void
tri_impl(const Prim *p, const Vtx *v0, const Vtx *v1, const Vtx *v2,
         const int TEX, const int DEPTH, const int RAW, const int SEMI,
         const int GOUR, const int DITHER)
{
    const Prim lp = *p; /* local copy: keeps fields in registers despite VRAM stores */
    /* sort by y */
    const Vtx *a = v0, *b = v1, *c = v2, *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    if (b->y < a->y) { t = a; a = b; b = t; }

    if (c->y == a->y) return;
    s32 denom = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (denom == 0) return;

    /* attribute gradients (Mednafen-style: 12-bit quotient << 12) */
    Grad gr;
    memset(&gr, 0, sizeof gr);
#define GRAD(attr, dx, dy)                                                              \
    do {                                                                                \
        s64 n_x = (s64)((s32)b->attr - (s32)a->attr) * (c->y - a->y) -                  \
                  (s64)((s32)c->attr - (s32)a->attr) * (b->y - a->y);                   \
        s64 n_y = (s64)((s32)c->attr - (s32)a->attr) * (b->x - a->x) -                  \
                  (s64)((s32)b->attr - (s32)a->attr) * (c->x - a->x);                   \
        dx = (u32)(s32)((n_x * 4096) / denom) << 12;                                  \
        dy = (u32)(s32)((n_y * 4096) / denom) << 12;                                  \
    } while (0)
    if (GOUR) { GRAD(r, gr.drdx, gr.drdy); GRAD(g, gr.dgdx, gr.dgdy); GRAD(b, gr.dbdx, gr.dbdy); }
    if (TEX) { GRAD(u, gr.dudx, gr.dudy); GRAD(v, gr.dvdx, gr.dvdy); }
#undef GRAD

    /* value at origin (0,0) so per-row/per-pixel values are base + dx*x + dy*y */
    const u32 bias = 1u << (FRAC - 1);
    u32 r0 = (a->r << FRAC) + bias - gr.drdx * (u32)a->x - gr.drdy * (u32)a->y;
    u32 g0 = (a->g << FRAC) + bias - gr.dgdx * (u32)a->x - gr.dgdy * (u32)a->y;
    u32 b0 = (a->b << FRAC) + bias - gr.dbdx * (u32)a->x - gr.dbdy * (u32)a->y;
    u32 u0 = (a->u << FRAC) + bias - gr.dudx * (u32)a->x - gr.dudy * (u32)a->y;
    u32 vv0 = (a->v << FRAC) + bias - gr.dvdx * (u32)a->x - gr.dvdy * (u32)a->y;
    if (!GOUR) { r0 = a->r << FRAC; g0 = a->g << FRAC; b0 = a->b << FRAC; }

    /* edges in 32.32 fixed point; long edge a->c, short a->b then b->c */
    bool long_left = denom > 0; /* middle vertex right of long edge */
    s64 long_step = floor_div((s64)(c->x - a->x) << 32, c->y - a->y);

    for (int part = 0; part < 2; part++) {
        const Vtx *sa = part ? b : a, *sb = part ? c : b;
        int ys = sa->y, ye = sb->y;
        if (ys == ye) continue;
        s64 short_step = floor_div((s64)(sb->x - sa->x) << 32, ye - ys);
        int y_first = ys < s_clip_y1 ? s_clip_y1 : ys;
        int y_last = ye - 1 > s_clip_y2 ? s_clip_y2 : ye - 1;
        if (y_first > y_last) continue;
        s64 lx = ((s64)a->x << 32) + long_step * (y_first - a->y);
        s64 sx = ((s64)sa->x << 32) + short_step * (y_first - ys);
        for (int y = y_first; y <= y_last; y++) {
            s64 xl = long_left ? lx : sx, xr = long_left ? sx : lx;
            int xs = (int)((xl + 0xFFFFFFFFll) >> 32);
            int xe = (int)((xr + 0xFFFFFFFFll) >> 32);
            if (xs < s_clip_x1) xs = s_clip_x1;
            if (xe > s_clip_x2 + 1) xe = s_clip_x2 + 1;
            if (xs < xe) {
                u32 ry = (u32)y, rx = (u32)xs;
                u32 r = r0, g = g0, bb = b0, u = u0, v = vv0;
                if (GOUR) {
                    r += gr.drdx * rx + gr.drdy * ry;
                    g += gr.dgdx * rx + gr.dgdy * ry;
                    bb += gr.dbdx * rx + gr.dbdy * ry;
                }
                if (TEX) {
                    u += gr.dudx * rx + gr.dudy * ry;
                    v += gr.dvdx * rx + gr.dvdy * ry;
                }
                span(&lp, &gr, y, xs, xe, r, g, bb, u, v, TEX, DEPTH, RAW, SEMI, GOUR, DITHER);
            }
            lx += long_step;
            sx += short_step;
        }
    }
}

typedef void (*TriFn)(const Prim *, const Vtx *, const Vtx *, const Vtx *);

#define TRI_FN(name, TEX, DEPTH, RAW, SEMI, GOUR, DITHER)                         \
    static void name(const Prim *p, const Vtx *a, const Vtx *b, const Vtx *c)   \
    { tri_impl(p, a, b, c, TEX, DEPTH, RAW, SEMI, GOUR, DITHER); }

/* untextured: index = semi*4 + gour*2 + dither */
TRI_FN(tri_u000, 0, 0, 0, 0, 0, 0)
TRI_FN(tri_u001, 0, 0, 0, 0, 0, 1)
TRI_FN(tri_u010, 0, 0, 0, 0, 1, 0)
TRI_FN(tri_u011, 0, 0, 0, 0, 1, 1)
TRI_FN(tri_u100, 0, 0, 0, 1, 0, 0)
TRI_FN(tri_u101, 0, 0, 0, 1, 0, 1)
TRI_FN(tri_u110, 0, 0, 0, 1, 1, 0)
TRI_FN(tri_u111, 0, 0, 0, 1, 1, 1)
static const TriFn s_tri_untex[8] = {
    tri_u000, tri_u001, tri_u010, tri_u011, tri_u100, tri_u101, tri_u110, tri_u111,
};

/* textured: [depth][raw][semi][gour][dither]; raw implies no gouraud/dither use */
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
static const TriFn s_tri_tex[3][10] = {
    TRI_TEX_TABLE(0), TRI_TEX_TABLE(1), TRI_TEX_TABLE(2),
};

static void hires_prim(HiresPrim *h, const Prim *p, bool tex, bool raw, bool semi, bool gour)
{
    h->tex = tex; h->raw = raw; h->semi = semi; h->gour = gour; h->dither = p->dither;
    h->blend = p->blend; h->depth = p->depth;
    h->tp_x = p->tp_x; h->tp_y = p->tp_y; h->clut_x = p->clut_x; h->clut_y = p->clut_y;
    h->tw_and_u = p->tw_and_u; h->tw_or_u = p->tw_or_u;
    h->tw_and_v = p->tw_and_v; h->tw_or_v = p->tw_or_v;
    h->mask_or = p->mask_or; h->mask_check = p->mask_check;
    h->has_uvr = false;
    h->clip_x1 = s_clip_x1; h->clip_y1 = s_clip_y1; h->clip_x2 = s_clip_x2; h->clip_y2 = s_clip_y2;
}

static void draw_tri(const Prim *p, const Vtx *a, const Vtx *b, const Vtx *c,
                     bool tex, bool raw, bool semi, bool gour)
{
    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    if (b->x < minx) minx = b->x;
    if (b->x > maxx) maxx = b->x;
    if (c->x < minx) minx = c->x;
    if (c->x > maxx) maxx = c->x;
    if (b->y < miny) miny = b->y;
    if (b->y > maxy) maxy = b->y;
    if (c->y < miny) miny = c->y;
    if (c->y > maxy) maxy = c->y;
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    if (maxx < s_clip_x1 || minx > s_clip_x2 || maxy < s_clip_y1 || miny > s_clip_y2) return;

    s_uv_track = g_hires && tex;
    if (s_uv_track) { s_trk_umin = s_trk_vmin = 1 << 20; s_trk_umax = s_trk_vmax = -1; }

    if (!tex) {
        int idx = (semi ? 4 : 0) | (gour ? 2 : 0) | (p->dither ? 1 : 0);
        s_tri_untex[idx](p, a, b, c);
    } else if (raw) {
        s_tri_tex[p->depth][semi ? 1 : 0](p, a, b, c);
    } else {
        int idx = 2 + ((semi ? 4 : 0) | (gour ? 2 : 0) | (p->dither ? 1 : 0));
        s_tri_tex[p->depth][idx](p, a, b, c);
    }

    /* forward to the hi-res target after the 1x draw, with the UV range 1x actually sampled */
    if (g_hires) {
        HiresPrim hp;
        hires_prim(&hp, p, tex, raw, semi, gour);
        if (s_uv_track && s_trk_umax >= 0) {
            hp.has_uvr = true;
            hp.uvr_umin = s_trk_umin; hp.uvr_umax = s_trk_umax;
            hp.uvr_vmin = s_trk_vmin; hp.uvr_vmax = s_trk_vmax;
        }
        s_uv_track = false;
        const Vtx *vs[3] = { a, b, c };
        HiresVtx hv[3];
        for (int i = 0; i < 3; i++) {
            hv[i].x = vs[i]->x * 65536; hv[i].y = vs[i]->y * 65536;
            hv[i].r = vs[i]->r; hv[i].g = vs[i]->g; hv[i].b = vs[i]->b;
            hv[i].u = vs[i]->u; hv[i].v = vs[i]->v;
        }
        g_hires->tri(&hp, hv);
    }
}

static void prim_common(Prim *p)
{
    p->blend = (int)((s_drawmode >> 5) & 3);
    p->depth = (int)((s_drawmode >> 7) & 3);
    if (p->depth == 3) p->depth = 2;
    p->tp_x = (s_drawmode & 0xF) * 64;
    p->tp_y = ((s_drawmode >> 4) & 1) * 256;
    p->mask_or = s_mask_set ? 0x8000 : 0;
    p->mask_check = s_mask_check;
    p->clut_x = p->clut_y = 0;
    p->dither = false;
    p->tw_and_u = s_tw_and_u;
    p->tw_or_u = s_tw_or_u;
    p->tw_and_v = s_tw_and_v;
    p->tw_or_v = s_tw_or_v;
}

static void gp0_polygon(const u32 *w)
{
    u32 cmd = w[0] >> 24;
    bool gour = (cmd & 0x10) != 0;
    bool quad = (cmd & 0x08) != 0;
    bool tex = (cmd & 0x04) != 0;
    bool semi = (cmd & 0x02) != 0;
    bool raw = (cmd & 0x01) != 0;
    int n = quad ? 4 : 3;
    Vtx v[4];
    int i = 0;
    u32 color = w[0] & 0xFFFFFF;
    u32 clut = 0;
    for (int k = 0; k < n; k++) {
        if (gour && k > 0) color = w[i] & 0xFFFFFF;
        if (k == 0 || gour) i++;
        u32 xy = w[i++];
        v[k].x = sext11((u32)(sext11(xy) + s_off_x));
        v[k].y = sext11((u32)(sext11(xy >> 16) + s_off_y));
        v[k].r = color & 0xFF;
        v[k].g = (color >> 8) & 0xFF;
        v[k].b = (color >> 16) & 0xFF;
        v[k].u = v[k].v = 0;
        if (tex) {
            u32 uv = w[i++];
            v[k].u = uv & 0xFF;
            v[k].v = (uv >> 8) & 0xFF;
            if (k == 0) clut = uv >> 16;
            if (k == 1) {
                u32 tp = uv >> 16;
                u32 mask = 0x1FF | (s_tex_disable_allowed ? 0x800 : 0);
                set_drawmode_bits(tp, mask);
            }
        }
    }
    Prim p;
    prim_common(&p);
    p.clut_x = (clut & 0x3F) * 16;
    p.clut_y = (clut >> 6) & 0x1FF;
    bool shaded = gour && !(tex && raw);
    p.dither = ((s_drawmode >> 9) & 1) && (gour || (tex && !raw));
    if (tex && raw) {
        for (int k = 0; k < n; k++) v[k].r = v[k].g = v[k].b = 0x80;
    }
    draw_tri(&p, &v[0], &v[1], &v[2], tex, raw, semi, shaded);
    if (quad) draw_tri(&p, &v[1], &v[2], &v[3], tex, raw, semi, shaded);
}

/* ---- rectangles --------------------------------------------------------- */

static inline __attribute__((always_inline)) void
rect_impl(const Prim *p, int x0, int y0, int w, int h, u32 r, u32 g, u32 b,
          u32 u0, u32 v0, bool flipx, bool flipy,
          const int TEX, const int DEPTH, const int RAW, const int SEMI)
{
    int xs = x0, xe = x0 + w, ys = y0, ye = y0 + h;
    if (xs < s_clip_x1) xs = s_clip_x1;
    if (xe > s_clip_x2 + 1) xe = s_clip_x2 + 1;
    if (ys < s_clip_y1) ys = s_clip_y1;
    if (ye > s_clip_y2 + 1) ye = s_clip_y2 + 1;
    if (xs >= xe || ys >= ye) return;
    int du = flipx ? -1 : 1, dv = flipy ? -1 : 1;
    if (TEX && flipx) u0 |= 1;
    for (int y = ys; y < ye; y++) {
        u32 v = (v0 + (u32)(dv * (y - y0))) & 0xFF;
        u32 u = (u0 + (u32)(du * (xs - x0))) & 0xFF;
        for (int x = xs; x < xe; x++) {
            plot(p, x, y, r, g, b, u, v, TEX, DEPTH, RAW, SEMI, 0);
            u = (u + (u32)du) & 0xFF;
        }
    }
}

typedef void (*RectFn)(const Prim *, int, int, int, int, u32, u32, u32, u32, u32, bool, bool);
#define RECT_FN(name, TEX, DEPTH, RAW, SEMI)                                                  \
    static void name(const Prim *p, int x0, int y0, int w, int h, u32 r, u32 g, u32 b,     \
                     u32 u0, u32 v0, bool fx, bool fy)                                    \
    { rect_impl(p, x0, y0, w, h, r, g, b, u0, v0, fx, fy, TEX, DEPTH, RAW, SEMI); }
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
static const RectFn s_rect_tex[3][4] = {
    { rect_t0_00, rect_t0_01, rect_t0_10, rect_t0_11 },
    { rect_t1_00, rect_t1_01, rect_t1_10, rect_t1_11 },
    { rect_t2_00, rect_t2_01, rect_t2_10, rect_t2_11 },
};

static void gp0_rect(const u32 *w)
{
    u32 cmd = w[0] >> 24;
    bool tex = (cmd & 4) != 0, semi = (cmd & 2) != 0, raw = (cmd & 1) != 0;
    int sz = (cmd >> 3) & 3;
    u32 color = w[0] & 0xFFFFFF;
    int x = sext11((u32)(sext11(w[1]) + s_off_x));
    int y = sext11((u32)(sext11(w[1] >> 16) + s_off_y));
    int i = 2;
    u32 u0 = 0, v0 = 0, clut = 0;
    if (tex) {
        u32 uv = w[i++];
        u0 = uv & 0xFF;
        v0 = (uv >> 8) & 0xFF;
        clut = uv >> 16;
    }
    int wd, ht;
    switch (sz) {
    case 0: wd = (int)(w[i] & 0x3FF); ht = (int)((w[i] >> 16) & 0x1FF); break;
    case 1: wd = ht = 1; break;
    case 2: wd = ht = 8; break;
    default: wd = ht = 16; break;
    }
    if (wd == 0 || ht == 0) return;
    Prim p;
    prim_common(&p);
    p.clut_x = (clut & 0x3F) * 16;
    p.clut_y = (clut >> 6) & 0x1FF;
    u32 r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    bool fx = (s_drawmode >> 12) & 1, fy = (s_drawmode >> 13) & 1;
    if (g_hires) {
        HiresPrim hp;
        hires_prim(&hp, &p, tex, raw, semi, false);
        g_hires->rect(&hp, x, y, wd, ht, r, g, b, u0, v0, fx, fy);
    }
    if (!tex) {
        (semi ? rect_u1 : rect_u0)(&p, x, y, wd, ht, r, g, b, 0, 0, false, false);
    } else {
        s_rect_tex[p.depth][(raw ? 2 : 0) | (semi ? 1 : 0)](&p, x, y, wd, ht, r, g, b, u0, v0, fx, fy);
    }
}

/* ---- lines ----------------------------------------------------------------- */

static void draw_line(int x0, int y0, u32 c0, int x1, int y1, u32 c1, bool gour, bool semi)
{
    x0 = sext11((u32)(x0 + s_off_x));
    y0 = sext11((u32)(y0 + s_off_y));
    x1 = sext11((u32)(x1 + s_off_x));
    y1 = sext11((u32)(y1 + s_off_y));
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= 1024 || ady >= 512) return;
    Prim p;
    prim_common(&p);
    bool dither = gour && ((s_drawmode >> 9) & 1);
    if (!gour) c1 = c0;
    if (g_hires) {
        HiresPrim hp;
        hires_prim(&hp, &p, false, false, semi, gour);
        hp.dither = dither;
        g_hires->line(&hp, x0, y0, c0, x1, y1, c1);
    }
    int k = adx > ady ? adx : ady;
    /* 32.32 fixed point positions, centered */
    s64 xf = ((s64)x0 << 32) + (1ll << 31);
    s64 yf = ((s64)y0 << 32) + (1ll << 31);
    s64 sx = 0, sy = 0;
    s32 r = (s32)((c0 & 0xFF) << 12), g = (s32)(((c0 >> 8) & 0xFF) << 12), b = (s32)(((c0 >> 16) & 0xFF) << 12);
    s32 dr = 0, dg = 0, db = 0;
    if (k) {
        sx = ((s64)dx << 32) / k;
        sy = ((s64)dy << 32) / k;
        dr = (s32)(((s32)(c1 & 0xFF) - (s32)(c0 & 0xFF)) * 4096 / k);
        dg = (s32)(((s32)((c1 >> 8) & 0xFF) - (s32)((c0 >> 8) & 0xFF)) * 4096 / k);
        db = (s32)(((s32)((c1 >> 16) & 0xFF) - (s32)((c0 >> 16) & 0xFF)) * 4096 / k);
    }
    /* bias negative-going steps so exact .5 values round consistently */
    if (dx < 0) xf -= 1;
    if (dy < 0) yf -= 1;
    r += 1 << 11; g += 1 << 11; b += 1 << 11;
    for (int i = 0; i <= k; i++) {
        int x = (int)(xf >> 32), y = (int)(yf >> 32);
        if (x >= s_clip_x1 && x <= s_clip_x2 && y >= s_clip_y1 && y <= s_clip_y2) {
            u32 cr = (u32)(r >> 12), cg = (u32)(g >> 12), cb = (u32)(b >> 12);
            if (dither) {
                if (semi) plot(&p, x, y, cr, cg, cb, 0, 0, 0, 0, 0, 1, 1);
                else plot(&p, x, y, cr, cg, cb, 0, 0, 0, 0, 0, 0, 1);
            } else {
                if (semi) plot(&p, x, y, cr, cg, cb, 0, 0, 0, 0, 0, 1, 0);
                else plot(&p, x, y, cr, cg, cb, 0, 0, 0, 0, 0, 0, 0);
            }
        }
        xf += sx; yf += sy;
        r += dr; g += dg; b += db;
    }
}

/* ---- VRAM ops ----------------------------------------------------------- */

static void gp0_fill(const u32 *w)
{
    u16 c = rgb24_to_15(w[0]);
    int x = (int)(w[1] & 0x3F0);
    int y = (int)((w[1] >> 16) & 0x1FF);
    int wd = (int)(((w[2] & 0x3FF) + 0xF) & ~0xFu);
    int ht = (int)((w[2] >> 16) & 0x1FF);
    if (g_hires) g_hires->fill(x, y, wd, ht, c);
    for (int j = 0; j < ht; j++) {
        u16 *row = &s_vram[((y + j) & 511) * VRAM_W];
        if (x + wd <= VRAM_W) {
            for (int i = 0; i < wd; i++) row[x + i] = c;
        } else {
            for (int i = 0; i < wd; i++) row[(x + i) & 1023] = c;
        }
    }
}

static inline void vram_put(int x, int y, u16 v)
{
    u16 *d = &s_vram[(y & 511) * VRAM_W + (x & 1023)];
    if (s_mask_check && (*d & 0x8000)) return;
    *d = v | (s_mask_set ? 0x8000 : 0);
}

static void gp0_copy(const u32 *w)
{
    int sx = (int)(w[1] & 0x3FF), sy = (int)((w[1] >> 16) & 0x1FF);
    int dx = (int)(w[2] & 0x3FF), dy = (int)((w[2] >> 16) & 0x1FF);
    int wd = (int)(((w[3] & 0xFFFF) - 1) & 0x3FF) + 1;
    int ht = (int)((((w[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    static u16 buf[1024];
    if (g_hires) g_hires->copy(sx, sy, dx, dy, wd, ht, s_mask_check, s_mask_set);
    for (int j = 0; j < ht; j++) {
        const u16 *src = &s_vram[((sy + j) & 511) * VRAM_W];
        for (int i = 0; i < wd; i++) buf[i] = src[(sx + i) & 1023];
        for (int i = 0; i < wd; i++) vram_put(dx + i, dy + j, buf[i]);
    }
}

static void xfer_setup(Xfer *t, const u32 *w)
{
    t->x = (int)(w[1] & 0x3FF);
    t->y = (int)((w[1] >> 16) & 0x1FF);
    t->w = (int)(((w[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    t->h = (int)((((w[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    t->cx = t->cy = 0;
    t->remaining = (u32)t->w * (u32)t->h;
}

static inline void xfer_advance(Xfer *t)
{
    if (++t->cx >= t->w) { t->cx = 0; t->cy++; }
    t->remaining--;
}

static void cpu2vram_word(u32 v)
{
    for (int k = 0; k < 2 && s_wr.remaining; k++) {
        if (g_hires)
            g_hires->put(s_wr.x + s_wr.cx, s_wr.y + s_wr.cy, (u16)(k ? v >> 16 : v),
                         s_mask_check, s_mask_set);
        vram_put(s_wr.x + s_wr.cx, s_wr.y + s_wr.cy, (u16)(k ? v >> 16 : v));
        xfer_advance(&s_wr);
    }
    if (!s_wr.remaining) s_mode = MODE_CMD;
}

/* ---- GP0 dispatch -------------------------------------------------------- */

static int gp0_len(u32 cmd)
{
    switch (cmd >> 5) {
    case 0: return cmd == 0x02 ? 3 : 1;
    case 1: {
        int n = (cmd & 8) ? 4 : 3;
        return 1 + n + ((cmd & 4) ? n : 0) + ((cmd & 0x10) ? n - 1 : 0);
    }
    case 2: return (cmd & 0x10) ? 4 : 3;
    case 3: return 2 + ((cmd & 4) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
    case 4: return 4;
    case 5: return 3;
    case 6: return 3;
    default: return 1;
    }
}

static void gp0_env(u32 v)
{
    u32 cmd = v >> 24;
    switch (cmd) {
    case 0xE1:
        set_drawmode_bits(v, 0x3FFF & ~(s_tex_disable_allowed ? 0 : 0x800u));
        break;
    case 0xE2:
        s_texwin = v & 0xFFFFF;
        update_texwin();
        break;
    case 0xE3:
        s_area_tl = v & 0xFFFFF;
        s_clip_x1 = (int)(v & 0x3FF);
        s_clip_y1 = (int)((v >> 10) & 0x1FF);
        break;
    case 0xE4:
        s_area_br = v & 0xFFFFF;
        s_clip_x2 = (int)(v & 0x3FF);
        s_clip_y2 = (int)((v >> 10) & 0x1FF);
        break;
    case 0xE5:
        s_off_raw = v & 0x3FFFFF;
        s_off_x = sext11(v);
        s_off_y = sext11(v >> 11);
        break;
    case 0xE6:
        s_mask_set = v & 1;
        s_mask_check = (v >> 1) & 1;
        s_stat = (s_stat & ~0x1800u) | ((v & 3) << 11);
        break;
    default:
        break;
    }
}

static void gp0_execute(const u32 *w)
{
    u32 cmd = w[0] >> 24;
    switch (cmd >> 5) {
    case 0:
        if (cmd == 0x02) gp0_fill(w);
        else if (cmd == 0x1F) {
            s_stat |= 1u << 24;
            irq_raise(IRQ_GPU);
        }
        break;
    case 1: gp0_polygon(w); break;
    case 2: {
        bool gour = (cmd & 0x10) != 0;
        bool semi = (cmd & 0x02) != 0;
        u32 c0 = w[0] & 0xFFFFFF;
        u32 p0 = w[1];
        u32 c1 = gour ? (w[2] & 0xFFFFFF) : c0;
        u32 p1 = gour ? w[3] : w[2];
        draw_line(sext11(p0), sext11(p0 >> 16), c0, sext11(p1), sext11(p1 >> 16), c1, gour, semi);
        if (cmd & 0x08) {
            s_mode = MODE_POLYLINE;
            s_pl_gouraud = gour;
            s_pl_semi = semi;
            s_pl_x = sext11(p1);
            s_pl_y = sext11(p1 >> 16);
            s_pl_color = c1;
            s_pl_have_color = false;
        }
        break;
    }
    case 3: gp0_rect(w); break;
    case 4: gp0_copy(w); break;
    case 5:
        xfer_setup(&s_wr, w);
        s_mode = MODE_CPU2VRAM;
        break;
    case 6:
        xfer_setup(&s_rd, w);
        s_rd_active = true;
        break;
    default:
        gp0_env(w[0]);
        break;
    }
}

static void polyline_word(u32 v)
{
    if ((v & 0xF000F000u) == 0x50005000u && !(s_pl_gouraud && s_pl_have_color)) {
        s_mode = MODE_CMD;
        return;
    }
    if (s_pl_gouraud && !s_pl_have_color) {
        s_pl_pending_color = v & 0xFFFFFF;
        s_pl_have_color = true;
        return;
    }
    u32 c1 = s_pl_gouraud ? s_pl_pending_color : s_pl_color;
    int x1 = sext11(v), y1 = sext11(v >> 16);
    draw_line(s_pl_x, s_pl_y, s_pl_color, x1, y1, c1, s_pl_gouraud, s_pl_semi);
    s_pl_x = x1;
    s_pl_y = y1;
    s_pl_color = c1;
    s_pl_have_color = false;
}

void gpu_write_gp0(u32 v)
{
    if (s_mode == MODE_CPU2VRAM) {
        cpu2vram_word(v);
        return;
    }
    if (s_mode == MODE_POLYLINE) {
        polyline_word(v);
        return;
    }
    if (s_cmd_len == 0) {
        s_cmd_need = gp0_len(v >> 24);
    }
    s_cmd[s_cmd_len++] = v;
    if (s_cmd_len >= s_cmd_need) {
        s_cmd_len = 0;
        gp0_execute(s_cmd);
    }
}

/* ---- GP1 ----------------------------------------------------------------- */

static void gpu_reset_state(void)
{
    reset_cmd_buffer();
    s_stat = 0x14802000;
    s_drawmode = 0;
    s_texwin = 0;
    update_texwin();
    s_clip_x1 = s_clip_y1 = s_clip_x2 = s_clip_y2 = 0;
    s_area_tl = s_area_br = 0;
    s_off_x = s_off_y = 0;
    s_off_raw = 0;
    s_mask_set = s_mask_check = false;
    s_tex_disable_allowed = false;
    s_disp_x = s_disp_y = 0;
    s_hrange = 0x200 | (0xC00 << 12);
    s_vrange = 0x10 | (0x100 << 10);
    s_field = false;
}

void gpu_write_gp1(u32 v)
{
    u32 cmd = (v >> 24) & 0x3F;
    switch (cmd) {
    case 0x00:
        gpu_reset_state();
        break;
    case 0x01:
        reset_cmd_buffer();
        break;
    case 0x02:
        s_stat &= ~(1u << 24);
        break;
    case 0x03:
        s_stat = (s_stat & ~(1u << 23)) | ((v & 1) << 23);
        break;
    case 0x04:
        s_stat = (s_stat & ~(3u << 29)) | ((v & 3) << 29);
        break;
    case 0x05:
        s_disp_x = (int)(v & 0x3FF);
        s_disp_y = (int)((v >> 10) & 0x1FF);
        break;
    case 0x06:
        s_hrange = v & 0xFFFFFF;
        break;
    case 0x07:
        s_vrange = v & 0xFFFFF;
        break;
    case 0x08: {
        u32 st = s_stat & ~((0x3Fu << 17) | (1u << 16) | (1u << 14));
        st |= (v & 0x3F) << 17;          /* bits 0-5 -> 17-22 */
        st |= ((v >> 6) & 1) << 16;      /* hres2 */
        st |= ((v >> 7) & 1) << 14;      /* reverse flag */
        s_stat = st;
        break;
    }
    case 0x09:
        s_tex_disable_allowed = v & 1;
        break;
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {
            switch (v & 7) {
            case 2: s_read_latch = s_texwin; break;
            case 3: s_read_latch = s_area_tl; break;
            case 4: s_read_latch = s_area_br; break;
            case 5: s_read_latch = s_off_raw; break;
            case 7: s_read_latch = 2; break;
            default: break;
            }
        }
        break;
    }
}

/* ---- reads ------------------------------------------------------------------ */

u32 gpu_read_data(void)
{
    if (!s_rd_active) return s_read_latch;
    u32 out = 0;
    for (int k = 0; k < 2; k++) {
        u16 h = 0;
        if (s_rd.remaining) {
            h = s_vram[((s_rd.y + s_rd.cy) & 511) * VRAM_W + ((s_rd.x + s_rd.cx) & 1023)];
            xfer_advance(&s_rd);
        }
        out |= (u32)h << (16 * k);
    }
    if (!s_rd.remaining) s_rd_active = false;
    s_read_latch = out;
    return out;
}

static bool interlaced(void)
{
    return (s_stat >> 22) & 1;
}

u32 gpu_read_stat(void)
{
    u32 st = s_stat & ~((1u << 25) | (7u << 26) | (1u << 31) | (1u << 13));
    st |= 1u << 26;                         /* ready to receive command */
    if (s_rd_active) st |= 1u << 27;        /* ready to send VRAM to CPU */
    st |= 1u << 28;                         /* ready to receive DMA block */
    switch ((s_stat >> 29) & 3) {
    case 0: break;
    case 1: st |= 1u << 25; break;          /* FIFO not full */
    case 2: st |= 1u << 25; break;          /* same as bit 28 */
    case 3: if (s_rd_active) st |= 1u << 25; break; /* same as bit 27 */
    }
    if (interlaced()) {
        if (s_field) st |= (1u << 31) | (1u << 13);
    } else {
        /* Non-interlaced: bit 31 toggles every scanline on hardware; emulate by
         * toggling on each read so polling loops make progress. Bit 13 = 1. */
        s_field = !s_field;
        st |= 1u << 13;
        if (s_field) st |= 1u << 31;
    }
    return st;
}

void gpu_vblank(void)
{
    if (interlaced()) s_field = !s_field;
}

u16 *gpu_vram(void) { return s_vram; }

void gpu_get_display(GpuDisplay *d)
{
    d->x = s_disp_x;
    d->y = s_disp_y;
    if ((s_stat >> 16) & 1) {
        d->w = 368;
    } else {
        static const int widths[4] = { 256, 320, 512, 640 };
        d->w = widths[(s_stat >> 17) & 3];
    }
    d->h = (((s_stat >> 19) & 1) && ((s_stat >> 22) & 1)) ? 480 : 240;
    d->rgb24 = (s_stat >> 21) & 1;
    d->enabled = !((s_stat >> 23) & 1);
}

void gpu_init(void)
{
    memset(s_vram, 0, sizeof s_vram);
    s_read_latch = 0;
    gpu_reset_state();
}
