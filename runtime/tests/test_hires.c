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
