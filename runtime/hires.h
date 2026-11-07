/* High-resolution render target (presentation only).
 *
 * gpu.c remains the authoritative 1x GPU: every GP0 command still executes on
 * the 1x VRAM and all CPU-visible state comes from there. In addition, each
 * drawing command is forwarded here, to a *separate* target that is rasterized
 * at an integer multiple S of the 1x resolution. The target is never read by
 * the emulated machine (textures/CLUTs are fetched from the 1x VRAM).
 *
 * The renderer sits behind HiresBackend (command in -> target out) so a GPU
 * (Metal/SDL_GPU) backend can replace the software one later. Vertices carry
 * 16.16 fixed-point positions (in 1x pixel units) so sub-pixel inputs (e.g.
 * PGXP-style GTE precision) can be supplied later; today they are integers.
 */
#ifndef HIRES_H
#define HIRES_H

#include "psx.h"

#define HIRES_MAX_SCALE 8

typedef struct {
    s32 x, y;         /* 1x pixel units, 16.16 fixed point; drawing offset already applied */
    u32 r, g, b;      /* 0..255 */
    u32 u, v;         /* 0..255 */
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
} HiresPrim;

/* A rectangle of the render target, as presented. */
typedef struct {
    const u16 *pixels;   /* 15-bit PS1 pixels (bit 15 = mask bit) */
    int stride;          /* in pixels */
    int w, h;            /* in target pixels */
    int scale_x, scale_y;
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
    void (*destroy)(void);
} HiresBackend;

/* Active backend; NULL = hi-res off. Defined in gpu.c, set by hires_set_scale(). */
extern const HiresBackend *g_hires;

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
/* Presentable view of the current display area, or false (24-bit display, display
 * not inside the target, hi-res off): present from 1x VRAM then. */
bool hires_get_view(const GpuDisplay *d, HiresView *v);
/* Scales for a window of pw x ph physical pixels showing a dw x dh display: the 4:3 letterbox
 * viewport (vw x vh) is covered without ever upsampling on either axis,
 * sx = ceil(vw/dw), sy = ceil(vh/dh), each capped to 1..max_x / 1..max_y. */
void hires_pick_scale(int pw, int ph, int dw, int dh, int max_x, int max_y, int *sx, int *sy);
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
 * outside the triangle's vertex UV range and were clamped; and those that still were outside
 * the range when fetched (must always be 0). */
typedef struct { u64 samples, clamped, escaped; } HiresUvStats;
void hires_get_uv_stats(HiresUvStats *st);

/* Compare the target against the 1x VRAM region (meaningful at scale 1x1, where they must
 * be identical). Returns the number of differing pixels; reports the first one. */
u32 hires_verify_1x(int *first_x, int *first_y);

#endif
