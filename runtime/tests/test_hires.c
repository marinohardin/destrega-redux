/* Self-checking tests for the scaled rasterizer (runtime/hires.c) driven through gpu.c.
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

static void setup(int scale)
{
    gpu_init();
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0);
    hires_set_scale(scale);
    gp0(0xE3000000);
    gp0(0xE4000000 | (479u << 10) | 511u);
    gp0(0xE5000000);
    gp0(0xE6000000);
}

/* Target pixel access (1x coords scaled by S). */
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
    int st, S = hires_scale();
    const u16 *t = tgt(&st);
    Cov c = { 0, 1 << 30, 1 << 30, -1, -1 };
    for (int y = 0; y < 480 * S; y++)
        for (int x = 0; x < 512 * S; x++)
            if ((t[y * st + x] & 0x7FFF) == color) {
                c.count++;
                if (x < c.minx) c.minx = x;
                if (y < c.miny) c.miny = y;
                if (x > c.maxx) c.maxx = x;
                if (y > c.maxy) c.maxy = y;
            }
    return c;
}

#define RED 0x1F   /* rgb(255,0,0) -> 15-bit */

static void test_rect(int S)
{
    setup(S);
    gp0(0x600000FF);                 /* flat rect, variable size, red */
    gp0(xy(10, 20));
    gp0(xy(7, 5));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(S * 7 * S * 5), "S=%d rect count %u != %d", S, c.count, 35 * S * S);
    CHECK(c.minx == 10 * S && c.maxx == 17 * S - 1 && c.miny == 20 * S && c.maxy == 25 * S - 1,
          "S=%d rect bounds %d..%d x %d..%d", S, c.minx, c.maxx, c.miny, c.maxy);
}

static void test_tri(int S)
{
    setup(S);
    gp0(0x200000FF);                 /* flat triangle: right triangle with 8px legs */
    gp0(xy(20, 20));
    gp0(xy(28, 20));
    gp0(xy(20, 28));
    Cov c = coverage(RED);
    int n = 8 * S;
    CHECK(c.count == (u32)(n * (n + 1) / 2), "S=%d tri count %u != %d", S, c.count, n * (n + 1) / 2);
    CHECK(c.minx == 20 * S && c.miny == 20 * S && c.maxy == 28 * S - 1, "S=%d tri bounds", S);
    /* gouraud triangle: colours stay within the vertex range and the 1x VRAM is unaffected by S */
}

static void test_line(int S)
{
    setup(S);
    gp0(0x400000FF);                 /* flat line (10,5)-(20,5): 11 1x pixels long, S thick */
    gp0(xy(10, 5));
    gp0(xy(20, 5));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(11 * S * S), "S=%d hline count %u != %d", S, c.count, 11 * S * S);
    CHECK(c.minx == 10 * S && c.maxx == 21 * S - 1 && c.miny == 5 * S && c.maxy == 6 * S - 1,
          "S=%d hline bounds %d..%d %d..%d", S, c.minx, c.maxx, c.miny, c.maxy);
    setup(S);
    gp0(0x400000FF);                 /* vertical */
    gp0(xy(30, 10));
    gp0(xy(30, 17));
    c = coverage(RED);
    CHECK(c.count == (u32)(8 * S * S), "S=%d vline count %u != %d", S, c.count, 8 * S * S);
    CHECK(c.minx == 30 * S && c.maxx == 31 * S - 1 && c.miny == 10 * S && c.maxy == 18 * S - 1,
          "S=%d vline bounds", S);
    setup(S);
    gp0(0x400000FF);                 /* single point */
    gp0(xy(40, 40));
    gp0(xy(40, 40));
    c = coverage(RED);
    CHECK(c.count == (u32)(S * S), "S=%d point count %u", S, c.count);
}

static void test_fill_upload_copy(int S)
{
    setup(S);
    gp0(0x020000FF);                 /* fill (16,16) 32x8 */
    gp0(xy(16, 16));
    gp0(xy(32, 8));
    Cov c = coverage(RED);
    CHECK(c.count == (u32)(32 * 8 * S * S), "S=%d fill count %u", S, c.count);
    CHECK(c.minx == 16 * S && c.maxx == 48 * S - 1 && c.miny == 16 * S && c.maxy == 24 * S - 1,
          "S=%d fill bounds", S);

    gp0(0xA0000000);                 /* upload 4x2 green pixels to (100,100) */
    gp0(xy(100, 100));
    gp0(xy(4, 2));
    for (int i = 0; i < 4; i++) gp0(0x03E003E0);
    Cov g = coverage(0x3E0);
    CHECK(g.count == (u32)(8 * S * S), "S=%d upload count %u", S, g.count);
    CHECK(g.minx == 100 * S && g.maxx == 104 * S - 1 && g.miny == 100 * S && g.maxy == 102 * S - 1,
          "S=%d upload bounds", S);

    gp0(0x80000000);                 /* copy the 32x8 red block to (200,300) */
    gp0(xy(16, 16));
    gp0(xy(200, 300));
    gp0(xy(32, 8));
    c = coverage(RED);
    CHECK(c.count == (u32)(2 * 32 * 8 * S * S), "S=%d copy count %u", S, c.count);
    CHECK(c.maxx == 232 * S - 1 && c.maxy == 308 * S - 1, "S=%d copy bounds", S);

    /* mask bit: with mask-check on, a masked pixel is not overwritten in the target */
    gp0(0xE6000001);                 /* set mask bit on draw */
    gp0(0x60001F00);                 /* blue-ish rect 4x4 at (300,100) */
    gp0(xy(300, 100));
    gp0(xy(4, 4));
    gp0(0xE6000002);                 /* check mask */
    gp0(0x600000FF);
    gp0(xy(300, 100));
    gp0(xy(4, 4));
    gp0(0xE6000000);
    c = coverage(RED);
    CHECK(c.count == (u32)(2 * 32 * 8 * S * S), "S=%d masked rect must not draw (count %u)", S, c.count);
}

static u64 vram_hash(void)
{
    const u64 *w = (const u64 *)gpu_vram();
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
    setup(1);
    busy_stream();
    int fx, fy;
    u32 bad = hires_verify_1x(&fx, &fy);
    CHECK(bad == 0, "S=1 target differs from 1x VRAM in %u pixels (first %d,%d)", bad, fx, fy);
    u64 h1 = vram_hash();
    for (int S = 0; S <= 4; S += 2) {
        setup(S);
        busy_stream();
        CHECK(vram_hash() == h1, "1x VRAM differs at scale %d", S);
    }
}

int main(void)
{
    for (int S = 2; S <= 4; S += 2) {
        test_rect(S);
        test_tri(S);
        test_line(S);
        test_fill_upload_copy(S);
    }
    test_rect(1); test_tri(1); test_line(1); test_fill_upload_copy(1);
    test_s1_equivalence_and_invariance();
    CHECK(hires_pick_scale(1920, 1440, 8) == 4, "pick 1920x1440");
    CHECK(hires_pick_scale(960, 720, 8) == 2, "pick 960x720");
    CHECK(hires_pick_scale(640, 480, 8) == 2, "pick 640x480");
    CHECK(hires_pick_scale(512, 384, 8) == 1, "pick 512x384");
    CHECK(hires_pick_scale(3840, 2160, 8) == 6, "pick 4k (letterboxed 2880x2160)");
    CHECK(hires_pick_scale(3840, 2160, 3) == 3, "pick cap");
    printf("hires: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
