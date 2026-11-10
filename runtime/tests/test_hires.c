/* Self-checking tests for the scaled rasterizer (runtime/hires.c) driven through gpu.c,
 * with independent horizontal (Sx) and vertical (Sy) scales.
 * Build: clang -std=c11 -O2 -Wall -Iruntime runtime/gpu.c runtime/hires.c runtime/tests/test_hires.c -o build/test_hires -lm
 */
#include "psx.h"
#include "hires.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void irq_raise(int bit) { (void)bit; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
               printf(__VA_ARGS__); printf("\n"); }                   \
    } while (0)

static void gp0(u32 v) { gpu_write_gp0(v); }
static u32 xy(int x, int y) { return ((u32)(y & 0xFFFF) << 16) | (u32)(x & 0xFFFF); }

static void setup(int sx, int sy)
{
    gpu_init();
    gpu_enable_display_copy();   /* the 1x picture these tests compare against (gpu.c skip rule) */
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0, 0);
    hires_set_scale(sx, sy);
    gp0(0xE3000000);
    gp0(0xE4000000 | (479u << 10) | 511u);
    gp0(0xE5000000);
    gp0(0xE6000000);
}

static const u16 *tgt(int *stride)
{
    HiresView v;
    GpuDisplay d = { 0, 0, 512, 480, false, true };
    if (!hires_get_view(&d, &v)) return NULL;
    *stride = v.stride;
    return v.pixels;
}

typedef struct { u32 count; int minx, miny, maxx, maxy; } Cov;
static Cov coverage(u16 color)
{
    int st, Sx, Sy;
    hires_get_scale(&Sx, &Sy);
    const u16 *t = tgt(&st);
    Cov c = { 0, 1 << 30, 1 << 30, -1, -1 };
    for (int y = 0; y < 480 * Sy; y++)
        for (int x = 0; x < 512 * Sx; x++)
            if ((t[y * st + x] & 0x7FFF) == color) {
                c.count++;
                if (x < c.minx) c.minx = x;
                if (y < c.miny) c.miny = y;
                if (x > c.maxx) c.maxx = x;
                if (y > c.maxy) c.maxy = y;
            }
    return c;
}

#define RED 0x1F   /* GP0 colour 0x0000FF -> 15-bit 0x1F */

static void test_rect(int Sx, int Sy)
{
    setup(Sx, Sy);
    gp0(0x600000FF);                 /* flat rect, variable size, red */
    gp0(xy(10, 20));
    gp0(xy(7, 5));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(Sx * 7 * Sy * 5), "%dx%d rect count %u != %d", Sx, Sy, c.count, 35 * Sx * Sy);
    CHECK(c.minx == 10 * Sx && c.maxx == 17 * Sx - 1 && c.miny == 20 * Sy && c.maxy == 25 * Sy - 1,
          "%dx%d rect bounds %d..%d x %d..%d", Sx, Sy, c.minx, c.maxx, c.miny, c.maxy);
}

static void test_tri(int Sx, int Sy)
{
    setup(Sx, Sy);
    gp0(0x200000FF);                 /* flat triangle: right triangle with 8px legs */
    gp0(xy(20, 20));
    gp0(xy(28, 20));
    gp0(xy(20, 28));
    Cov c = coverage(RED);
    /* hi-res legs are nx = 8*Sx wide and ny = 8*Sy tall; the top-left rule gives, for row Y,
     * pixels 0 .. ceil(nx*(ny-Y)/ny)-1 */
    int nx = 8 * Sx, ny = 8 * Sy;
    u32 expect = 0;
    for (int Y = 0; Y < ny; Y++) expect += (u32)((nx * (ny - Y) + ny - 1) / ny);
    CHECK(c.count == expect, "%dx%d tri count %u != %u", Sx, Sy, c.count, expect);
    CHECK(c.minx == 20 * Sx && c.miny == 20 * Sy && c.maxy == 28 * Sy - 1, "%dx%d tri bounds", Sx, Sy);
}

static void test_line(int Sx, int Sy)
{
    setup(Sx, Sy);
    gp0(0x400000FF);                 /* horizontal (10,5)-(20,5): 11 1x pixels long, Sy thick */
    gp0(xy(10, 5));
    gp0(xy(20, 5));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(11 * Sx * Sy), "%dx%d hline count %u != %d", Sx, Sy, c.count, 11 * Sx * Sy);
    CHECK(c.minx == 10 * Sx && c.maxx == 21 * Sx - 1 && c.miny == 5 * Sy && c.maxy == 6 * Sy - 1,
          "%dx%d hline bounds %d..%d %d..%d", Sx, Sy, c.minx, c.maxx, c.miny, c.maxy);
    setup(Sx, Sy);
    gp0(0x400000FF);                 /* vertical: 8 1x pixels long, Sx thick */
    gp0(xy(30, 10));
    gp0(xy(30, 17));
    c = coverage(RED);
    CHECK(c.count == (u32)(8 * Sx * Sy), "%dx%d vline count %u != %d", Sx, Sy, c.count, 8 * Sx * Sy);
    CHECK(c.minx == 30 * Sx && c.maxx == 31 * Sx - 1 && c.miny == 10 * Sy && c.maxy == 18 * Sy - 1,
          "%dx%d vline bounds", Sx, Sy);
    setup(Sx, Sy);
    gp0(0x400000FF);                 /* single point */
    gp0(xy(40, 40));
    gp0(xy(40, 40));
    c = coverage(RED);
    CHECK(c.count == (u32)(Sx * Sy), "%dx%d point count %u", Sx, Sy, c.count);
}

static void test_fill_upload_copy(int Sx, int Sy)
{
    setup(Sx, Sy);
    gp0(0x020000FF);                 /* fill (16,16) 32x8 */
    gp0(xy(16, 16));
    gp0(xy(32, 8));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(32 * 8 * Sx * Sy), "%dx%d fill count %u", Sx, Sy, c.count);
    CHECK(c.minx == 16 * Sx && c.maxx == 48 * Sx - 1 && c.miny == 16 * Sy && c.maxy == 24 * Sy - 1,
          "%dx%d fill bounds", Sx, Sy);

    gp0(0xA0000000);                 /* upload 4x2 green pixels to (100,100) */
    gp0(xy(100, 100));
    gp0(xy(4, 2));
    for (int i = 0; i < 4; i++) gp0(0x03E003E0);
    Cov g = coverage(0x3E0);
    CHECK(g.count == (u32)(8 * Sx * Sy), "%dx%d upload count %u", Sx, Sy, g.count);
    CHECK(g.minx == 100 * Sx && g.maxx == 104 * Sx - 1 && g.miny == 100 * Sy && g.maxy == 102 * Sy - 1,
          "%dx%d upload bounds", Sx, Sy);

    gp0(0x80000000);                 /* copy the 32x8 red block to (200,300) */
    gp0(xy(16, 16));
    gp0(xy(200, 300));
    gp0(xy(32, 8));
    c = coverage(RED);
    CHECK(c.count == (u32)(2 * 32 * 8 * Sx * Sy), "%dx%d copy count %u", Sx, Sy, c.count);
    CHECK(c.maxx == 232 * Sx - 1 && c.maxy == 308 * Sy - 1, "%dx%d copy bounds", Sx, Sy);

    /* mask bit: with mask-check on, a masked pixel is not overwritten in the target */
    gp0(0xE6000001);                 /* set mask bit on draw */
    gp0(0x60001F00);
    gp0(xy(300, 100));
    gp0(xy(4, 4));
    gp0(0xE6000002);                 /* check mask */
    gp0(0x600000FF);
    gp0(xy(300, 100));
    gp0(xy(4, 4));
    gp0(0xE6000000);
    c = coverage(RED);
    CHECK(c.count == (u32)(2 * 32 * 8 * Sx * Sy), "%dx%d masked rect must not draw (count %u)", Sx, Sy, c.count);
}

static u64 vram_hash(void)
{
    const u64 *w = (const u64 *)gpu_display_vram();
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < 1024 * 512 * 2 / 8; i++) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}

/* A busy command stream: textured + gouraud + semi-transparent polygons, rects, lines. */
static void busy_stream(void)
{
    /* texture: upload a 16x16 4bpp texture and a CLUT into the texture area */
    gp0(0xA0000000); gp0(xy(640, 0)); gp0(xy(16, 16));
    for (int i = 0; i < 128; i++) gp0(0x76543210u * (u32)(i + 1));
    gp0(0xA0000000); gp0(xy(0, 500)); gp0(xy(16, 1));
    for (int i = 0; i < 8; i++) gp0(0x7C1F03E0u + (u32)i * 0x00210021u);
    gp0(0x24808080);                                        /* textured triangle */
    gp0(xy(50, 50)); gp0((500u << 6 << 16) | 0x0000);       /* uv0 + CLUT (0,500) */
    gp0(xy(120, 55)); gp0((10u << 16) | 0x000F);            /* uv1 + texpage x=640 4bpp */
    gp0(xy(60, 130)); gp0(0x0F00);
    gp0(0x2C808080);                                        /* textured quad, semi off */
    gp0(xy(150, 150)); gp0((500u << 6 << 16) | 0x0000);
    gp0(xy(190, 150)); gp0((10u << 16) | 0x000F);
    gp0(xy(150, 190)); gp0(0x0F00);
    gp0(xy(195, 195)); gp0(0x0F0F);
    gp0(0x3000FF00); gp0(xy(200, 20)); gp0(0x000000FF); gp0(xy(260, 40));
    gp0(0x00FF0000); gp0(xy(210, 90));                       /* gouraud tri */
    gp0(0x62808080); gp0(xy(300, 60)); gp0(xy(20, 20));      /* semi-transparent rect */
    gp0(0x50FF00FF); gp0(xy(5, 5)); gp0(0x500000FF); gp0(xy(90, 70)); gp0(0x55555555); /* gouraud line */
}

static void test_s1_equivalence_and_invariance(void)
{
    setup(1, 1);
    busy_stream();
    int fx, fy;
    u32 bad = hires_verify_1x(&fx, &fy);
    CHECK(bad == 0, "1x1 target differs from 1x VRAM in %u pixels (first %d,%d)", bad, fx, fy);
    u64 h1 = vram_hash();
    static const int sc[][2] = { { 0, 0 }, { 2, 2 }, { 4, 4 }, { 2, 3 }, { 4, 6 }, { 1, 3 }, { 3, 1 } };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        setup(sc[i][0], sc[i][1]);
        busy_stream();
        CHECK(vram_hash() == h1, "1x VRAM differs at scale %dx%d", sc[i][0], sc[i][1]);
    }
}

/* Non-uniform scale of a busy stream, downsampled by box-averaging, must stay close to the 1x
 * picture: catches axis mix-ups (e.g. x/y swapped scales) that coverage counts alone might miss. */
static void test_nonuniform_matches_1x_shape(int Sx, int Sy)
{
    setup(1, 1);
    busy_stream();
    int st1, Sx1, Sy1;
    hires_get_scale(&Sx1, &Sy1);
    const u16 *t1 = tgt(&st1);
    static u16 ref[512 * 480];
    for (int y = 0; y < 480; y++) memcpy(&ref[y * 512], &t1[y * st1], 512 * 2);
    setup(Sx, Sy);
    busy_stream();
    int st;
    const u16 *t = tgt(&st);
    /* a 1x pixel is "mostly the same" if the sub-pixel at its centre has the same colour */
    long same = 0, total = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 512; x++) {
            u16 a = ref[y * 512 + x] & 0x7FFF, b = t[(y * Sy + Sy / 2) * st + x * Sx + Sx / 2] & 0x7FFF;
            total++;
            if (a == b) same++;
            else {
                int dr = abs((a & 31) - (b & 31)), dg = abs(((a >> 5) & 31) - ((b >> 5) & 31)),
                    db = abs(((a >> 10) & 31) - ((b >> 10) & 31));
                if (dr + dg + db <= 12) same++;   /* close colours / blended edges */
            }
        }
    CHECK(same * 100 >= total * 98, "%dx%d picture deviates from 1x: %ld/%ld similar", Sx, Sy, same, total);
}

static u64 target_hash(int Sx, int Sy)
{
    int st;
    const u16 *t = tgt(&st);
    u64 h = 1469598103934665603ull;
    for (int y = 0; y < 480 * Sy; y++)
        for (int x = 0; x < 512 * Sx; x++) h = (h ^ t[y * st + x]) * 1099511628211ull;
    return h;
}

/* The worker pool must not change the result: same target with 0 and 4 workers, with every
 * primitive dispatched (threshold 0). */
static void test_threads_identical(int Sx, int Sy)
{
    hires_set_threads(0);
    hires_set_parallel_threshold(0);
    setup(Sx, Sy);
    busy_stream();
    u64 h0 = target_hash(Sx, Sy);
    hires_set_threads(4);
    setup(Sx, Sy);
    busy_stream();
    u64 h4 = target_hash(Sx, Sy);
    CHECK(hires_threads() == 4, "pool did not start");
    CHECK(h0 == h4, "%dx%d target differs between 0 and 4 worker threads", Sx, Sy);
    hires_set_threads(0);
    hires_set_parallel_threshold(4000);
}

/* Texture bleed: a quad with UVs 0..15 sits next to differently coloured texels at u=255 (wraps
 * to the left of u=0) and u=16 (right of u=15). Sampling at hi-res pixel centres reaches
 * outside the polygon's UV range; no pixel of the neighbouring colours may appear. */
static void test_uv_clamp(int Sx, int Sy)
{
    setup(Sx, Sy);
    /* 8bpp texture page x=640 (page 10): 16 rows; CLUT at (0,500): index1 = green(inside),
     * index2 = blue (u=255 column), index3 = white (u=16 column) */
    gp0(0xA0000000); gp0(xy(0, 500)); gp0(xy(16, 1));
    u16 clut[16] = { 0, 0x03E0, 0x7C00, 0x7FFF };
    for (int i = 0; i < 8; i++) gp0((u32)clut[2 * i] | ((u32)clut[2 * i + 1] << 16));
    /* texture rows v=0..15 at texture x 640..  (8bpp: 2 texels per halfword, 128 hw per row) */
    gp0(0xA0000000); gp0(xy(640, 0)); gp0(xy(128, 16));
    for (int v = 0; v < 16; v++)
        for (int hw = 0; hw < 128; hw += 2) {
            u32 w[2];
            for (int k = 0; k < 2; k++) {
                int u0 = (hw + k) * 2;
                u16 val = 0;
                for (int b = 0; b < 2; b++) {
                    int u = u0 + b, idx = 0;
                    if (u < 16) idx = 1;
                    else if (u == 16) idx = 3;
                    else if (u == 255) idx = 2;
                    val |= (u16)(idx << (8 * b));
                }
                w[k] = val;
            }
            gp0(w[0] | (w[1] << 16));
        }
    /* a textured quad, UV 0..15 on both axes, covering 8x8 1x pixels at (100,100), so the texture is minified (~2 texels per pixel): a slightly
     * sheared quad so that interpolation is not axis aligned */
    u32 clutw = (500u << 6) << 16;                    /* CLUT (0,500) */
    u32 tpage = 10u | (1u << 7);                      /* page 10, 8bpp */
    gp0(0x2C808080);
    gp0(xy(100, 100)); gp0(clutw | 0x0000);
    gp0(xy(108, 101)); gp0((tpage << 16) | 0x000F);
    gp0(xy(100, 109)); gp0(0x0F00);
    gp0(xy(108, 110)); gp0(0x0F0F);
    int st;
    const u16 *t = tgt(&st);
    long green = 0, blue = 0, white = 0;
    for (int y = 90 * Sy; y < 130 * Sy; y++)
        for (int x = 90 * Sx; x < 130 * Sx; x++) {
            u16 px = t[y * st + x] & 0x7FFF;
            if (px == 0x03E0 || ((px & 0x3E0) > 0x200 && (px & 0x7C1F) < 0x2000 && px != 0x7FFF)) green++;
            if (px == 0x7C00) blue++;
            if (px == 0x7FFF) white++;
        }
    CHECK(green > 0, "%dx%d no inside texels drawn", Sx, Sy);
    CHECK(blue == 0, "%dx%d %ld pixels of the u=255 neighbour texels leaked in", Sx, Sy, blue);
    CHECK(white == 0, "%dx%d %ld pixels of the u=16 neighbour texels leaked in", Sx, Sy, white);
    HiresUvStats uv;
    hires_get_uv_stats(&uv);
    CHECK(uv.escaped == 0, "%dx%d %llu samples escaped the UV range", Sx, Sy, (unsigned long long)uv.escaped);
}

/* The 1x GPU never draws a triangle's last column/row, so it never samples the texels at the far
 * end of the vertex UV range. Here those (u or v = 14, 15) are bright white while everything 1x
 * can reach is green; hi-res sub-pixel samples near the right/bottom edge must not show white. */
static void test_uv_1x_range(int Sx, int Sy)
{
    setup(Sx, Sy);
    gp0(0xA0000000); gp0(xy(0, 500)); gp0(xy(16, 1));
    u16 clut[16] = { 0, 0x03E0, 0x7FFF, 0 };
    for (int i = 0; i < 8; i++) gp0((u32)clut[2 * i] | ((u32)clut[2 * i + 1] << 16));
    gp0(0xA0000000); gp0(xy(640, 0)); gp0(xy(128, 16));
    for (int v = 0; v < 16; v++)
        for (int hw = 0; hw < 128; hw += 2) {
            u32 w[2];
            for (int k = 0; k < 2; k++) {
                u16 val = 0;
                for (int b = 0; b < 2; b++) {
                    int u = (hw + k) * 2 + b;
                    int idx = 0;
                    if (u < 16 && v < 16) idx = (u >= 14 || v >= 14) ? 2 : 1;
                    val |= (u16)(idx << (8 * b));
                }
                w[k] = val;
            }
            gp0(w[0] | (w[1] << 16));
        }
    u32 clutw = (500u << 6) << 16, tpage = 10u | (1u << 7);
    gp0(0x2C808080);
    gp0(xy(100, 100)); gp0(clutw | 0x0000);
    gp0(xy(108, 100)); gp0((tpage << 16) | 0x000F);
    gp0(xy(100, 108)); gp0(0x0F00);
    gp0(xy(108, 108)); gp0(0x0F0F);
    int st;
    const u16 *t = tgt(&st);
    long green = 0, white = 0;
    for (int y = 90 * Sy; y < 130 * Sy; y++)
        for (int x = 90 * Sx; x < 130 * Sx; x++) {
            u16 px = t[y * st + x] & 0x7FFF;
            if (px == 0x7FFF || ((px & 0x7C00) > 0x5000 && (px & 0x1F) > 0x14)) white++;
            else if (px) green++;
        }
    CHECK(green > 0, "%dx%d 1x-range quad not drawn", Sx, Sy);
    CHECK(white == 0, "%dx%d %ld pixels of texels the 1x draw never samples (u/v >= 14) appeared", Sx, Sy, white);
    HiresUvStats uv;
    hires_get_uv_stats(&uv);
    CHECK(uv.escaped == 0, "%dx%d %llu samples escaped the 1x-sampled range", Sx, Sy, (unsigned long long)uv.escaped);
    if (Sx > 1 || Sy > 1) CHECK(uv.tris_1x_range > 0, "%dx%d the 1x-sampled range was not used", Sx, Sy);
}

/* ---- render quality: texture filters, full-precision colour, SSAA ---------------------- */

static void quality(int filter, bool full, int ssaa)
{
    hires_set_tex_filter(filter);
    hires_set_colour(full);
    hires_set_ssaa(ssaa);
}

static bool view_of(HiresView *v)
{
    GpuDisplay d = { 0, 0, 512, 480, false, true };
    return hires_get_view(&d, v);
}

/* 8bpp page 10 (x 640), CLUT at (0,500); idx(u, v) picks the CLUT entry of every texel */
static void upload_tex8(const u16 clut[16], int (*idx)(int u, int v))
{
    gp0(0xA0000000); gp0(xy(0, 500)); gp0(xy(16, 1));
    for (int i = 0; i < 8; i++) gp0((u32)clut[2 * i] | ((u32)clut[2 * i + 1] << 16));
    gp0(0xA0000000); gp0(xy(640, 0)); gp0(xy(128, 32));
    for (int v = 0; v < 32; v++)
        for (int hw = 0; hw < 128; hw += 2) {
            u32 w[2];
            for (int k = 0; k < 2; k++) {
                int u = (hw + k) * 2;
                w[k] = (u32)idx(u, v) | ((u32)idx(u + 1, v) << 8);
            }
            gp0(w[0] | (w[1] << 16));
        }
}
#define CLUTW ((500u << 6) << 16)
#define TPAGE8 (10u | (1u << 7))

static int idx_far_white(int u, int v) { return (u < 16 && v < 16) ? ((u >= 14 || v >= 14) ? 2 : 1) : 3; }

/* The whole filter footprint (both bilinear taps, xbr's neighbours) stays inside the 1x-sampled
 * range: texels the 1x draw never reads (u/v >= 14, and everything outside 0..15) are white /
 * red, everything else pure green, so any tap outside shows up as red or blue in a pixel. */
static void test_filter_footprint(int filter, int Sx, int Sy)
{
    quality(filter, false, 1);
    setup(Sx, Sy);
    static const u16 clut[16] = { 0, 0x03E0, 0x7FFF, 0x001F };
    upload_tex8(clut, idx_far_white);
    gp0(0x2C808080);   /* 3D-ish quad (minified, not 1:1) */
    gp0(xy(100, 100)); gp0(CLUTW | 0x0000);
    gp0(xy(108, 100)); gp0((TPAGE8 << 16) | 0x000F);
    gp0(xy(100, 108)); gp0(0x0F00);
    gp0(xy(108, 108)); gp0(0x0F0F);
    gp0(0x2C808080);   /* 1:1 sprite quad (2D path of xbr): drawn u/v 0..13 */
    gp0(xy(200, 100)); gp0(CLUTW | 0x0000);
    gp0(xy(214, 100)); gp0((TPAGE8 << 16) | 0x000E);
    gp0(xy(200, 114)); gp0(0x0E00);
    gp0(xy(214, 114)); gp0(0x0E0E);
    gp0(0xE1000000 | TPAGE8);   /* textured rects: 14x14 from u/v 0, and x-flipped from u 13 */
    gp0(0x64808080); gp0(xy(300, 100)); gp0(CLUTW | 0x0000); gp0(xy(14, 14));
    gp0(0xE1000000 | TPAGE8 | 0x1000);
    gp0(0x64808080); gp0(xy(400, 100)); gp0(CLUTW | 0x000D); gp0(xy(14, 14));
    gp0(0xE1000000 | TPAGE8);
    int st;
    const u16 *t = tgt(&st);
    long drawn = 0, bad = 0, mixed = 0;
    for (int y = 90 * Sy; y < 130 * Sy; y++)
        for (int x = 90 * Sx; x < 430 * Sx; x++) {
            u16 px = t[y * st + x] & 0x7FFF;
            if (!px) continue;
            drawn++;
            if ((px & 0x7C1F) != 0) bad++;
            if ((px & 0x3E0) != 0x3E0) mixed++;
        }
    const char *fn = hires_tex_filter_name(filter);
    CHECK(drawn > 0, "%s %dx%d nothing drawn", fn, Sx, Sy);
    CHECK(bad == 0, "%s %dx%d %ld pixels sampled texels outside the 1x-sampled range", fn, Sx, Sy, bad);
    CHECK(mixed == 0, "%s %dx%d %ld pixels darkened (uniform texels must stay exact)", fn, Sx, Sy, mixed);
    HiresUvStats uv;
    hires_get_uv_stats(&uv);
    CHECK(uv.escaped == 0, "%s %dx%d %llu samples escaped", fn, Sx, Sy, (unsigned long long)uv.escaped);
    quality(HIRES_TEX_NEAREST, false, 1);
}

/* Texel 0000 is transparent: filtered taps on it carry no weight (renormalised), so the edge
 * between a sprite and its transparent surround is the sprite colour or the background, never
 * darker (no black fringe); full precision keeps it exact too. */
static int idx_checker(int u, int v) { return (((u >> 2) + (v >> 2)) & 1) ? 0 : 1; }
static void test_transparent_texels(int filter, bool full, int Sx, int Sy)
{
    quality(filter, full, 1);
    setup(Sx, Sy);
    static const u16 clut[16] = { 0, 0x03E0 };
    upload_tex8(clut, idx_checker);
    gp0(0x02F80000); gp0(xy(0, 0)); gp0(xy(512, 300));   /* background: blue */
    gp0(0xE1000000 | TPAGE8);
    gp0(0x64808080); gp0(xy(100, 100)); gp0(CLUTW | 0x0101); gp0(xy(24, 24));   /* 2D */
    gp0(0x2C808080);   /* 3D (magnified 2x, sheared) */
    gp0(xy(200, 100)); gp0(CLUTW | 0x0000);
    gp0(xy(248, 104)); gp0((TPAGE8 << 16) | 0x0018);
    gp0(xy(198, 148)); gp0(0x1800);
    gp0(xy(246, 152)); gp0(0x1818);
    HiresView v;
    CHECK(view_of(&v), "no view");
    long fg = 0, other = 0;
    for (int y = 95 * Sy; y < 160 * Sy; y++)
        for (int x = 95 * Sx; x < 260 * Sx; x++) {
            u32 r, g, b;
            if (v.fmt == HIRES_FMT_RGBA8) {
                u32 p = v.px32[(size_t)y * v.stride + x];
                r = p & 255; g = (p >> 8) & 255; b = (p >> 16) & 255;
            } else {
                u16 p = v.pixels[(size_t)y * v.stride + x];
                r = (p & 31u) << 3; g = ((p >> 5) & 31u) << 3; b = ((p >> 10) & 31u) << 3;
            }
            if (r == 0 && g == 0xF8 && b == 0) fg++;   /* sprite green, exact */
            else if (!(r == 0 && g == 0 && b == 0xF8)) other++;
        }
    const char *fn = hires_tex_filter_name(filter);
    CHECK(fg > 0, "%s %s %dx%d sprite not drawn", fn, full ? "full" : "15", Sx, Sy);
    CHECK(other == 0, "%s %s %dx%d %ld fringe pixels (neither sprite nor background)", fn, full ? "full" : "15", Sx, Sy, other);
    quality(HIRES_TEX_NEAREST, false, 1);
}

/* Full precision: 8-bit channels; the four semi-transparency modes clamp like the hardware;
 * the mask bit is kept and honoured. */
static void test_full_blend(int Sx, int Sy)
{
    quality(HIRES_TEX_NEAREST, true, 1);
    setup(Sx, Sy);
    static const u32 bg = 0x18C8F0, fgc[] = { 0x30E0FF, 0x0F1003, 0xC0C0C0 };   /* GP0 BGR */
    for (int m = 0; m < 4; m++)
        for (int k = 0; k < 3; k++) {
            int x = 16 + 32 * (m * 3 + k);
            gp0(0x02000000 | bg); gp0(xy(x, 100)); gp0(xy(32, 20));
            gp0(0xE1000000 | ((u32)m << 5));
            gp0(0x62000000 | fgc[k]); gp0(xy(x + 4, 104)); gp0(xy(10, 10));
        }
    HiresView v;
    CHECK(view_of(&v) && v.fmt == HIRES_FMT_RGBA8, "full precision target missing");
    if (v.fmt != HIRES_FMT_RGBA8) return;
    int bad = 0;
    for (int m = 0; m < 4; m++)
        for (int k = 0; k < 3; k++) {
            int x = 16 + 32 * (m * 3 + k) + 7;
            u32 p = v.px32[(size_t)(106 * Sy) * v.stride + (size_t)x * Sx];
            for (int c = 0; c < 3; c++) {
                int B = (int)((bg >> (8 * c)) & 0xF8), F = (int)((fgc[k] >> (8 * c)) & 255), e;
                switch (m) {
                case 0: e = (B + F) >> 1; break;
                case 1: e = B + F > 255 ? 255 : B + F; break;
                case 2: e = B - F < 0 ? 0 : B - F; break;
                default: e = B + F / 4 > 255 ? 255 : B + F / 4; break;
                }
                int got = (int)((p >> (8 * c)) & 255);
                if (got != e) { bad++; printf("  mode %d fg %06x ch %d: got %d want %d\n", m, fgc[k], c, got, e); }
            }
            CHECK(!(p & 0xFF000000u), "mode %d: mask bit set", m);
        }
    CHECK(bad == 0, "full precision %dx%d: %d blended channels wrong", Sx, Sy, bad);
    /* mask bit kept: set-mask on, then a check-mask draw must leave it alone */
    gp0(0xE1000000);
    gp0(0xE6000001); gp0(0x60FF0000); gp0(xy(20, 200)); gp0(xy(8, 8));
    gp0(0xE6000002); gp0(0x6000FF00); gp0(xy(20, 200)); gp0(xy(8, 8));
    gp0(0xE6000000);
    CHECK(view_of(&v), "no view");
    u32 p = v.px32[(size_t)(203 * Sy) * v.stride + (size_t)23 * Sx];
    CHECK(p == 0xFFFF0000u, "mask bit: got %08x", p);   /* untextured: the 8-bit vertex colour */
    quality(HIRES_TEX_NEAREST, false, 1);
}

/* Opaque primitives: full precision >> 3 is exactly the 15-bit result (only blending, gouraud
 * and modulation keep the extra bits), dither being off in both above 1x1. */
static void opaque_scene(void)
{
    static const u16 clut[16] = { 0, 0x03E0, 0x7FFF, 0x001F };
    upload_tex8(clut, idx_far_white);
    gp0(0x30FF0000); gp0(xy(10, 10)); gp0(0x0000FF00); gp0(xy(200, 30)); gp0(0x000000FF); gp0(xy(60, 220));
    gp0(0x2C6040C0);
    gp0(xy(250, 20)); gp0(CLUTW | 0x0000);
    gp0(xy(400, 40)); gp0((TPAGE8 << 16) | 0x001F);
    gp0(xy(260, 200)); gp0(0x1F00);
    gp0(xy(420, 230)); gp0(0x1F1F);
    gp0(0x3C102030);   /* gouraud textured */
    gp0(xy(20, 250)); gp0(CLUTW | 0x0000); gp0(0x00A0FF40);
    gp0(xy(200, 260)); gp0((TPAGE8 << 16) | 0x000F); gp0(0x00FFFFFF);
    gp0(xy(30, 400)); gp0(0x0F00); gp0(0x00808080);
    gp0(xy(210, 420)); gp0(0x0F0F);
    gp0(0xE1000000 | TPAGE8);
    gp0(0x64FF8020); gp0(xy(300, 300)); gp0(CLUTW | 0x0000); gp0(xy(30, 30));
    gp0(0x50FF00FF); gp0(xy(5, 470)); gp0(0x0000FF00); gp0(xy(500, 300));
}
static void test_full_matches_15(int Sx, int Sy)
{
    quality(HIRES_TEX_NEAREST, false, 1);
    setup(Sx, Sy);
    opaque_scene();
    HiresView a;
    CHECK(view_of(&a) && a.fmt == HIRES_FMT_15, "15-bit target");
    const size_t n = (size_t)a.stride * a.h;
    u16 *c15 = malloc(n * 2);
    memcpy(c15, a.pixels, n * 2);
    const int st15 = a.stride;
    quality(HIRES_TEX_NEAREST, true, 1);
    setup(Sx, Sy);
    opaque_scene();
    HiresView b;
    CHECK(view_of(&b) && b.fmt == HIRES_FMT_RGBA8, "full target");
    long diff = 0, nonzero = 0;
    for (int y = 0; y < 480 * Sy; y++)
        for (int x = 0; x < 512 * Sx; x++) {
            u32 p = b.px32[(size_t)y * b.stride + x];
            u16 q = c15[(size_t)y * st15 + x];
            u16 r = (u16)(((p & 255) >> 3) | ((((p >> 8) & 255) >> 3) << 5) | ((((p >> 16) & 255) >> 3) << 10) |
                          ((p >> 24) ? 0x8000 : 0));
            if (q) nonzero++;
            if (r != q) diff++;
        }
    free(c15);
    CHECK(nonzero > 1000, "scene not drawn");
    CHECK(diff == 0, "full >> 3 vs 15-bit %dx%d: %ld pixels differ", Sx, Sy, diff);
    quality(HIRES_TEX_NEAREST, false, 1);
}

/* SSAA multiplies the render scale; hires_get_scale keeps the base; the present conversion is
 * an n x n box average; the factor shrinks to fit the per-axis and size limits. */
static void test_ssaa(void)
{
    quality(HIRES_TEX_NEAREST, true, 2);
    setup(2, 3);
    int bx, by, rx, ry;
    hires_get_scale(&bx, &by);
    hires_get_render_scale(&rx, &ry);
    CHECK(bx == 2 && by == 3 && rx == 4 && ry == 6 && hires_ssaa() == 2, "ssaa 2 at 2x3: base %dx%d render %dx%d n %d",
          bx, by, rx, ry, hires_ssaa());
    gp0(0x30FF0000); gp0(xy(10, 10)); gp0(0x0000FF00); gp0(xy(200, 30)); gp0(0x000000FF); gp0(xy(60, 220));
    HiresView v;
    CHECK(view_of(&v) && v.ssaa == 2, "view ssaa");
    const int ow = v.w / 2, oh = v.h / 2;
    u32 *out = malloc((size_t)ow * oh * 4);
    hires_view_to_rgba(&v, out, (size_t)ow, 0, oh);
    long bad = 0, edge = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            u32 s[3] = { 0, 0, 0 };
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    u32 p = v.px32[(size_t)(2 * y + j) * v.stride + 2 * x + i];
                    for (int c = 0; c < 3; c++) s[c] += (p >> (8 * c)) & 255;
                }
            const u32 o = out[(size_t)y * ow + x];
            const u32 e = ((s[0] + 2) / 4) | (((s[1] + 2) / 4) << 8) | (((s[2] + 2) / 4) << 16);
            if ((o & 0xFFFFFF) != e) bad++;
            if (s[0] % 4) edge++;
        }
    free(out);
    CHECK(bad == 0, "ssaa box average: %ld pixels differ", bad);
    CHECK(edge > 0, "ssaa: no partially covered pixels (no anti-aliasing?)");
    quality(HIRES_TEX_NEAREST, false, 4);
    setup(4, 6);   /* 4x6 x 4 = 16x24 > 16 per axis: falls back to x2 (8x12, 4096 x 5760) */
    hires_get_render_scale(&rx, &ry);
    CHECK(hires_ssaa() == 2 && rx == 8 && ry == 12, "ssaa 4 at 4x6: n %d render %dx%d", hires_ssaa(), rx, ry);
    hires_set_ssaa_cap(24);   /* the frontend's default: no more than a 4x6 target */
    quality(HIRES_TEX_NEAREST, false, 2);
    setup(2, 3);
    CHECK(hires_ssaa() == 2, "cap 24: 2x3 x2 = 4x6 allowed, n %d", hires_ssaa());
    setup(4, 6);
    CHECK(hires_ssaa() == 1, "cap 24: 4x6 x2 refused, n %d", hires_ssaa());
    hires_set_ssaa_cap(0);
    setup(1, 1);
    CHECK(!hires_verify_possible(), "verify must be refused with ssaa");
    quality(HIRES_TEX_NEAREST, true, 1);
    setup(1, 1);
    CHECK(!hires_verify_possible(), "verify must be refused in full precision");
    quality(HIRES_TEX_NEAREST, false, 1);
    setup(1, 1);
    CHECK(hires_verify_possible(), "verify at 1x1 15-bit");
}

static void test_2d_detection(void)
{
    HiresVtx s[3] = { { 10 << 16, 20 << 16, 0, 0, 0, 5, 7, 0 }, { 30 << 16, 20 << 16, 0, 0, 0, 25, 7, 0 },
                      { 10 << 16, 50 << 16, 0, 0, 0, 5, 37, 0 } };
    CHECK(hires_tri_is_2d(s), "1:1 sprite triangle not 2D");
    HiresVtx f[3] = { s[0], s[1], s[2] };
    f[0].u = 25; f[1].u = 5; f[2].u = 25;   /* mirrored: still 1:1 */
    CHECK(hires_tri_is_2d(f), "mirrored sprite not 2D");
    HiresVtx m[3] = { s[0], s[1], s[2] };
    m[1].u = 45;   /* 2:1 magnified */
    CHECK(!hires_tri_is_2d(m), "scaled triangle taken as 2D");
    HiresVtx r[3] = { s[0], s[1], s[2] };
    r[1].v = 8;    /* sheared */
    CHECK(!hires_tri_is_2d(r), "sheared triangle taken as 2D");
    HiresVtx w[3] = { s[0], s[1], s[2] };
    w[0].w = w[1].w = w[2].w = 100.0f;   /* PGXP depth = 3D */
    CHECK(!hires_tri_is_2d(w), "PGXP triangle taken as 2D");
}

static void check_pick(int pw, int ph, int dw, int dh, int mx, int my, int ex, int ey)
{
    int sx, sy;
    hires_pick_scale(pw, ph, dw, dh, mx, my, &sx, &sy);
    CHECK(sx == ex && sy == ey, "pick %dx%d disp %dx%d cap %dx%d -> %dx%d, want %dx%d",
          pw, ph, dw, dh, mx, my, sx, sy, ex, ey);
}

int main(void)
{
    static const int sc[][2] = { { 1, 1 }, { 2, 2 }, { 4, 4 }, { 2, 3 }, { 4, 6 }, { 3, 1 }, { 1, 5 } };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        test_rect(sc[i][0], sc[i][1]);
        test_tri(sc[i][0], sc[i][1]);
        test_line(sc[i][0], sc[i][1]);
        test_fill_upload_copy(sc[i][0], sc[i][1]);
    }
    test_s1_equivalence_and_invariance();
    test_nonuniform_matches_1x_shape(2, 3);
    test_nonuniform_matches_1x_shape(4, 6);
    test_uv_clamp(1, 1);
    test_uv_clamp(2, 2);
    test_uv_clamp(4, 6);
    test_uv_clamp(3, 5);
    test_uv_1x_range(1, 1);
    test_uv_1x_range(2, 2);
    test_uv_1x_range(4, 6);
    test_uv_1x_range(3, 5);
    test_threads_identical(2, 3);
    test_threads_identical(4, 6);
    static const int qs[][2] = { { 2, 2 }, { 4, 6 }, { 3, 5 }, { 1, 1 } };
    for (unsigned i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        test_filter_footprint(HIRES_TEX_BILINEAR, qs[i][0], qs[i][1]);
        test_filter_footprint(HIRES_TEX_XBR, qs[i][0], qs[i][1]);
        test_transparent_texels(HIRES_TEX_BILINEAR, false, qs[i][0], qs[i][1]);
        test_transparent_texels(HIRES_TEX_XBR, true, qs[i][0], qs[i][1]);
        test_full_blend(qs[i][0], qs[i][1]);
    }
    test_full_matches_15(2, 2);
    test_full_matches_15(4, 6);
    test_ssaa();
    test_2d_detection();
    check_pick(1920, 1440, 512, 240, 8, 8, 4, 6);     /* 1440p-ish window: 2048x1440 */
    check_pick(960, 720, 512, 240, 8, 8, 2, 3);
    check_pick(1920, 1080, 512, 240, 8, 8, 3, 5);     /* letterboxed 1440x1080 */
    check_pick(512, 384, 512, 240, 8, 8, 1, 2);
    check_pick(3840, 2160, 512, 240, 8, 8, 6, 9 > 8 ? 8 : 9);
    check_pick(3840, 2160, 512, 240, 4, 6, 4, 6);     /* caps */
    check_pick(1920, 1440, 320, 240, 8, 8, 6, 6);     /* narrower display mode */
    printf("hires: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
