/* The SDL_GPU backend (runtime/hires_gpu.c) against the software rasterizer (hires.c), without
 * the game: random GP0 command streams (triangles and quads: flat/gouraud/textured 4/8/15 bit,
 * raw, semi-transparent modes 1-3, texture windows, dithering; rects; lines; fills; CPU->VRAM
 * uploads; VRAM copies) go through gpu.c in compare mode (software primary, GPU shadow) and
 * hires_compare() must find 0 differing pixels at several scales, with and without the
 * widescreen guard band. A second pass sends PGXP triangles (sub-pixel positions, depth) through
 * gpu_shadow_packet: exact with affine texturing, within a small budget with perspective (the
 * software backend uses double, the shader 2^-38 fixed point; see AGENTS.md "PGXP").
 *
 * `fast [API]` runs the same streams with the GPU backend in fast precision (hardware
 * rasterization and interpolation, hires_gpu.c "Fast precision"): not exact, so it checks a
 * generous tolerance instead (pixels off by more than 4 15-bit steps, mean abs error).
 *
 * Needs an SDL_GPU device (Metal, or Vulkan: real hardware, MoltenVK, or Mesa lavapipe on CI);
 * exits 77 (CTest "skipped") when there is none. argv[1] = auto|metal|vulkan|d3d12 (one API), or
 * switch: shut down and restart the backend on every available API in turn, comparing each time.
 * Build (macOS): clang -std=c11 -O2 -Iruntime runtime/gpu.c runtime/hires.c runtime/hires_gpu.c
 *   runtime/pgxp.c runtime/platform.c runtime/tests/test_hires_gpu.c -lSDL3 -o build/tests/test_hires_gpu
 */
#include "psx.h"
#include "hires.h"
#include "hires_gpu.h"
#include "pgxp.h"
#include "hlr/hlr_gpu.h"
#include "hlr/hlr_cam.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void irq_raise(int bit) { (void)bit; }
static bool s_verbose;
void rt_log(const char *fmt, ...)
{
    if (!s_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
               printf(__VA_ARGS__); printf("\n"); }                   \
    } while (0)

static u32 s_rng = 12345;
static u32 rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return s_rng >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (u32)(hi - lo + 1)); }

static void gp0(u32 v) { gpu_write_gp0(v); }
static u32 xy(int x, int y) { return ((u32)(y & 0x7FF) << 16) | (u32)(x & 0x7FF); }
static u32 rgb(void) { return (u32)rnd() & 0xFFFFFF; }

/* texture pages live right of the display region (x >= 512), CLUTs at y = 480.. */
static u32 tpage(int depth, int semi_mode)
{
    int tx = rr(8, 15), ty = rr(0, 1);   /* 64-px units: x 512..960, y 0 / 256 */
    return (u32)tx | ((u32)ty << 4) | ((u32)semi_mode << 5) | ((u32)depth << 7);
}
static u32 clut(void) { return (u32)(rr(0, 31) * 16 / 16) | ((u32)rr(480, 511) << 6); }

static void upload(int x, int y, int w, int h, bool noise_zero)
{
    gp0(0xA0000000);
    gp0(xy(x, y));
    gp0(((u32)h << 16) | (u32)w);
    int n = (w * h + 1) / 2;
    for (int i = 0; i < n; i++) {
        u32 a = rnd() & 0xFFFF, b = rnd() & 0xFFFF;
        if (noise_zero && (rnd() & 15) == 0) a = 0;   /* transparent texels */
        if ((rnd() & 3) == 0) a &= 0x7FFF;            /* mixed semi-transparency bits */
        gp0(a | (b << 16));
    }
}

static void env(void)
{
    gp0(0xE3000000 | xy(0, 0));                           /* draw area 0,0 - 511,479 */
    gp0(0xE4000000 | (479u << 10) | 511u);
    gp0(0xE5000000);
    gp0(0xE6000000);
}

static int vx(void) { return rr(-120, 630); }
static int vy(void) { return rr(-100, 580); }

static void poly(bool small)
{
    bool quad = rnd() & 1, gour = rnd() & 1, tex = (rnd() % 3) != 0, semi = (rnd() & 3) == 0;
    bool raw = tex && (rnd() & 3) == 0;
    int depth = rr(0, 2), mode = rr(1, 3);   /* mode 0 (B/2+F/2) has a documented tie case */
    u32 cmd = 0x20u | (quad ? 0x08u : 0) | (gour ? 0x10u : 0) | (tex ? 0x04u : 0) | (semi ? 0x02u : 0) | (raw ? 0x01u : 0);
    int n = quad ? 4 : 3;
    int cx = vx(), cy = vy(), sz = small ? 24 : 200;
    gp0((cmd << 24) | rgb());
    for (int i = 0; i < n; i++) {
        if (i && gour) gp0(rgb());
        gp0(xy(cx + rr(-sz, sz), cy + rr(-sz, sz)));
        if (tex) {
            u32 uv = (u32)rr(0, 255) | ((u32)rr(0, 255) << 8);
            if (i == 0) uv |= clut() << 16;
            if (i == 1) uv |= tpage(depth, mode) << 16;
            gp0(uv);
        }
    }
}

/* a 2D sprite quad: screen-aligned, u/v advancing exactly one texel per pixel (hires_tri_is_2d),
 * so the xbr filter takes its 2D path on polygons too */
static void poly2d(void)
{
    bool semi = (rnd() & 3) == 0, raw = rnd() & 1;
    int depth = rr(0, 2), w = rr(4, 90), h = rr(4, 90), x = rr(-60, 520), y = rr(-60, 470);
    int u0 = rr(0, 255 - w), v0 = rr(0, 255 - h);
    gp0(((0x2Cu | (semi ? 0x02u : 0) | (raw ? 0x01u : 0)) << 24) | rgb());
    gp0(xy(x, y));         gp0((u32)u0 | ((u32)v0 << 8) | (clut() << 16));
    gp0(xy(x + w, y));     gp0((u32)(u0 + w) | ((u32)v0 << 8) | (tpage(depth, rr(1, 3)) << 16));
    gp0(xy(x, y + h));     gp0((u32)u0 | ((u32)(v0 + h) << 8));
    gp0(xy(x + w, y + h)); gp0((u32)(u0 + w) | ((u32)(v0 + h) << 8));
}

static void rect(void)
{
    bool tex = rnd() & 1, semi = (rnd() & 3) == 0, raw = tex && (rnd() & 1);
    int size = rr(0, 3);   /* variable, 1x1, 8x8, 16x16 */
    u32 cmd = 0x60u | ((u32)size << 3) | (tex ? 0x04u : 0) | (semi ? 0x02u : 0) | (raw ? 0x01u : 0);
    if (tex) gp0(0xE1000000 | tpage(rr(0, 2), rr(1, 3)) | (rnd() & 1 ? 0x1000u : 0) | (rnd() & 1 ? 0x2000u : 0));
    gp0((cmd << 24) | rgb());
    gp0(xy(rr(-40, 540), rr(-40, 500)));
    if (tex) gp0((u32)rr(0, 255) | ((u32)rr(0, 255) << 8) | (clut() << 16));
    if (size == 0) gp0(((u32)rr(1, 120) << 16) | (u32)rr(1, 160));
}

static void line(void)
{
    bool gour = rnd() & 1, semi = (rnd() & 3) == 0;
    gp0(((0x40u | (gour ? 0x10u : 0) | (semi ? 0x02u : 0)) << 24) | rgb());
    gp0(xy(rr(-20, 530), rr(-20, 500)));
    if (gour) gp0(rgb());
    gp0(xy(rr(-20, 530), rr(-20, 500)));
}

static void misc(void)
{
    switch (rr(0, 5)) {
    case 0:   /* fill (16-px aligned x, as the hardware) */
        gp0(0x02000000 | rgb());
        gp0(xy(rr(0, 31) * 16, rr(0, 479)));
        gp0(((u32)rr(1, 200) << 16) | (u32)(rr(1, 20) * 16));
        break;
    case 1: {   /* VRAM -> VRAM copy, inside / into / out of the region */
        gp0(0x80000000);
        gp0(xy(rr(0, 1000), rr(0, 500)));
        gp0(xy(rr(0, 600), rr(0, 500)));
        gp0(((u32)rr(1, 64) << 16) | (u32)rr(1, 64));
        break;
    }
    case 2: upload(rr(0, 500), rr(0, 470), rr(1, 40), rr(1, 10), false); break;   /* into the region */
    case 3: upload(rr(512, 1000), rr(0, 500), rr(1, 32), rr(1, 8), true); break;   /* texture change */
    case 4: gp0(0xE2000000 | (u32)rr(0, 0xFFFFF)); break;                         /* texture window */
    default: gp0(0xE1000000 | tpage(0, 1) | (rnd() & 1 ? 0x200u : 0)); break;      /* dither on/off */
    }
}

static void compare(const char *what, u64 max_diff)
{
    HiresCompare c;
    bool ok = hires_compare(&c);
    CHECK(ok, "%s: compare failed (no shadow target?)", what);
    if (!ok) return;
    CHECK(c.diff <= max_diff && c.mask_diff == 0, "%s: %llu of %llu pixels differ (%llu by >1 step, %llu mask), first at %d,%d",
          what, (unsigned long long)c.diff, (unsigned long long)c.pixels, (unsigned long long)c.diff_big,
          (unsigned long long)c.mask_diff, c.first_x, c.first_y);
    printf("  %-34s %8llu px compared, %llu differ\n", what, (unsigned long long)c.pixels, (unsigned long long)c.diff);
}

/* fast precision: limits per compare (see run_fast) */
static bool s_fast_mode;
static double s_fast_big4_max = 3.0, s_fast_mae_max = 3.0;   /* % of pixels > 4 steps; of 255 per channel */
static double s_fast_worst_big4, s_fast_worst_mae;
static void compare_tol(const char *what)
{
    HiresCompare c;
    bool ok = hires_compare(&c);
    CHECK(ok, "%s: compare failed (no shadow target?)", what);
    if (!ok || !c.pixels) return;
    const double big4 = 100.0 * (double)c.diff_big4 / (double)c.pixels, mae = (double)c.sum_abs / (3.0 * (double)c.pixels);
    if (big4 > s_fast_worst_big4) s_fast_worst_big4 = big4;
    if (mae > s_fast_worst_mae) s_fast_worst_mae = mae;
    CHECK(big4 <= s_fast_big4_max && mae <= s_fast_mae_max,
          "%s: fast precision out of tolerance: %.3f%% of pixels off by > 4 steps (max %.2f), mean abs error %.3f (max %.2f)",
          what, big4, s_fast_big4_max, mae, s_fast_mae_max);
    printf("  %-34s %8llu px, %6.3f%% differ, > 1 step %.3f%%, > 4 steps %.3f%%, MAE %.3f\n", what,
           (unsigned long long)c.pixels, 100.0 * (double)c.diff / (double)c.pixels,
           100.0 * (double)c.diff_big / (double)c.pixels, big4, mae);
}

static bool setup(int sx, int sy, int guard)
{
    gpu_init();
    gpu_enable_display_copy();   /* full 1x picture, as the windowed 15-bit fallback has */
    g_pgxp = false;
    hires_set_region(0, 0, 512, 480);
    hires_set_backend(NULL, hires_gpu_backend());
    hires_set_scale(0, 0);
    hires_set_guard(guard);
    if (!hires_set_scale(sx, sy)) return false;
    env();
    /* textures, CLUTs and some framebuffer content */
    upload(512, 0, 512, 512, true);
    upload(0, 480, 512, 32, false);
    upload(0, 0, 512, 480, false);
    return true;
}

/* render-quality modes: texture filter, full-precision colour, SSAA factor */
typedef struct { int filter; bool full; int ssaa; const char *name; } Mode;
static const Mode MODE_DEFAULT = { HIRES_TEX_NEAREST, false, 1, "" };
static Mode s_mode = { HIRES_TEX_NEAREST, false, 1, "" };

/* "external" mode (CTest hlr_gpu): an empty HiresBackend.external pass every N primitives, as
 * the HLR inserts one where the walk enters a captured scene table; it splits groups and render
 * passes but must leave the target exactly as before */
static int s_external_every, s_external_calls;
static void external_noop(void *ctx, void *cb, void *tex, int w, int h)
{
    (void)ctx; (void)cb; (void)w; (void)h;
    if (tex) s_external_calls++;
}


/* "external" mode, part 2 (CTest hlr_gpu): the native stage pass itself (hlr_gpu.c). A cube of six
 * flat-coloured faces at a known camera, drawn through hlr_gpu_external into a black 2x2 target,
 * the back faces last: every visible face's centre must hold its colour exactly, the back faces'
 * colours must not appear anywhere (depth test), the corners stay black; with MSAA the silhouette
 * has resolved in-between colours, without it none. On the 15-bit target PS1 red is in the low
 * five bits (the B5G5R5A1 channel swap). */
static const u8 CUBE_COL[6][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 } };

static void cube_rot(const float in[3], float o[3])
{
    const float a = 0.6f, b = 0.45f;   /* yaw, then pitch: three faces towards the camera */
    const float x = cosf(a) * in[0] + sinf(a) * in[2], z = -sinf(a) * in[0] + cosf(a) * in[2];
    o[0] = x;
    o[1] = cosf(b) * in[1] - sinf(b) * z;
    o[2] = sinf(b) * in[1] + cosf(b) * z;
}

static void run_cube(int msaa, bool full)
{
    char what[64];
    snprintf(what, sizeof what, "cube %s, MSAA %d", full ? "RGBA8" : "15-bit", msaa);
    hires_set_tex_filter(HIRES_TEX_NEAREST);
    hires_set_colour(full);
    hires_set_ssaa(1);
    gpu_init();
    g_pgxp = false;
    hires_set_region(0, 0, 512, 480);
    hires_set_backend(NULL, hires_gpu_backend());
    hires_set_scale(0, 0);
    hires_set_guard(0);
    if (!hires_set_scale(2, 2)) { CHECK(0, "%s: cannot allocate", what); return; }
    env();
    gp0(0x02000000); gp0(0); gp0((240u << 16) | 512u);   /* black screen */
    HiresGpuInfo gi;
    CHECK(hires_gpu_info(&gi), "%s: no target info", what);
    hlr_gpu_set_msaa(msaa);
    /* camera: identity rotation, eye 1600 in front of the cube, H 300, drawing offset (256, 120) */
    GteState gs;
    memset(&gs, 0, sizeof gs);
    gs.c[0] = 0x1000; gs.c[2] = 0x1000; gs.c[4] = 0x1000;
    gs.c[7] = 1600; gs.c[26] = 300;
    HlrCam k;
    hlr_cam_from_gte(&k, &gs);
    HlrGpuFrame *f = hlr_gpu_frame_new();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) f->u.rt[r][c] = k.rt[r][c];
    for (int c = 0; c < 3; c++) f->u.tr[c] = k.tr[c];
    f->px0 = 0; f->py0 = 0; f->pw = 1024; f->ph = 480;
    const int rect[4] = { f->px0, f->py0, f->pw, f->ph };
    hlr_cam_clip(&k, 256, 120, (float)gi.Sx, (float)gi.Sy, (float)gi.ox, (float)gi.oy, rect, f->u.proj);
    f->u.misc[0] = 16.0f; f->u.misc[1] = 1.0f; f->u.misc[2] = full ? 0.0f : 1.0f;
    f->samples = hlr_gpu_msaa_for(gi.target_format);
    /* faces: +x -x +y -y +z -z; corners of a 600-unit cube */
    static const int axis[6] = { 0, 0, 1, 1, 2, 2 };
    float centre[6][3];
    bool front[6];
    int order[6], nf = 0, nb = 0, back[6];
    for (int i = 0; i < 6; i++) {
        float n0[3] = { 0, 0, 0 }, n[3];
        n0[axis[i]] = i & 1 ? -1.0f : 1.0f;
        cube_rot(n0, n);
        float c0[3] = { n0[0] * 300, n0[1] * 300, n0[2] * 300 }, view[3];
        cube_rot(c0, centre[i]);
        hlr_cam_view(&k, centre[i], view);
        front[i] = n[0] * view[0] + n[1] * view[1] + n[2] * view[2] < 0;   /* faces the eye */
        if (front[i]) order[nf++] = i; else back[nb++] = i;
    }
    for (int i = 0; i < nb; i++) order[nf + i] = back[i];
    CHECK(nf == 3, "%s: %d faces towards the camera, expected 3", what, nf);
    HlrVtx *v = hlr_gpu_frame_vtx(f, 36);
    for (int fi = 0; fi < 6; fi++) {
        const int i = order[fi], a = axis[i], b = (a + 1) % 3, c = (a + 2) % 3;
        float q[4][3];
        for (int j = 0; j < 4; j++) {
            float p0[3];
            p0[a] = i & 1 ? -300.0f : 300.0f;
            p0[b] = (j & 1) ? 300.0f : -300.0f;
            p0[c] = (j & 2) ? 300.0f : -300.0f;
            cube_rot(p0, q[j]);
        }
        static const int tri[6] = { 0, 1, 2, 1, 3, 2 };
        for (int j = 0; j < 6; j++) {
            HlrVtx *o = &v[fi * 6 + j];
            memset(o, 0, sizeof *o);
            memcpy(o->pos, q[tri[j]], sizeof o->pos);
            o->col[0] = CUBE_COL[i][0]; o->col[1] = CUBE_COL[i][1]; o->col[2] = CUBE_COL[i][2];
            o->info = 0;   /* flat (HLR_SH_FLAT), untextured */
        }
    }
    HlrGpuPass *ps = calloc(1, sizeof *ps);
    ps->f = f; f->refs++;
    ps->d = calloc(1, sizeof *ps->d); ps->nd = 1;
    ps->d[0] = (HlrDraw){ 0, 36, -1 };
    hires_gpu_backend()->external(hlr_gpu_external, ps);
    HiresView hv;
    const bool rd = hires_gpu_backend()->read(0, 0, 512, 240, &hv);
    CHECK(rd, "%s: read back failed", what);
    if (!rd) { hlr_gpu_frame_unref(f); return; }
    HlrGpuStats st;
    hlr_gpu_get_stats(&st);
    CHECK(st.failed == 0, "%s: %llu passes failed", what, (unsigned long long)st.failed);
    #define PIX(x, y) (full ? hv.px32[(size_t)(y) * (size_t)hv.stride + (size_t)(x)] : (u32)hv.pixels[(size_t)(y) * (size_t)hv.stride + (size_t)(x)])
    /* the colour a face colour becomes in this target (15-bit: PS1 red in the low 5 bits) */
    u32 want[7];
    for (int i = 0; i < 6; i++)
        want[i] = full ? (u32)CUBE_COL[i][0] | (u32)CUBE_COL[i][1] << 8 | (u32)CUBE_COL[i][2] << 16
                       : (u32)(CUBE_COL[i][0] >> 3) | (u32)(CUBE_COL[i][1] >> 3) << 5 | (u32)(CUBE_COL[i][2] >> 3) << 10;
    want[6] = 0;
    for (int i = 0; i < 6; i++) {
        if (!front[i]) continue;
        float view[3], sx, sy;
        hlr_cam_view(&k, centre[i], view);
        hlr_cam_project(&k, view, &sx, &sy);
        const int x = (int)floorf((256.0f + sx) * (float)gi.Sx), y = (int)floorf((120.0f + sy) * (float)gi.Sy);
        const u32 got = PIX(x, y) & (full ? 0xffffffu : 0x7fffu);
        CHECK(got == want[i], "%s: face %d centre (%d,%d) = %06x, want %06x", what, i, x, y, got, want[i]);
    }
    u64 hidden = 0, mixed = 0, lit = 0;
    for (int y = 0; y < hv.h; y++)
        for (int x = 0; x < hv.w; x++) {
            const u32 p = PIX(x, y) & (full ? 0xffffffu : 0x7fffu);
            int m = -1;
            for (int i = 0; i < 7; i++) if (p == want[i]) m = i;
            if (m >= 0 && m < 6 && !front[m]) hidden++;
            if (m < 0) mixed++;
            if (p) lit++;
        }
    CHECK(hidden == 0, "%s: %llu pixels of hidden faces (depth test)", what, (unsigned long long)hidden);
    CHECK(PIX(0, 0) == 0 && PIX(hv.w - 1, hv.h - 1) == 0, "%s: corners not black", what);
    CHECK(lit > 20000, "%s: only %llu pixels drawn", what, (unsigned long long)lit);
    if (f->samples > 1) CHECK(mixed > 100, "%s: MSAA %d left no resolved edge pixels (%llu)", what, f->samples, (unsigned long long)mixed);
    else CHECK(mixed == 0, "%s: %llu in-between colours without MSAA", what, (unsigned long long)mixed);
    printf("  %-34s %llu px drawn, %llu edge blends, MSAA %d in effect\n", what, (unsigned long long)lit,
           (unsigned long long)mixed, f->samples);
    #undef PIX
    hlr_gpu_frame_unref(f);
}

static void run_scale(int sx, int sy, int guard, int n)
{
    char what[64];
    snprintf(what, sizeof what, "%dx%d%s random%s%s", sx, sy, guard ? " guard" : "", s_mode.name[0] ? " " : "", s_mode.name);
    hires_set_tex_filter(s_mode.filter);
    hires_set_colour(s_mode.full);
    hires_set_ssaa(s_mode.ssaa);
    if (!setup(sx, sy, guard)) { CHECK(0, "%s: cannot allocate", what); return; }
    for (int i = 0; i < n; i++) {
        int k = rr(0, 10);
        if (k == 10) poly2d();
        else if (k < 6) poly(k < 4);
        else if (k < 8) rect();
        else if (k == 8) line();
        else misc();
        /* the GPU backend is the shadow of the compare tee here: call it directly */
        if (s_external_every && i % s_external_every == s_external_every - 1)
            hires_gpu_backend()->external(external_noop, NULL);
    }
    if (s_fast_mode) { hires_gpu_tc_check(); compare_tol(what); }
    else compare(what, 0);
}

/* PGXP: the same random triangles, sent as presentation-only packets with sub-pixel vertices
 * (and depths); perspective = all three depths differ */
static void run_pgxp(int sx, int sy, bool persp, int n, u64 budget)
{
    char what[64];
    snprintf(what, sizeof what, "%dx%d pgxp %s", sx, sy, persp ? "perspective" : "affine");
    if (!setup(sx, sy, 0)) { CHECK(0, "%s: cannot allocate", what); return; }
    hires_set_perspective(persp);
    g_pgxp = true;
    static PgxpVert pv[4];
    for (int i = 0; i < n; i++) {
        u32 w[12];
        const PgxpVert *pp[12] = { 0 };
        bool tex = rnd() & 1, gour = rnd() & 1;
        int cx = rr(0, 511), cy = rr(0, 479), k = 0;
        w[k++] = ((0x20u | (gour ? 0x10u : 0) | (tex ? 0x04u : 0)) << 24) | rgb();
        for (int v = 0; v < 3; v++) {
            if (v && gour) w[k++] = rgb();
            int x = cx + rr(-60, 60), y = cy + rr(-60, 60);
            pv[v].sxy = xy(x, y);
            pv[v].dx = (float)rr(-450, 450) / 1000.0f;
            pv[v].dy = (float)rr(-450, 450) / 1000.0f;
            pv[v].w = (float)rr(600, 4000);
            pp[k] = &pv[v];
            w[k++] = pv[v].sxy;
            if (tex) {
                u32 uv = (u32)rr(0, 255) | ((u32)rr(0, 255) << 8);
                if (v == 0) uv |= clut() << 16;
                if (v == 1) uv |= tpage(rr(0, 2), 1) << 16;
                w[k++] = uv;
            }
        }
        pgxp_new_batch();
        gpu_shadow_packet_in_area(w, k, pp);   /* drawn inside the draw area (no guard band here) */
    }
    hires_set_perspective(true);
    if (s_fast_mode) compare_tol(what);
    else compare(what, budget);
}

int main(int argc, char **argv)
{
    s_verbose = getenv("TEST_VERBOSE") != NULL;
    const char *drv = argc > 1 ? argv[1] : "auto";
    if (!strcmp(drv, "switch")) {
        /* View > Renderer's API switch, repeated: shut the backend down and start it on the next
         * available API (each a fresh device of its own), compare after every switch */
        int order[HGPU_NAPI], n = hires_gpu_api_order(order), avail[HGPU_NAPI], na = 0;
        s_verbose = true;
        for (int i = 0; i < n; i++)
            if (hires_gpu_api_probe(order[i])) avail[na++] = order[i];
        s_verbose = getenv("TEST_VERBOSE") != NULL;
        if (!na) { printf("SKIP: no usable SDL_GPU device\n"); return 77; }
        for (int round = 0; round < 6; round++) {
            const int api = avail[round % na];
            hires_set_backend(NULL, NULL);
            hires_set_scale(0, 0);
            hires_gpu_shutdown();
            hires_gpu_set_driver(hires_gpu_api_name(api));
            CHECK(hires_gpu_init(NULL) && hires_gpu_api() == api, "switch %d: %s did not start", round, hires_gpu_api_name(api));
            if (hires_gpu_api() != api) continue;
            char what[64];
            snprintf(what, sizeof what, "switch %d %s", round, hires_gpu_api_title(api));
            s_mode = MODE_DEFAULT;
            s_mode.name = what;
            run_scale(round % 2 ? 4 : 2, round % 2 ? 6 : 2, round % 3 ? 88 : 0, 600);
        }
        hires_set_backend(NULL, NULL);
        hires_set_scale(0, 0);
        hires_gpu_shutdown();
        CHECK(hires_gpu_api() < 0, "shutdown left the backend initialised");
        printf("test_hires_gpu: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(drv, "external")) {
        drv = argc > 2 ? argv[2] : "auto";
        if (!hires_gpu_set_driver(drv)) {
            if (hires_gpu_api_from_name(drv) >= 0) { printf("SKIP: %s is not offered on this platform\n", drv); return 77; }
            printf("usage: test_hires_gpu external [auto|metal|vulkan|d3d12]\n");
            return 2;
        }
        if (!hires_gpu_init(NULL)) { printf("SKIP: no usable SDL_GPU device (%s)\n", drv); return 77; }
        s_external_every = 37;
        static const int esc[][2] = { { 1, 1 }, { 2, 3 }, { 4, 6 } };
        for (size_t i = 0; i < sizeof esc / sizeof esc[0]; i++) {
            run_scale(esc[i][0], esc[i][1], 0, 800);
            run_scale(esc[i][0], esc[i][1], 88, 800);
        }
        CHECK(s_external_calls > 0, "the external pass never ran");
        printf("test_hires_gpu external: %d external passes run\n", s_external_calls);
        s_external_every = 0;
        static const int ms[] = { 1, 4 };
        for (int full = 0; full < 2; full++)
            for (size_t i = 0; i < sizeof ms / sizeof ms[0]; i++) run_cube(ms[i], full != 0);
        hlr_gpu_set_msaa(4);
        hires_set_backend(NULL, NULL);
        hires_set_scale(0, 0);
        hires_gpu_shutdown();
        printf("test_hires_gpu: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(drv, "fast")) {
        /* fast precision: the same streams and modes, checked against a tolerance */
        drv = argc > 2 ? argv[2] : "auto";
        if (!hires_gpu_set_driver(drv)) {
            if (hires_gpu_api_from_name(drv) >= 0) { printf("SKIP: %s is not offered on this platform\n", drv); return 77; }
            printf("usage: test_hires_gpu fast [auto|metal|vulkan|d3d12]\n");
            return 2;
        }
        if (!hires_gpu_init(NULL)) { printf("SKIP: no usable SDL_GPU device (%s)\n", drv); return 77; }
        hires_gpu_set_fast(true);
        hires_gpu_set_tex_cache(HGPU_TC_VERIFY, 1);   /* texture cache on, every entry re-checked */
        s_fast_mode = true;
        static const int fsc[][2] = { { 1, 1 }, { 2, 2 }, { 4, 6 } };
        static const Mode fmodes[] = {
            { HIRES_TEX_NEAREST, false, 1, "" },          { HIRES_TEX_BILINEAR, false, 1, "bilinear" },
            { HIRES_TEX_XBR, true, 1, "xbr full" },       { HIRES_TEX_NEAREST, true, 2, "full ssaa2" },
        };
        for (size_t m = 0; m < sizeof fmodes / sizeof fmodes[0]; m++) {
            s_mode = fmodes[m];
            for (size_t i = 0; i < sizeof fsc / sizeof fsc[0]; i++) {
                if (s_mode.ssaa > 1 && fsc[i][0] * fsc[i][1] > 8) continue;
                run_scale(fsc[i][0], fsc[i][1], 0, 1200);
                run_scale(fsc[i][0], fsc[i][1], 88, 1200);
            }
        }
        s_mode = MODE_DEFAULT;
        hires_set_tex_filter(HIRES_TEX_NEAREST); hires_set_colour(false); hires_set_ssaa(1);
        {
            HiresGpuStats st;
            hires_gpu_get_stats(&st);
            printf("  texture cache: %llu triangles, %llu decodes, %llu re-decodes, %llu evictions, %llu entry checks, "
                   "%llu stale, %llu layers read back, %llu differing\n", st.tc_prims, st.tc_decodes, st.tc_redecodes,
                   st.tc_evictions, st.tc_verified, st.tc_stale, st.tc_readbacks, st.tc_readback_bad);
            CHECK(st.tc_prims > 0 && st.tc_redecodes > 0 && st.tc_verified > 0 && st.tc_readbacks > 0,
                  "texture cache: not exercised");
            CHECK(st.tc_stale == 0 && st.tc_readback_bad == 0, "texture cache: stale entries or GPU layers differ");
        }
        /* PGXP: interpolation precision against hires.c's planes; their random UVs (the whole page
         * across 60 px) are minified noise, which mipmaps average by design: cache off */
        hires_gpu_set_tex_cache(HGPU_TC_OFF, 0);
        run_pgxp(2, 2, false, 600, 0);
        run_pgxp(4, 6, true, 600, 0);
        hires_set_backend(NULL, NULL);
        hires_set_scale(0, 0);
        hires_gpu_set_fast(false);
        printf("test_hires_gpu fast: worst %.3f%% of pixels > 4 steps, worst mean abs error %.3f\n",
               s_fast_worst_big4, s_fast_worst_mae);
        printf("test_hires_gpu: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (!hires_gpu_set_driver(drv)) {
        if (hires_gpu_api_from_name(drv) >= 0) { printf("SKIP: %s is not offered on this platform\n", drv); return 77; }
        printf("usage: test_hires_gpu [auto|metal|vulkan|d3d12]\n");
        return 2;
    }
    s_verbose = true;
    if (!hires_gpu_init(NULL)) { printf("SKIP: no usable SDL_GPU device (%s)\n", drv); return 77; }
    s_verbose = getenv("TEST_VERBOSE") != NULL;
    static const int sc[][2] = { { 1, 1 }, { 2, 2 }, { 3, 2 }, { 4, 6 } };
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        run_scale(sc[i][0], sc[i][1], 0, 1500);
        run_scale(sc[i][0], sc[i][1], 88, 1500);
    }
    run_pgxp(2, 2, false, 600, 0);
    run_pgxp(4, 6, false, 600, 0);
    /* perspective: double vs fixed point may pick the neighbouring texel at exact ties */
    run_pgxp(2, 2, true, 600, 200);
    run_pgxp(4, 6, true, 600, 1200);
    /* every render-quality mode alone and combined: exact as well */
    static const Mode modes[] = {
        { HIRES_TEX_BILINEAR, false, 1, "bilinear" }, { HIRES_TEX_XBR, false, 1, "xbr" },
        { HIRES_TEX_NEAREST, true, 1, "full" },       { HIRES_TEX_NEAREST, false, 2, "ssaa2" },
        { HIRES_TEX_XBR, true, 1, "xbr full" },       { HIRES_TEX_BILINEAR, true, 2, "bilinear full ssaa2" },
    };
    static const int msc[][2] = { { 1, 1 }, { 2, 2 }, { 3, 2 }, { 4, 6 } };
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        s_mode = modes[m];
        for (size_t i = 0; i < sizeof msc / sizeof msc[0]; i++) {
            if (s_mode.ssaa > 1 && msc[i][0] * msc[i][1] > 8) continue;   /* 4x6 x2 = 8x12: slow, covered by 4x4 */
            run_scale(msc[i][0], msc[i][1], 0, 1200);
            run_scale(msc[i][0], msc[i][1], 88, 1200);
        }
    }
    s_mode = modes[5];   /* the windowed defaults at a large scale */
    s_mode.filter = HIRES_TEX_XBR; s_mode.name = "xbr full ssaa2";
    run_scale(2, 3, 88, 1200);
    s_mode = MODE_DEFAULT;
    hires_set_tex_filter(HIRES_TEX_NEAREST); hires_set_colour(false); hires_set_ssaa(1);
    printf("test_hires_gpu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
