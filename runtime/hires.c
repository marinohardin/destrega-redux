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
 */
#include "hires.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VRAM_W 1024
#define VRAM_H 512

static const u16 *s_vram;              /* 1x VRAM (texture/CLUT source) */
static u16 *s_t;                       /* target pixels */
static int s_S;                        /* current scale; 0 = none */
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

/* Compute the colour of one pixel. Returns false for a transparent texel.
 * (x1, y1) is the 1x pixel the sample belongs to (dither pattern position). */
static inline __attribute__((always_inline)) bool
shade(const HiresPrim *p, int x1, int y1, u32 r, u32 g, u32 b, u32 u, u32 v,
      const int TEX, const int DEPTH, const int RAW, const int SEMI, const int DITHER,
      u16 *out, bool *blend)
{
    if (TEX) {
        u16 t = tex_fetch(p, DEPTH, u, v);
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
    } else {
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
    }
    return true;
}

static inline void store_px(const HiresPrim *p, u16 *dst, u16 out, bool blend)
{
    if (p->mask_check && (*dst & 0x8000)) return;
    if (blend) out = blend_px(p->blend, *dst, out);
    *dst = out | p->mask_or;
}

/* ---- triangles ------------------------------------------------------------- */

#define FRAC 24

typedef struct {
    s32 X1, Y1;      /* 1x, 16.16 */
    s64 Xh, Yh;      /* hi-res, 16.16 */
    const HiresVtx *v;
} HV;

static inline __attribute__((always_inline)) void
tri_impl(const HiresPrim *p, const HiresVtx *v0, const HiresVtx *v1, const HiresVtx *v2,
         const int TEX, const int DEPTH, const int RAW, const int SEMI,
         const int GOUR, const int DITHER)
{
    const int S = s_S;
    HV h0 = { v0->x, v0->y, (s64)v0->x * S, (s64)v0->y * S, v0 };
    HV h1 = { v1->x, v1->y, (s64)v1->x * S, (s64)v1->y * S, v1 };
    HV h2 = { v2->x, v2->y, (s64)v2->x * S, (s64)v2->y * S, v2 };
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
    u32 gstep_x[5] = { 0 }, gstep_y[5] = { 0 };   /* per hi-res pixel */
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
        gstep_x[i] = (u32)((s32)dx / S);
        gstep_y[i] = (u32)((s32)dy / S);
    }
    (void)gstep_y;
    const double ax = (double)a->X1 / 65536.0, ay = (double)a->Y1 / 65536.0;

    const bool long_left = denom > 0;
    const s64 long_step = floor_div128(((__int128)(c->Xh - a->Xh)) << 32, c->Yh - a->Yh);

    const int cx1 = imax(p->clip_x1 * S, s_ox);
    const int cx2 = imin((p->clip_x2 + 1) * S, s_ox + s_tw) - 1;
    const int cy1 = imax(p->clip_y1 * S, s_oy);
    const int cy2 = imin((p->clip_y2 + 1) * S, s_oy + s_th) - 1;

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
                const double P = ((double)xs + 0.5) / S - 0.5, Q = ((double)y + 0.5) / S - 0.5;
                u32 val[5];
                for (int i = 0; i < 5; i++) {
                    if ((i < 3 && !GOUR) || (i >= 3 && !TEX)) { val[i] = base[i]; continue; }
                    double d = (double)base[i] + gdx[i] * (P - ax) + gdy[i] * (Q - ay);
                    val[i] = (u32)(s64)floor(d);
                }
                u32 r = val[0], g = val[1], bb = val[2], u = val[3], v = val[4];
                u16 *dst = s_t + (size_t)(y - s_oy) * s_tw + (xs - s_ox);
                const int y1 = y / S;
                for (int x = xs; x < xe; x++, dst++) {
                    u16 out; bool blend;
                    if (!(p->mask_check && (*dst & 0x8000)) &&
                        shade(p, x / S, y1, r >> FRAC, g >> FRAC, bb >> FRAC, u >> FRAC, v >> FRAC,
                              TEX, DEPTH, RAW, SEMI, DITHER, &out, &blend)) {
                        if (blend) out = blend_px(p->blend, *dst, out);
                        *dst = out | p->mask_or;
                    }
                    if (GOUR) { r += gstep_x[0]; g += gstep_x[1]; bb += gstep_x[2]; }
                    if (TEX) { u += gstep_x[3]; v += gstep_x[4]; }
                }
            }
            lx += long_step;
            sx += short_step;
        }
    }
}

typedef void (*TriFn)(const HiresPrim *, const HiresVtx *, const HiresVtx *, const HiresVtx *);

#define TRI_FN(name, TEX, DEPTH, RAW, SEMI, GOUR, DITHER)                                  \
    static void name(const HiresPrim *p, const HiresVtx *a, const HiresVtx *b,             \
                     const HiresVtx *c)                                                    \
    { tri_impl(p, a, b, c, TEX, DEPTH, RAW, SEMI, GOUR, DITHER); }

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

static inline bool dither_on(const HiresPrim *p) { return p->dither && (s_S == 1 || s_dither_hi); }

static void be_tri(const HiresPrim *p, const HiresVtx v[3])
{
    if (!s_t) return;
    bool dith = dither_on(p);
    if (!p->tex) {
        int idx = (p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (dith ? 1 : 0);
        s_tri_untex[idx](p, &v[0], &v[1], &v[2]);
    } else {
        note_tex_page(p);
        int d = p->depth;
        if (p->raw) {
            s_tri_tex[d][p->semi ? 1 : 0](p, &v[0], &v[1], &v[2]);
        } else {
            int idx = 2 + ((p->semi ? 4 : 0) | (p->gour ? 2 : 0) | (dith ? 1 : 0));
            s_tri_tex[d][idx](p, &v[0], &v[1], &v[2]);
        }
    }
}

/* ---- rectangles -------------------------------------------------------------- */

static inline __attribute__((always_inline)) void
rect_impl(const HiresPrim *p, int x0, int y0, int w, int h, u32 r, u32 g, u32 b,
          u32 u0, u32 v0, bool flipx, bool flipy,
          const int TEX, const int DEPTH, const int RAW, const int SEMI)
{
    const int S = s_S;
    int xs = x0, xe = x0 + w, ys = y0, ye = y0 + h;
    if (xs < p->clip_x1) xs = p->clip_x1;
    if (xe > p->clip_x2 + 1) xe = p->clip_x2 + 1;
    if (ys < p->clip_y1) ys = p->clip_y1;
    if (ye > p->clip_y2 + 1) ye = p->clip_y2 + 1;
    xs = imax(xs, s_rx); xe = imin(xe, s_rx + s_rw);
    ys = imax(ys, s_ry); ye = imin(ye, s_ry + s_rh);
    if (xs >= xe || ys >= ye) return;
    int du = flipx ? -1 : 1, dv = flipy ? -1 : 1;
    if (TEX && flipx) u0 |= 1;
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
        for (int sy = 0; sy < S; sy++) {
            u16 *dst = s_t + (size_t)(y * S + sy - s_oy) * s_tw + (xs * S - s_ox);
            for (int i = 0; i < cnt; i++) {
                if (!vis[i]) { dst += S; continue; }
                for (int sx = 0; sx < S; sx++, dst++) store_px(p, dst, outs[i], vis[i] == 2);
            }
        }
    }
}

typedef void (*RectFn)(const HiresPrim *, int, int, int, int, u32, u32, u32, u32, u32, bool, bool);
#define RECT_FN(name, TEX, DEPTH, RAW, SEMI)                                                   \
    static void name(const HiresPrim *p, int x0, int y0, int w, int h, u32 r, u32 g, u32 b,   \
                     u32 u0, u32 v0, bool fx, bool fy)                                       \
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

static void be_rect(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g, u32 b,
                    u32 u0, u32 v0, bool fx, bool fy)
{
    if (!s_t) return;
    if (!p->tex) {
        (p->semi ? rect_u1 : rect_u0)(p, x, y, w, h, r, g, b, 0, 0, false, false);
    } else {
        note_tex_page(p);
        s_rect_tex[p->depth][(p->raw ? 2 : 0) | (p->semi ? 1 : 0)](p, x, y, w, h, r, g, b, u0, v0, fx, fy);
    }
}

/* ---- lines ----------------------------------------------------------------- */

static void be_line(const HiresPrim *p, int x0, int y0, u32 c0, int x1, int y1, u32 c1)
{
    if (!s_t) return;
    const int S = s_S;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    const bool gour = p->gour;
    if (!gour) c1 = c0;
    const bool dith = dither_on(p);
    const int k = adx > ady ? adx : ady;
    const bool xmajor = adx > ady;
    const int cx1 = imax(p->clip_x1 * S, s_ox), cx2 = imin((p->clip_x2 + 1) * S, s_ox + s_tw) - 1;
    const int cy1 = imax(p->clip_y1 * S, s_oy), cy2 = imin((p->clip_y2 + 1) * S, s_oy + s_th) - 1;

#define LINE_PLOT(PX, PY, CR, CG, CB)                                                          \
    do {                                                                                        \
        int px_ = (PX), py_ = (PY);                                                             \
        if (px_ >= cx1 && px_ <= cx2 && py_ >= cy1 && py_ <= cy2) {                             \
            u16 o_; bool bl_;                                                                   \
            u16 *d_ = s_t + (size_t)(py_ - s_oy) * s_tw + (px_ - s_ox);                         \
            if (!(p->mask_check && (*d_ & 0x8000)) &&                                           \
                shade(p, px_ / S, py_ / S, (CR), (CG), (CB), 0, 0, 0, 0, 0, p->semi, dith,      \
                      &o_, &bl_)) {                                                             \
                if (bl_) o_ = blend_px(p->blend, *d_, o_);                                      \
                *d_ = o_ | p->mask_or;                                                          \
            }                                                                                   \
        }                                                                                       \
    } while (0)

    if (k == 0) {
        for (int j = 0; j < S; j++)
            for (int i = 0; i < S; i++)
                LINE_PLOT(x0 * S + i, y0 * S + j, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF);
        return;
    }
    /* centre of the first 1x pixel in hi-res coordinates, 32.32; the line is extended by
     * half a 1x pixel at each end so that it covers whole 1x pixels like the 1x one. */
    s64 xf = (((s64)x0 * S) << 32) + ((s64)S << 31);
    s64 yf = (((s64)y0 * S) << 32) + ((s64)S << 31);
    s64 sx = ((s64)dx << 32) / k, sy = ((s64)dy << 32) / k;
    xf -= sx * (S - 1) / 2;
    yf -= sy * (S - 1) / 2;
    s32 r = (s32)((c0 & 0xFF) << 12), g = (s32)(((c0 >> 8) & 0xFF) << 12), b = (s32)(((c0 >> 16) & 0xFF) << 12);
    const int kh = k * S;
    s32 dr = (s32)(((s32)(c1 & 0xFF) - (s32)(c0 & 0xFF)) * 4096 / kh);
    s32 dg = (s32)(((s32)((c1 >> 8) & 0xFF) - (s32)((c0 >> 8) & 0xFF)) * 4096 / kh);
    s32 db = (s32)(((s32)((c1 >> 16) & 0xFF) - (s32)((c0 >> 16) & 0xFF)) * 4096 / kh);
    if (dx < 0) xf -= 1;
    if (dy < 0) yf -= 1;
    r += 1 << 11; g += 1 << 11; b += 1 << 11;
    const int steps = kh + (S - 1);
    for (int i = 0; i <= steps; i++) {
        int x = (int)(xf >> 32), y = (int)(yf >> 32);
        u32 cr = (u32)(r >> 12), cg = (u32)(g >> 12), cb = (u32)(b >> 12);
        if (xmajor) {
            for (int j = 0; j < S; j++) LINE_PLOT(x, y - S / 2 + j, cr, cg, cb);
        } else {
            for (int j = 0; j < S; j++) LINE_PLOT(x - S / 2 + j, y, cr, cg, cb);
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
    const int S = s_S;
    u16 *row0 = s_t + (size_t)(y * S - s_oy) * s_tw + (x0 * S - s_ox);
    const int n = (x1 - x0) * S;
    for (int i = 0; i < n; i++) row0[i] = c;
    for (int j = 1; j < S; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)n * sizeof(u16));
}

static void be_fill(int x, int y, int w, int h, u16 c)
{
    if (!s_t) return;
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
    const int S = s_S;
    const u16 val = v | (ms ? 0x8000 : 0);
    for (int sy = 0; sy < S; sy++) {
        u16 *d = s_t + (size_t)(y * S + sy - s_oy) * s_tw + (x * S - s_ox);
        for (int sx = 0; sx < S; sx++, d++) {
            if (mc && (*d & 0x8000)) continue;
            *d = val;
        }
    }
}

static void be_copy(int sx, int sy, int dx, int dy, int wd, int ht, bool mc, bool ms)
{
    if (!s_t) return;
    const int S = s_S;
    /* skip when the destination can't touch the region at all */
    bool any = false;
    for (int j = 0; j < ht && !any; j++) {
        int drow = (dy + j) & 511;
        if (drow >= s_ry && drow < s_ry + s_rh) any = true;
    }
    if (!any) return;
    static u16 buf[(size_t)HIRES_MAX_SCALE * VRAM_W * HIRES_MAX_SCALE];
    const size_t stride = (size_t)wd * S;
    for (int j = 0; j < ht; j++) {
        int srow = (sy + j) & 511, drow = (dy + j) & 511;
        if (drow < s_ry || drow >= s_ry + s_rh) continue;
        for (int i = 0; i < wd; i++) {
            int scol = (sx + i) & 1023;
            bool in = in_region(scol, srow);
            for (int a = 0; a < S; a++)
                for (int b = 0; b < S; b++)
                    buf[a * stride + (size_t)i * S + b] = in
                        ? s_t[(size_t)(srow * S + a - s_oy) * s_tw + (scol * S + b - s_ox)]
                        : s_vram[srow * VRAM_W + scol];
        }
        for (int i = 0; i < wd; i++) {
            int dcol = (dx + i) & 1023;
            if (!in_region(dcol, drow)) continue;
            for (int a = 0; a < S; a++) {
                u16 *d = s_t + (size_t)(drow * S + a - s_oy) * s_tw + (dcol * S - s_ox);
                for (int b = 0; b < S; b++, d++) {
                    if (mc && (*d & 0x8000)) continue;
                    *d = buf[a * stride + (size_t)i * S + b] | (ms ? 0x8000 : 0);
                }
            }
        }
    }
}

static void seed(void)
{
    const int S = s_S;
    for (int y = 0; y < s_rh; y++) {
        u16 *row0 = s_t + (size_t)y * S * s_tw;
        const u16 *src = &s_vram[(s_ry + y) * VRAM_W + s_rx];
        for (int x = 0; x < s_rw; x++)
            for (int i = 0; i < S; i++) row0[x * S + i] = src[x];
        for (int j = 1; j < S; j++) memcpy(row0 + (size_t)j * s_tw, row0, (size_t)s_tw * sizeof(u16));
    }
}

static void be_destroy(void)
{
    free(s_t);
    s_t = NULL;
    s_S = 0;
}

static bool be_configure(const u16 *vram1x, int scale, int rx, int ry, int rw, int rh)
{
    if (scale < 1 || scale > HIRES_MAX_SCALE || rw <= 0 || rh <= 0 ||
        rx < 0 || ry < 0 || rx + rw > VRAM_W || ry + rh > VRAM_H)
        return false;
    u16 *nt = malloc((size_t)rw * scale * rh * scale * sizeof(u16));
    if (!nt) return false;
    free(s_t);
    s_t = nt;
    s_vram = vram1x;
    s_S = scale;
    s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
    s_tw = rw * scale; s_th = rh * scale;
    s_ox = rx * scale; s_oy = ry * scale;
    memset(s_fb_tex_seen, 0, sizeof s_fb_tex_seen);
    seed();
    return true;
}

static bool be_view(int x, int y, int w, int h, HiresView *out)
{
    if (!s_t || w <= 0 || h <= 0) return false;
    if (x < s_rx || y < s_ry || x + w > s_rx + s_rw || y + h > s_ry + s_rh) return false;
    out->pixels = s_t + (size_t)(y * s_S - s_oy) * s_tw + (x * s_S - s_ox);
    out->stride = s_tw;
    out->w = w * s_S;
    out->h = h * s_S;
    out->scale = s_S;
    return true;
}

static const HiresBackend s_soft = {
    "software", be_configure, be_tri, be_rect, be_line, be_fill, be_copy, be_put, be_view, be_destroy,
};

/* ---- control --------------------------------------------------------------------- */

bool hires_set_scale(int scale)
{
    if (scale < 0) scale = 0;
    if (scale > HIRES_MAX_SCALE) scale = HIRES_MAX_SCALE;
    if (scale == 0) {
        if (g_hires) g_hires->destroy();
        g_hires = NULL;
        return true;
    }
    if (g_hires && s_S == scale && s_rx == s_cfg_rx && s_ry == s_cfg_ry &&
        s_rw == s_cfg_rw && s_rh == s_cfg_rh)
        return true;
    if (!s_soft.configure(gpu_vram(), scale, s_cfg_rx, s_cfg_ry, s_cfg_rw, s_cfg_rh)) {
        g_hires = NULL;
        return false;
    }
    g_hires = &s_soft;
    return true;
}

int hires_scale(void) { return g_hires ? s_S : 0; }

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

int hires_pick_scale(int pw, int ph, int max_scale)
{
    if (max_scale < 1) max_scale = 1;
    if (max_scale > HIRES_MAX_SCALE) max_scale = HIRES_MAX_SCALE;
    if (pw < 1 || ph < 1) return 1;
    int vw, vh;                              /* 4:3 letterbox viewport */
    if ((s64)pw * 3 > (s64)ph * 4) { vh = ph; vw = ph * 4 / 3; }
    else { vw = pw; vh = pw * 3 / 4; }
    /* corrected target size at scale S is 512*S x 384*S */
    int s = imax((vw + 511) / 512, (vh + 383) / 384);
    return s < 1 ? 1 : (s > max_scale ? max_scale : s);
}

u32 hires_verify_1x(int *first_x, int *first_y)
{
    if (!g_hires || s_S != 1) return 0;
    u32 bad = 0;
    for (int y = 0; y < s_rh; y++)
        for (int x = 0; x < s_rw; x++)
            if (s_t[y * s_tw + x] != s_vram[(s_ry + y) * VRAM_W + s_rx + x]) {
                if (!bad) { if (first_x) *first_x = s_rx + x; if (first_y) *first_y = s_ry + y; }
                bad++;
            }
    return bad;
}
