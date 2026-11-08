/* High-resolution render target (presentation only).
 *
 * gpu.c remains the authoritative 1x GPU: every GP0 command still executes on
 * the 1x VRAM and all CPU-visible state comes from there. In addition, each
 * drawing command is forwarded here, to a *separate* target that is rasterized
 * at an integer multiple S of the 1x resolution. The target is never read by
 * the emulated machine (textures/CLUTs are fetched from the 1x VRAM).
 *
 * The renderer sits behind HiresBackend (command in -> target out): the software
 * rasterizer (hires.c, the reference and default) or the SDL_GPU backend (hires_gpu.c).
 * Vertices carry 16.16 fixed-point positions (in 1x pixel units): integers, or sub-pixel
 * positions from the GTE when PGXP tracking (runtime/pgxp.h) matched the vertex, in which
 * case `w` (view depth) is also set for perspective-correct texturing.
 */
#ifndef HIRES_H
#define HIRES_H

#include "psx.h"

#define HIRES_MAX_SCALE 8

typedef struct {
    s32 x, y;         /* 1x pixel units, 16.16 fixed point; drawing offset already applied */
    u32 r, g, b;      /* 0..255 */
    u32 u, v;         /* 0..255 */
    /* PGXP: view depth (> 0) when x/y are precise GTE values, else 0. A backend texturing a
     * triangle whose three vertices all have w > 0 may interpolate u/v perspective-correctly
     * (u/w, v/w, 1/w); otherwise affine, as the PS1 does. Backends may ignore it. */
    float w;
} HiresVtx;

typedef struct {
    bool tex, raw, semi, gour, dither;
    int blend;                        /* semi-transparency mode 0..3 */
    int depth;                        /* 0=4bpp 1=8bpp 2=15bpp */
    u32 tp_x, tp_y, clut_x, clut_y;   /* texpage / CLUT origin in 1x VRAM */
    u32 tw_and_u, tw_or_u, tw_and_v, tw_or_v;
    u16 mask_or;
    bool mask_check;
    int clip_x1, clip_y1, clip_x2, clip_y2;   /* draw area, inclusive, 1x pixels */
    /* triangles: u/v range the 1x rasterizer actually sampled (pre texture window); if the 1x
     * draw sampled nothing, has_uvr is false and the vertex UV range is used */
    bool has_uvr;
    int uvr_umin, uvr_umax, uvr_vmin, uvr_vmax;
    /* gouraud triangles: per-channel (r, g, b) colour range the 1x rasterizer actually produced
     * (its 8-bit values, which wrap on thin slivers); if the 1x draw covered nothing, has_cr is
     * false and the vertex colour range is used */
    bool has_cr;
    int cr_min[3], cr_max[3];
} HiresPrim;

/* A rectangle of the render target, as presented. */
typedef struct {
    const u16 *pixels;   /* 15-bit PS1 pixels (bit 15 = mask bit) */
    int stride;          /* in pixels */
    int w, h;            /* in target pixels */
    int scale_x, scale_y;
    /* GPU-resident view (GPU backend): pixels is NULL; gpu is the backend's target and
     * (gx, gy) the view's top-left pixel in it. hires_read_view() gives CPU pixels. */
    void *gpu;
    int gx, gy;
} HiresView;

typedef struct HiresBackend {
    const char *name;
    /* (Re)allocate the target for scales sx (horizontal) x sy (vertical) over the 1x VRAM rectangle (rx,ry,rw,rh)
     * and seed it from `vram1x` by nearest-neighbour upscale. */
    bool (*configure)(const u16 *vram1x, int sx, int sy, int rx, int ry, int rw, int rh);
    void (*tri)(const HiresPrim *p, const HiresVtx v[3]);
    void (*rect)(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g, u32 b,
                 u32 u0, u32 v0, bool flipx, bool flipy);
    void (*line)(const HiresPrim *p, int x0, int y0, u32 c0, int x1, int y1, u32 c1);
    void (*fill)(int x, int y, int w, int h, u16 c);
    void (*copy)(int sx, int sy, int dx, int dy, int w, int h, bool mask_check, bool mask_set);
    void (*put)(int x, int y, u16 v, bool mask_check, bool mask_set);   /* CPU->VRAM pixel */
    bool (*view)(int x, int y, int w, int h, HiresView *out);          /* 1x rect -> target */
    bool (*read)(int x, int y, int w, int h, HiresView *out);          /* same, CPU pixels (readback) */
    void (*destroy)(void);
    /* Optional (NULL = unsupported; widescreen then stays 4:3). Guard band: gx virtual 1x
     * columns on each side of the region, taking effect at the next configure(). They are
     * not VRAM: only tri/rect/line draw there, never fill/copy/put, except that a fill row
     * covering the whole region width also fills them (screen clears). A primitive whose draw
     * area starts at the region's left edge (ends at its right edge) is clipped gx columns
     * further out on that side, and an untextured rect covering the whole draw-area width is
     * widened across the guard band (fades). view() then accepts x in [rx - gx, rx + rw + gx).
     * The 1x VRAM, and so all emulated state, never sees any of it. */
    void (*set_guard)(int gx);
} HiresBackend;

/* Active backend; NULL = hi-res off. Defined in gpu.c, set by hires_set_scale(). */
extern const HiresBackend *g_hires;
/* Guard band columns of the active target (0 = none). Defined in gpu.c, set by hires.c. */
extern int g_hires_guard;
/* True while gpu.c forwards a presentation-only (guard-band) packet: it was not drawn into the
 * 1x VRAM, so a backend must not treat its area as changed VRAM. Defined in gpu.c. */
extern bool g_hires_shadow;

/* Backend selection (takes effect on the next hires_set_scale that allocates). be = NULL:
 * software. shadow != NULL: every command is also fed to shadow (same scales), for
 * hires_compare(); views and presentation come from be. */
void hires_set_backend(const HiresBackend *be, const HiresBackend *shadow);
const HiresBackend *hires_software_backend(void);
bool hires_get_dither(void);                  /* --hires-dither setting */

/* ---- control (runtime/hires.c) ------------------------------------------- */

/* Set the render scales: sx = horizontal, sy = vertical (independent integers 1..HIRES_MAX_SCALE);
 * sx = 0 turns hi-res off. A changed scale reallocates the target and seeds it from the
 * current 1x VRAM. Never touches emulated state. */
bool hires_set_scale(int sx, int sy);
bool hires_get_scale(int *sx, int *sy);   /* false when off */
/* 1x VRAM rectangle the target tracks (default 0,0,512,480). Takes effect on the
 * next hires_set_scale(). */
void hires_set_region(int rx, int ry, int rw, int rh);
void hires_set_dither(bool enable);           /* dither at scale > 1 (default off) */
/* Widescreen guard band (see HiresBackend.set_guard): gx 1x columns left and right of the
 * region (0 = off, the default; max HIRES_MAX_GUARD). Takes effect on the next
 * hires_set_scale() (forces a reallocation). hires_guard() = the guard band in use. */
#define HIRES_MAX_GUARD 256
void hires_set_guard(int gx);
int hires_guard(void);
/* Presentable view of the current display area, or false (24-bit display, display
 * not inside the target, hi-res off): present from 1x VRAM then. */
bool hires_get_view(const GpuDisplay *d, HiresView *v);
/* Same, widened by `extra` guard-band columns on each side (clamped to the guard band). */
bool hires_get_view_wide(const GpuDisplay *d, int extra, HiresView *v);
bool hires_read_view(const GpuDisplay *d, HiresView *v);   /* same with CPU pixels (GPU: readback) */
bool hires_read_view_wide(const GpuDisplay *d, int extra, HiresView *v);
/* Scales for a window of pw x ph physical pixels showing a dw x dh display: the 4:3 letterbox
 * viewport (vw x vh) is covered without ever upsampling on either axis,
 * sx = ceil(vw/dw), sy = ceil(vh/dh), each capped to 1..max_x / 1..max_y. */
void hires_pick_scale(int pw, int ph, int dw, int dh, int max_x, int max_y, int *sx, int *sy);
/* Same for content of aspect ar (width / height as presented, 4:3 = 1.333...). */
void hires_pick_scale_ar(int pw, int ph, int dw, int dh, double ar, int max_x, int max_y, int *sx, int *sy);
/* Worker pool (hi-res path only; 1x GPU and emulated state stay single-threaded). Large
 * primitives are split into row chunks and rasterized fork-join, so command order is kept.
 * n = worker threads besides the calling one; -1 = auto (performance cores - 1); 0 = serial. */
void hires_set_threads(int n);
int hires_threads(void);
void hires_set_parallel_threshold(int px);   /* bbox pixels below which a primitive runs inline */
/* Run fn(ctx, y0, y1) over row chunks of [0, rows) on the pool; fn must clip y1 to rows. */
void hires_parallel_rows(void (*fn)(void *ctx, int y0, int y1), void *ctx, int rows);

/* Presentation helpers: the target changes only when something was drawn. */
u64 hires_epoch(void);                       /* bumped whenever the target is (re)allocated */
bool hires_dirty_rows(int y0, int y1);       /* drawn into 1x rows [y0, y1] since the last clear? */
void hires_clear_dirty(void);

/* UV sampling diagnostics: textured hi-res pixels sampled; those whose interpolated u/v fell
 * outside the UV range the 1x draw of that triangle sampled (or, if it sampled nothing, the
 * vertex range) and were clamped; and those that still were outside it when fetched (must
 * always be 0). tris / tris_1x_range: textured triangles drawn / using the 1x-sampled range. */
typedef struct { u64 samples, clamped, escaped, tris, tris_1x_range; } HiresUvStats;
void hires_get_uv_stats(HiresUvStats *st);

/* Perspective-correct texturing of triangles whose vertices all carry w (default on; with it
 * off, w is ignored and texturing is affine). */
void hires_set_perspective(bool enable);
bool hires_get_perspective(void);            /* for other backends: same setting */
/* Triangles drawn, and those textured perspective-correctly (software backend; the GPU
 * backend counts its own in HiresGpuStats). */
void hires_get_persp_stats(u64 *tris, u64 *persp);

/* Compare the target against the 1x VRAM region (meaningful at scale 1x1, where they must
 * be identical). Returns the number of differing pixels; reports the first one. */
u32 hires_verify_1x(int *first_x, int *first_y);

/* Compare the primary backend's target against the shadow's over the whole region (and the
 * guard band, if any).
 * diff = pixels whose 15-bit value differs (mask bit ignored), diff_big = those where some
 * channel differs by more than 1 (of 31); mask_diff = pixels where only bit 15 differs. */
typedef struct { u64 pixels, diff, diff_big, mask_diff; int first_x, first_y; } HiresCompare;
bool hires_compare(HiresCompare *c);

#endif
