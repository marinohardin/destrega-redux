/* Self-checking tests for runtime/gpu.c.
 * Build: clang -std=c11 -O2 -Wall -Iruntime runtime/gpu.c runtime/tests/test_gpu.c -o build/test_gpu
 */
#define _POSIX_C_SOURCE 199309L
#include "psx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_irq_count;
void irq_raise(int bit) { if (bit == IRQ_GPU) g_irq_count++; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
               printf(__VA_ARGS__); printf("\n"); }                   \
    } while (0)

static u16 px(int x, int y) { return gpu_vram()[(y & 511) * 1024 + (x & 1023)]; }
static void gp0(u32 v) { gpu_write_gp0(v); }
static u32 xy(int x, int y) { return ((u32)(y & 0xFFFF) << 16) | (u32)(x & 0xFFFF); }
static u16 rgb15(int r, int g, int b) { return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

static void set_full_draw_area(void)
{
    gp0(0xE3000000);
    gp0(0xE4000000 | (511u << 10) | 1023u);
    gp0(0xE5000000);
}

static void clear_vram(void) { memset(gpu_vram(), 0, 1024 * 512 * 2); }

static void test_stat(void)
{
    gpu_init();
    u32 st = gpu_read_stat();
    CHECK((st & (1u << 26)) && (st & (1u << 28)), "ready bits 26/28 stat=%08x", st);
    CHECK(!(st & (1u << 27)), "bit 27 clear without pending read stat=%08x", st);
    CHECK(st & (1u << 23), "display disabled after reset");
    CHECK(!(st & (1u << 25)), "bit25 = 0 for dma dir 0");
    gpu_write_gp1(0x04000002);
    st = gpu_read_stat();
    CHECK(((st >> 29) & 3) == 2 && (st & (1u << 25)), "dma dir 2 stat=%08x", st);
    gpu_write_gp1(0x03000000);
    CHECK(!(gpu_read_stat() & (1u << 23)), "display enable");
    gp0(0xE1000000 | 0x7FF);
    CHECK((gpu_read_stat() & 0x7FF) == 0x7FF, "drawmode mirrored in stat");
    gp0(0xE6000003);
    CHECK(((gpu_read_stat() >> 11) & 3) == 3, "mask bits in stat");
    gpu_write_gp1(0x08000000 | 0x01 | 0x04 | 0x20 | 0x10);  /* 320, 480i, 24bit */
    GpuDisplay d;
    gpu_write_gp1(0x05000000 | (16u << 10) | 64);
    gpu_get_display(&d);
    CHECK(d.w == 320 && d.h == 480 && d.rgb24 && d.enabled && d.x == 64 && d.y == 16,
          "display %d %d %d %d %d %d", d.x, d.y, d.w, d.h, d.rgb24, d.enabled);
    /* interlace field toggles per vblank */
    u32 a = gpu_read_stat() >> 31;
    gpu_vblank();
    u32 b = gpu_read_stat() >> 31;
    CHECK(a != b, "bit31 toggles on vblank in interlace");
    gpu_write_gp1(0x08000040);
    gpu_get_display(&d);
    CHECK(d.w == 368 && d.h == 240 && !d.rgb24, "368 mode %d %d", d.w, d.h);
    /* GP1(10h) info */
    gp0(0xE2012345 & 0xE20FFFFF);
    gp0(0xE3000000 | (5u << 10) | 7);
    gp0(0xE4000000 | (200u << 10) | 300);
    gp0(0xE5000000 | ((0x7FFu & (u32)-3) << 11) | 10);
    gpu_write_gp1(0x10000002); CHECK(gpu_read_data() == 0x12345, "info texwin");
    gpu_write_gp1(0x10000003); CHECK(gpu_read_data() == ((5u << 10) | 7), "info TL");
    gpu_write_gp1(0x10000004); CHECK(gpu_read_data() == ((200u << 10) | 300), "info BR");
    gpu_write_gp1(0x10000005); CHECK(gpu_read_data() == (((0x7FFu & (u32)-3) << 11) | 10), "info offset");
    gpu_write_gp1(0x10000007); CHECK(gpu_read_data() == 2, "info version");
    /* IRQ */
    g_irq_count = 0;
    gp0(0x1F000000);
    CHECK(g_irq_count == 1 && (gpu_read_stat() & (1u << 24)), "gp0 1f irq");
    gpu_write_gp1(0x02000000);
    CHECK(!(gpu_read_stat() & (1u << 24)), "irq ack");
    gpu_write_gp1(0x00000000);
    CHECK((gpu_read_stat() & 0x00FFFFFF & ~(1u << 13)) == (0x14802000 & 0x00FFFFFF & ~(1u << 13)), "reset stat %08x", gpu_read_stat());
}

static void test_fill(void)
{
    gpu_init();
    gp0(0x020000FF);       /* red */
    gp0(xy(20, 10));       /* x rounds down to 16 */
    gp0(xy(10, 3));        /* w rounds up to 16 */
    u16 red = rgb15(255, 0, 0);
    CHECK(px(15, 10) == 0 && px(16, 10) == red && px(31, 12) == red && px(32, 10) == 0 && px(16, 13) == 0,
          "fill rect bounds");
    /* fill ignores draw area / offset */
    gp0(0xE5000000 | 100);
    gp0(0x02FF0000);
    gp0(xy(1008, 0));      /* wraps horizontally */
    gp0(xy(32, 1));
    u16 blue = rgb15(0, 0, 255);
    CHECK(px(1008, 0) == blue && px(1023, 0) == blue && px(0, 0) == blue && px(15, 0) == blue && px(16, 0) == 0,
          "fill wraps");
}

static void test_transfer(void)
{
    gpu_init();
    u16 src[9];
    for (int i = 0; i < 9; i++) src[i] = (u16)(0x1000 + i * 0x111);
    gp0(0xA0000000);
    gp0(xy(1022, 511));    /* wraps both axes */
    gp0(xy(3, 3));
    for (int i = 0; i < 9; i += 2) gp0(src[i] | (i + 1 < 9 ? (u32)src[i + 1] << 16 : 0xDEAD0000u));
    CHECK(px(1022, 511) == src[0] && px(1024, 511) == src[2] && px(0, 0) == src[5] && px(0, 1) == src[8],
          "cpu->vram wrap");
    /* next command must be processed normally (odd count consumed exactly) */
    gp0(0x02FFFFFF); gp0(xy(64, 64)); gp0(xy(16, 1));
    CHECK(px(64, 64) == 0x7FFF, "command after odd transfer");

    gp0(0xC0000000);
    gp0(xy(1022, 511));
    gp0(xy(3, 3));
    CHECK(gpu_read_stat() & (1u << 27), "bit27 during vram->cpu");
    u16 got[10];
    for (int i = 0; i < 10; i += 2) {
        u32 w = gpu_read_data();
        got[i] = (u16)w;
        got[i + 1] = (u16)(w >> 16);
    }
    CHECK(memcmp(got, src, sizeof src) == 0, "vram->cpu readback");
    CHECK(!(gpu_read_stat() & (1u << 27)), "bit27 cleared after read");

    /* vram->vram copy */
    gp0(0x80000000); gp0(xy(1022, 511)); gp0(xy(200, 100)); gp0(xy(3, 3));
    CHECK(px(200, 100) == src[0] && px(202, 102) == src[8], "vram->vram copy");

    /* mask set applies to cpu->vram */
    gp0(0xE6000001);
    gp0(0xA0000000); gp0(xy(300, 300)); gp0(xy(2, 1)); gp0(0x00020001);
    CHECK(px(300, 300) == 0x8001 && px(301, 300) == 0x8002, "mask set on upload");
    gp0(0xE6000002);
    gp0(0xA0000000); gp0(xy(300, 300)); gp0(xy(2, 1)); gp0(0x00040003);
    CHECK(px(300, 300) == 0x8001, "mask check blocks upload");
}

static void test_flat_triangle(void)
{
    gpu_init();
    set_full_draw_area();
    gp0(0x20FFFFFF);
    gp0(xy(0, 0)); gp0(xy(10, 0)); gp0(xy(0, 10));
    int count = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) count += px(x, y) != 0;
    CHECK(count == 55, "triangle pixel count %d", count);
    CHECK(px(9, 0) && !px(10, 0) && px(0, 9) && !px(0, 10) && px(4, 5) && !px(5, 5), "top-left rule");

    /* quad 0-1-2 / 1-2-3: full square without seam or overlap */
    clear_vram();
    gp0(0xE1000000 | (1u << 5));   /* mode 1 additive */
    gp0(0x2A000080);
    gp0(xy(100, 100)); gp0(xy(120, 100)); gp0(xy(100, 120)); gp0(xy(120, 120));
    int bad = 0, cov = 0;
    for (int y = 98; y < 123; y++)
        for (int x = 98; x < 123; x++) {
            u16 p = px(x, y);
            bool in = x >= 100 && x < 120 && y >= 100 && y < 120;
            if (in) { cov++; if (p != rgb15(0x80, 0, 0)) bad++; }
            else if (p) bad++;
        }
    CHECK(cov == 400 && bad == 0, "quad coverage bad=%d", bad);

    /* draw area clip + offset */
    clear_vram();
    gp0(0xE3000000 | (50u << 10) | 50);
    gp0(0xE4000000 | (59u << 10) | 59);
    gp0(0xE5000000 | (40u << 11) | 40);
    gp0(0x2800FF00);
    gp0(xy(0, 0)); gp0(xy(100, 0)); gp0(xy(0, 100)); gp0(xy(100, 100));
    count = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 200; x++) count += px(x, y) != 0;
    CHECK(count == 100 && px(50, 50) && px(59, 59) && !px(60, 59), "clip count=%d", count);

    /* oversize rejection */
    clear_vram();
    set_full_draw_area();
    gp0(0x20FFFFFF);
    gp0(xy(-1, 0)); gp0(xy(1023, 5)); gp0(xy(0, 10));
    CHECK(px(1, 1) == 0, "polygon width>=1024 rejected");
}

static void test_gouraud(void)
{
    gpu_init();
    set_full_draw_area();
    gp0(0x300000FF); gp0(xy(0, 0));
    gp0(0x000000FF); gp0(xy(64, 0));
    gp0(0x00FF0000); gp0(xy(0, 64));
    u16 a = px(0, 0), b = px(1, 60);
    CHECK((a & 0x1F) == 31 && ((a >> 10) & 0x1F) == 0, "gouraud top red %04x", a);
    CHECK(((b >> 10) & 0x1F) > 25, "gouraud bottom blue %04x", b);
}

static void upload(int x, int y, int w, int h, const u16 *data)
{
    gp0(0xA0000000); gp0(xy(x, y)); gp0(xy(w, h));
    int n = w * h;
    for (int i = 0; i < n; i += 2) gp0(data[i] | (i + 1 < n ? (u32)data[i + 1] << 16 : 0));
}

static void test_textured(void)
{
    gpu_init();
    set_full_draw_area();
    u16 clut[16];
    clut[0] = 0;
    for (int i = 1; i < 16; i++) clut[i] = (u16)(i * 0x0421);  /* gray ramp */
    upload(0, 480, 16, 1, clut);
    /* 4bpp 16x16 texture at texpage 10 (x=640): texel(u,v) = (u + v) & 15 */
    u16 tex[4 * 16];
    for (int v = 0; v < 16; v++)
        for (int hw = 0; hw < 4; hw++) {
            u16 w = 0;
            for (int n = 0; n < 4; n++) w |= (u16)(((hw * 4 + n + v) & 15) << (n * 4));
            tex[v * 4 + hw] = w;
        }
    upload(640, 0, 4, 16, tex);
    gp0(0xE100000A);             /* texpage x=640, 4bpp */
    u32 clutw = (480u << 6) | 0;
    gp0(0x65808080);             /* raw textured variable rect */
    gp0(xy(100, 100));
    gp0((clutw << 16) | 0);
    gp0(xy(16, 16));
    int bad = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            u16 exp = clut[(x + y) & 15];
            if (px(100 + x, 100 + y) != exp) bad++;
        }
    CHECK(bad == 0, "4bpp rect raw texture bad=%d", bad);
    CHECK(px(100, 100) == 0 && px(101, 100) == clut[1], "texel 0 transparent");

    /* x-flip */
    gp0(0xE100000A | (1u << 12));
    gp0(0x65808080); gp0(xy(200, 100)); gp0((clutw << 16) | 15); gp0(xy(16, 1));
    bad = 0;
    for (int x = 0; x < 16; x++) if (px(200 + x, 100) != clut[(15 - x) & 15]) bad++;
    CHECK(bad == 0, "x-flip rect bad=%d", bad);

    /* modulated textured triangle-pair quad == rect output when color 0x80 */
    gp0(0xE100000A);
    gp0(0x2C808080);
    gp0(xy(300, 100)); gp0((clutw << 16) | 0x0000);
    gp0(xy(316, 100)); gp0((0x000Au << 16) | 0x0010);
    gp0(xy(300, 116)); gp0(0x1000);
    gp0(xy(316, 116)); gp0(0x1010);
    bad = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (px(300 + x, 100 + y) != clut[(x + y) & 15]) bad++;
    CHECK(bad == 0, "textured quad affine mapping bad=%d", bad);

    /* modulation by half brightness */
    gp0(0x64404040); gp0(xy(400, 100)); gp0((clutw << 16) | 15); gp0(xy(1, 1));
    CHECK(px(400, 100) == (u16)((15 / 2) * 0x0421), "modulation %04x", px(400, 100));

    /* 15bpp texture + texture window */
    u16 t15[4] = { 0x001F, 0x03E0, 0x7C00, 0x7FFF };
    upload(768, 256, 4, 1, t15);
    gp0(0xE1000000 | 12 | 16 | (2u << 7));
    gp0(0xE2000000 | 0x1F);      /* mask x = 0x1F*8 -> u & 7 */
    gp0(0x65000000); gp0(xy(500, 100)); gp0(0); gp0(xy(16, 1));
    CHECK(px(500, 100) == 0x001F && px(503, 100) == 0x7FFF && px(508, 100) == 0x001F, "texture window / 15bpp");
    gp0(0xE2000000);
}

static void test_semi(void)
{
    gpu_init();
    set_full_draw_area();
    gp0(0x02000000 | 0x0000F8); gp0(xy(0, 0)); gp0(xy(32, 8));   /* bg red 31 */
    gp0(0xE1000000);   /* blend mode 0 */
    gp0(0x6200F800); gp0(xy(0, 0)); gp0(xy(4, 4));                /* green 31 */
    u16 p = px(1, 1);
    CHECK((p & 0x1F) == 15 && ((p >> 5) & 0x1F) == 15 && (p >> 10) == 0, "semi mode 0 %04x", p);
    gp0(0xE1000000 | (2u << 5));   /* B-F */
    gp0(0x62000080); gp0(xy(8, 0)); gp0(xy(4, 4));
    CHECK((px(9, 1) & 0x1F) == 15, "semi mode 2 %04x", px(9, 1));
    gp0(0xE1000000 | (3u << 5));   /* B+F/4 */
    gp0(0x62F80000); gp0(xy(16, 0)); gp0(xy(4, 4));
    CHECK(px(17, 1) == (31 | (7 << 10)), "semi mode 3 %04x", px(17, 1));
    /* mask check: set mask, then draw over with check */
    gp0(0xE6000001);
    gp0(0x60FFFFFF); gp0(xy(24, 0)); gp0(xy(2, 2));
    CHECK(px(24, 0) == 0xFFFF, "mask set on draw");
    gp0(0xE6000002);
    gp0(0x60000000 | 0xF8); gp0(xy(24, 0)); gp0(xy(4, 1));
    CHECK(px(24, 0) == 0xFFFF && px(26, 0) == 0x001F, "mask check");
}

static void test_lines(void)
{
    gpu_init();
    set_full_draw_area();
    gp0(0x40FFFFFF); gp0(xy(10, 10)); gp0(xy(20, 10));
    int c = 0;
    for (int x = 0; x < 30; x++) c += px(x, 10) != 0;
    CHECK(c == 11 && px(10, 10) && px(20, 10), "horizontal line %d", c);
    /* polyline terminated, then a normal command */
    gp0(0x48FF0000); gp0(xy(0, 50)); gp0(xy(10, 60)); gp0(xy(20, 50)); gp0(0x55555555);
    gp0(0x02FFFFFF); gp0(xy(64, 64)); gp0(xy(16, 1));
    CHECK(px(0, 50) && px(10, 60) && px(20, 50) && px(5, 55), "polyline vertices");
    CHECK(px(64, 64) == 0x7FFF, "command after polyline");
    /* gouraud polyline */
    gp0(0x580000FF); gp0(xy(100, 0)); gp0(0x0000FF00); gp0(xy(110, 0)); gp0(0x00FF0000); gp0(xy(110, 10));
    gp0(0x50005000);
    CHECK((px(100, 0) & 0x1F) == 31 && ((px(110, 0) >> 5) & 0x1F) >= 30 && (px(110, 10) >> 10) == 31, "gouraud polyline");
    /* unknown commands consumed */
    gp0(0xFF123456); gp0(0x0F000000); gp0(0xE7000000);
    gp0(0x02FFFFFF); gp0(xy(128, 64)); gp0(xy(16, 1));
    CHECK(px(128, 64) == 0x7FFF, "unknown commands");
}

static void scene_and_bench(void)
{
    gpu_init();
    set_full_draw_area();
    gp0(0xE3000000);
    gp0(0xE4000000 | (239u << 10) | 319);
    gp0(0x02201008); gp0(xy(0, 0)); gp0(xy(320, 240));
    /* checker 8bpp texture at x=640 with 256-entry clut at y=500 */
    static u16 clut[256], tex[128 * 64];
    for (int i = 0; i < 256; i++) clut[i] = (u16)(((i & 31)) | (((i >> 3) & 31) << 5) | ((31 - (i & 31)) << 10) | (i ? 0 : 0));
    clut[0] = 0x0001;
    upload(0, 500, 256, 1, clut);
    for (int v = 0; v < 64; v++)
        for (int hw = 0; hw < 32; hw++) {
            int u0 = hw * 2;
            int a = (((u0 >> 3) ^ (v >> 3)) & 1) ? 200 : 40 + u0;
            int b = (((u0 + 1) >> 3 ^ (v >> 3)) & 1) ? 200 : 41 + u0;
            tex[v * 128 + hw] = (u16)(a | (b << 8));
        }
    upload(640, 0, 128, 64, tex);
    u32 clutw = (500u << 6);
    u32 tpage = 10 | (1u << 7) | (1u << 9);   /* 8bpp, dither */
    gp0(0xE1000000 | tpage);
    /* gouraud textured quad */
    gp0(0x3C0000FF); gp0(xy(20, 20)); gp0((clutw << 16) | 0x0000);
    gp0(0x0000FF00); gp0(xy(140, 30)); gp0((tpage << 16) | 0x003F);
    gp0(0x00FF0000); gp0(xy(10, 140)); gp0(0x3F00);
    gp0(0x00FFFFFF); gp0(xy(150, 130)); gp0(0x3F3F);
    /* gouraud untextured triangle */
    gp0(0x30FF00FF); gp0(xy(180, 20)); gp0(0x0000FFFF); gp0(xy(300, 60)); gp0(0x00FFFF00); gp0(xy(200, 200));
    /* semi transparent rect */
    gp0(0xE1000000 | tpage);
    gp0(0x62FFFFFF); gp0(xy(100, 100)); gp0(xy(120, 60));
    /* lines */
    for (int i = 0; i < 10; i++) { gp0(0x5000FFFF); gp0(xy(10, 200 + i * 3)); gp0(0x00FF00FF); gp0(xy(300, 160 + i * 7)); }

    FILE *f = fopen("build/gpu_test.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n320 240\n255\n");
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 320; x++) {
                u16 p = px(x, y);
                u8 rgb[3] = { (u8)((p & 31) << 3), (u8)(((p >> 5) & 31) << 3), (u8)(((p >> 10) & 31) << 3) };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        printf("wrote build/gpu_test.ppm\n");
    }

    /* benchmark: 3000 textured gouraud dithered triangles (~200px each, ~8x a 320x240 screen) */
    const int N = 3000;
    double best = 1e9;
    for (int rep = 0; rep < 8; rep++) {
        struct timespec t0, t1;
        timespec_get(&t0, TIME_UTC);   /* C11 (MinGW has no clock_gettime without winpthread) */
        unsigned seed = 1;
        for (int i = 0; i < N; i++) {
            seed = seed * 1103515245 + 12345;
            int x = (int)((seed >> 8) % 280), y = (int)((seed >> 16) % 200);
            gp0(0x34808080); gp0(xy(x, y)); gp0((clutw << 16) | 0);
            gp0(0x00606060); gp0(xy(x + 20, y + 2)); gp0((tpage << 16) | 0x003F);
            gp0(0x00A0A0A0); gp0(xy(x + 1, y + 20)); gp0(0x3F00);
        }
        timespec_get(&t1, TIME_UTC);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ms < best) best = ms;
    }
    double ms = best;
    printf("bench: %d textured gouraud dithered triangles (~200px) in %.3f ms (best of 8)\n", N, ms);
}

/* The fixed 1x skip rule (gpu.c header): with the draw area inside the 512x480 framebuffer
 * region, primitives are not drawn into the emulated VRAM; fills and transfers still are, and
 * the optional display copy holds exactly the picture full drawing gives. */
static void skip_stream(void)
{
    gp0(0xE3000000); gp0(0xE4000000 | (479u << 10) | 511u); gp0(0xE5000000);
    gp0(0x02102030); gp0(xy(0, 200)); gp0(xy(64, 32));                /* fill */
    gp0(0x2000FF00); gp0(xy(10, 10)); gp0(xy(60, 12)); gp0(xy(20, 50)); /* flat tri */
    gp0(0x600000FF); gp0(xy(100, 100)); gp0(xy(16, 8));              /* rect */
    gp0(0x40FF0000); gp0(xy(200, 5)); gp0(xy(300, 40));              /* line */
    gp0(0xA0000000); gp0(xy(400, 300)); gp0(xy(2, 1)); gp0(0x7FFF001F); /* upload */
    gp0(0xE4000000 | (511u << 10) | 1023u);                         /* area outside the region */
    gp0(0x2000FF00); gp0(xy(600, 10)); gp0(xy(650, 12)); gp0(xy(610, 50));
}

static void test_skip_rule(void)
{
    static u16 full[1024 * 512], skip[1024 * 512];
    gpu_init(); gpu_set_skip_rule(false); skip_stream();
    memcpy(full, gpu_vram(), sizeof full);
    gpu_set_skip_rule(true);
    gpu_init(); skip_stream();
    memcpy(skip, gpu_vram(), sizeof skip);
    CHECK(full[20 * 1024 + 25] != 0 && skip[20 * 1024 + 25] == 0, "triangle in the region skipped");
    CHECK(full[104 * 1024 + 108] != 0 && skip[104 * 1024 + 108] == 0, "rect in the region skipped");
    CHECK(skip[216 * 1024 + 32] == full[216 * 1024 + 32] && skip[216 * 1024 + 32] != 0, "fill still written");
    CHECK(skip[300 * 1024 + 401] == 0x7FFF, "upload still written");
    CHECK(skip[20 * 1024 + 615] != 0 && skip[20 * 1024 + 615] == full[20 * 1024 + 615], "outside the region drawn");
    int other = 0;
    for (int y = 0; y < 512; y++)
        for (int x = 512; x < 1024; x++) other += full[y * 1024 + x] != skip[y * 1024 + x];
    CHECK(other == 0, "texture area identical (%d differ)", other);
    /* a guest read of skipped pixels is reported (no display copy: unverified) */
    u64 u0 = gpu_1x_observed(2);
    gp0(0xC0000000); gp0(xy(20, 20)); gp0(xy(2, 1)); (void)gpu_read_data();
    CHECK(gpu_1x_observed(2) == u0 + 1, "VRAM->CPU read of skipped pixels observed");
    /* display copy = full drawing; emulated VRAM unchanged by it */
    gpu_init(); gpu_enable_display_copy(); skip_stream();
    CHECK(memcmp(gpu_display_vram(), full, sizeof full) == 0, "display copy equals full drawing");
    CHECK(memcmp(gpu_vram(), skip, sizeof skip) == 0, "emulated VRAM independent of the display copy");
    u64 d0 = gpu_1x_observed(1);
    gp0(0xC0000000); gp0(xy(20, 20)); gp0(xy(2, 1)); (void)gpu_read_data();
    CHECK(gpu_1x_observed(1) == d0 + 1, "differing read observed with the copy");
}

int main(void)
{
    gpu_set_skip_rule(false);   /* rasterizer tests use small draw areas; test_skip_rule turns it on */
    test_stat();
    test_fill();
    test_transfer();
    test_flat_triangle();
    test_gouraud();
    test_textured();
    test_semi();
    test_lines();
    scene_and_bench();
    test_skip_rule();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
