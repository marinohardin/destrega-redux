/* PGXP precision tracking (runtime/pgxp.[ch]): a precise GTE vertex must survive
 * GTE -> CPU register -> RAM -> register move -> RAM -> GP0 and reach the hi-res
 * target, must be dropped as soon as the integer word no longer matches, and must
 * never change what the guest sees from the GTE.
 * Build: clang -std=c11 -O2 -Wall -Iruntime runtime/gte.c runtime/gpu.c runtime/hires.c
 *        runtime/pgxp.c runtime/tests/test_pgxp.c -o build/test_pgxp -lm
 */
#include "psx.h"
#include "hires.h"
#include "pgxp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

u8 g_ram[RAM_SIZE];
u8 g_scratch[1024];
Cpu g_cpu;
u32 bus_read8(u32 a) { (void)a; return 0; }
u32 bus_read16(u32 a) { (void)a; return 0; }
u32 bus_read32(u32 a) { (void)a; return 0; }
void bus_write8(u32 a, u32 v) { (void)a; (void)v; }
void bus_write16(u32 a, u32 v) { (void)a; (void)v; }
void bus_write32(u32 a, u32 v) { (void)a; (void)v; }
void irq_raise(int bit) { (void)bit; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
               printf(__VA_ARGS__); printf("\n"); }                   \
    } while (0)

#define RTPS 0x4a180001u
#define RTPT 0x4a280030u

static void gte_setup(void)
{
    gte_init();
    gte_write_ctrl(0, 0x1000);   /* RT = identity (4.12) */
    gte_write_ctrl(1, 0);
    gte_write_ctrl(2, 0x1000);
    gte_write_ctrl(3, 0);
    gte_write_ctrl(4, 0x1000);
    gte_write_ctrl(5, 0); gte_write_ctrl(6, 0); gte_write_ctrl(7, 0);   /* TR */
    gte_write_ctrl(24, 160u << 16);   /* OFX */
    gte_write_ctrl(25, 120u << 16);   /* OFY */
    gte_write_ctrl(26, 200);          /* H */
}

static void set_v(int n, int x, int y, int z)
{
    gte_write_data(n * 2, ((u32)(u16)x) | ((u32)(u16)y << 16));
    gte_write_data(n * 2 + 1, (u32)z);
}

/* The CPU side of the path, as generated code does it: mfc2 rt, SXYn; sw rt, a */
static void mfc2_sw(int sxy_reg, int rt, u32 a)
{
    g_cpu.r[rt] = gte_read_data(sxy_reg); pgxp_mfc2(rt, sxy_reg);
    pgxp_sw(a, rt); mem_w32(a, g_cpu.r[rt]);
}

/* GP0 from RAM the way a DMA does it: each word with the tag of its RAM word */
static void gp0_from_ram(u32 a, int words)
{
    pgxp_new_batch();   /* one DMA = one welding batch */
    for (int i = 0; i < words; i++) {
        u32 w = a + 4u * (u32)i;
        gpu_write_gp0_tag(mem_r32(w), g_pgxp_mem[pgxp_slot(w)]);
    }
}

static void test_guest_identical(void)
{
    /* every guest-visible GTE register after RTPS/RTPT must be identical with tracking on/off */
    static const int vs[][3] = { { 101, 53, 997 }, { -300, 77, 211 }, { 5, -9, 60 }, { 3000, 2000, 50 },
                                 { 0, 0, 0 }, { -32000, 31000, 4000 }, { 17, 19, 65000 } };
    u32 res[2][64];
    for (int on = 0; on < 2; on++) {
        g_pgxp = on;
        gte_setup();
        int k = 0;
        for (size_t i = 0; i + 2 < sizeof vs / sizeof vs[0]; i++) {
            for (int n = 0; n < 3; n++) set_v(n, vs[i + n][0], vs[i + n][1], vs[i + n][2]);
            gte_command(i & 1 ? RTPT : RTPS);
        }
        for (int r = 0; r < 32; r++) res[on][k++] = gte_read_data(r);
        for (int r = 0; r < 32; r++) res[on][k++] = gte_read_ctrl(r);
    }
    CHECK(!memcmp(res[0], res[1], sizeof res[0]), "GTE registers differ with PGXP on");
}

static void test_precision_path(void)
{
    g_pgxp = true;
    pgxp_reset();
    gte_setup();
    /* sub-pixel projections: 160 + 101*200/997 = 180.26, 120 + 53*200/997 = 130.63 */
    set_v(0, 101, 53, 997);
    set_v(1, -41, 30, 1003);
    set_v(2, 20, -77, 1210);
    gte_command(RTPT);
    u32 w0 = gte_read_data(12);
    const PgxpVert *q = pgxp_lookup(g_pgxp_gte[0], w0);
    CHECK(q != NULL, "RTPT vertex 0 has no precise value");
    if (q) {
        double fx = 160.0 + 101.0 * 200.0 / 997.0, fy = 120.0 + 53.0 * 200.0 / 997.0;
        CHECK(fabs((s16)(w0 & 0xFFFF) + q->dx - fx) < 1e-3, "precise x %f", (s16)(w0 & 0xFFFF) + q->dx);
        CHECK(fabs((s16)(w0 >> 16) + q->dy - fy) < 1e-3, "precise y %f", (s16)(w0 >> 16) + q->dy);
        CHECK(fabs(q->w - 997.0f) < 1e-3, "w %f", q->w);
    }

    /* packet: F3 polygon at 0x80100000; vertex 0 travels GTE -> v0 -> RAM(stack) -> a1 (lw)
     * -> a2 (move) -> RAM(packet); vertices 1, 2 are stored with swc2 directly */
    const u32 pkt = 0x80100000u, stack = 0x1f800010u;   /* stack in the scratchpad */
    mem_w32(pkt, 0x20ff00ffu);
    pgxp_sw(pkt, 0);
    mfc2_sw(12, 2, stack);
    g_cpu.r[5] = mem_r32(stack); pgxp_lw(5, stack);
    g_cpu.r[6] = g_cpu.r[5]; pgxp_mov(6, 5);
    pgxp_sw(pkt + 4, 6); mem_w32(pkt + 4, g_cpu.r[6]);
    pgxp_swc2(pkt + 8, 13); mem_w32(pkt + 8, gte_read_data(13));
    pgxp_swc2(pkt + 12, 14); mem_w32(pkt + 12, gte_read_data(14));

    /* hi-res on: all three vertices reach the target precise */
    gpu_init();
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0, 0);
    hires_set_scale(2, 2);
    gpu_write_gp0(0xE3000000);
    gpu_write_gp0(0xE4000000 | (479u << 10) | 511u);
    gpu_write_gp0(0xE5000000);
    g_pgxp_stat_vtx = g_pgxp_stat_hit = 0;
    gp0_from_ram(pkt, 4);
    CHECK(g_pgxp_stat_vtx == 3 && g_pgxp_stat_hit == 3, "path: %llu of %llu precise (want 3/3)",
          (unsigned long long)g_pgxp_stat_hit, (unsigned long long)g_pgxp_stat_vtx);

    /* the game changes vertex 1's X with a halfword store: the tag no longer matches */
    mem_w16(pkt + 8, mem_r16(pkt + 8) + 1);
    g_pgxp_stat_vtx = g_pgxp_stat_hit = 0;
    gp0_from_ram(pkt, 4);
    CHECK(g_pgxp_stat_hit == 2, "modified word: %llu precise (want 2)", (unsigned long long)g_pgxp_stat_hit);

    /* an ALU result in a register that once held a vertex: stale tag, different value */
    g_cpu.r[6] = g_cpu.r[6] + 0x10001;   /* addiu-like, no hook */
    pgxp_sw(pkt + 4, 6); mem_w32(pkt + 4, g_cpu.r[6]);
    g_pgxp_stat_vtx = g_pgxp_stat_hit = 0;
    gp0_from_ram(pkt, 4);
    CHECK(g_pgxp_stat_hit == 1, "stale register: %llu precise (want 1)", (unsigned long long)g_pgxp_stat_hit);

    /* a plain store of an untracked register clears the word's association */
    g_cpu.r[7] = mem_r32(pkt + 12); g_pgxp_reg[7] = 0;
    pgxp_sw(pkt + 12, 7); mem_w32(pkt + 12, g_cpu.r[7]);
    g_pgxp_stat_vtx = g_pgxp_stat_hit = 0;
    gp0_from_ram(pkt, 4);
    CHECK(g_pgxp_stat_hit == 0, "cleared word: %llu precise (want 0)", (unsigned long long)g_pgxp_stat_hit);

    /* SXYP writes shift the tag FIFO; mtc2 of a tracked register re-attaches */
    gte_command(RTPS);
    u32 tagged = gte_read_data(14);
    g_cpu.r[8] = tagged; pgxp_mfc2(8, 14);
    gte_write_data(15, 0x00100010u); pgxp_mtc2(15, 0);         /* untracked push */
    CHECK(pgxp_lookup(g_pgxp_gte[1], gte_read_data(13)) != NULL, "FIFO shift lost the tag");
    CHECK(pgxp_lookup(g_pgxp_gte[2], gte_read_data(14)) == NULL, "untracked SXYP push has a tag");
    gte_write_data(12, tagged); pgxp_mtc2(12, 8);
    CHECK(pgxp_lookup(g_pgxp_gte[0], gte_read_data(12)) != NULL, "mtc2 did not carry the tag");

    /* tracking off: nothing is associated */
    g_pgxp = false;
    gte_setup();
    set_v(0, 101, 53, 997);
    gte_command(RTPS);
    CHECK(g_pgxp_gte[2] == 0, "tracking off still tags");
    hires_set_scale(0, 0);
}

static void test_perspective(void)
{
    /* a textured triangle whose three vertices have depth is textured perspective-correctly;
     * with one integer vertex it stays affine */
    g_pgxp = true;
    gpu_init();
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0, 0);
    hires_set_scale(2, 2);
    hires_set_perspective(true);
    gpu_write_gp0(0xE3000000);
    gpu_write_gp0(0xE4000000 | (479u << 10) | 511u);
    gpu_write_gp0(0xE5000000);
    u32 t[3];
    for (int i = 0; i < 3; i++) t[i] = pgxp_new(((u32)(20 + 30 * i) << 16) | (u32)(10 + 40 * i), 0.25f, 0.5f, 100.0f * (i + 1));
    u64 tris0, p0, tris1, p1;
    hires_get_persp_stats(&tris0, &p0);
    u32 pkt[7] = { 0x24808080u, (20u << 16) | 10u, 0x00000000u, (50u << 16) | 50u, 0x00000020u, (80u << 16) | 90u, 0x00002020u };
    u32 tags[7] = { 0, t[0], 0, t[1], 0, t[2], 0 };
    for (int i = 0; i < 7; i++) gpu_write_gp0_tag(pkt[i], tags[i]);
    hires_get_persp_stats(&tris1, &p1);
    CHECK(tris1 - tris0 == 1 && p1 - p0 == 1, "perspective triangle not counted (%llu, %llu)",
          (unsigned long long)(tris1 - tris0), (unsigned long long)(p1 - p0));
    tags[5] = 0;
    pgxp_new_batch();
    for (int i = 0; i < 7; i++) gpu_write_gp0_tag(pkt[i], tags[i]);
    hires_get_persp_stats(&tris0, &p0);
    CHECK(tris0 - tris1 == 1 && p0 - p1 == 0, "mixed triangle textured perspective-correctly");
    HiresUvStats uv;
    hires_get_uv_stats(&uv);
    CHECK(uv.escaped == 0, "%llu UV samples outside the clamp", (unsigned long long)uv.escaped);
    hires_set_scale(0, 0);
    g_pgxp = false;
}

static void test_weld(void)
{
    /* two primitives in one batch share an SXY word but carry different precise values (distinct
     * GTE results that round to the same pixel): both must be drawn at one position (the first
     * value), or the mesh cracks; a new batch starts over */
    g_pgxp = true;
    gpu_init();
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0, 0);
    hires_set_scale(2, 2);
    gpu_write_gp0(0xE3000000);
    gpu_write_gp0(0xE4000000 | (479u << 10) | 511u);
    const u32 shared = (40u << 16) | 30u;
    u32 a = pgxp_new(shared, 0.2f, 0.3f, 500.0f), b = pgxp_new(shared, 0.8f, 0.7f, 510.0f);
    u32 o1 = pgxp_new((10u << 16) | 10u, 0.5f, 0.5f, 500.0f), o2 = pgxp_new((60u << 16) | 12u, 0.5f, 0.5f, 500.0f);
    u32 o3 = pgxp_new((70u << 16) | 50u, 0.5f, 0.5f, 500.0f);
    pgxp_new_batch();
    u64 w0 = g_pgxp_stat_welded, s0 = g_pgxp_stat_split;
    u32 t1[4] = { 0x20808080u, (10u << 16) | 10u, shared, (60u << 16) | 12u };
    u32 g1[4] = { 0, o1, a, o2 };
    u32 t2[4] = { 0x20808080u, shared, (70u << 16) | 50u, (60u << 16) | 12u };
    u32 g2[4] = { 0, b, o3, o2 };
    for (int i = 0; i < 4; i++) gpu_write_gp0_tag(t1[i], g1[i]);
    for (int i = 0; i < 4; i++) gpu_write_gp0_tag(t2[i], g2[i]);
    CHECK(g_pgxp_stat_welded - w0 == 1, "shared word not welded (%llu)", (unsigned long long)(g_pgxp_stat_welded - w0));
    CHECK(g_pgxp_stat_split == s0, "shared word sent at two positions");
    const PgxpVert *q = pgxp_weld(shared, pgxp_lookup(b, shared));
    CHECK(q && q->dx == 0.2f && q->dy == 0.3f, "weld did not keep the first value");
    pgxp_new_batch();
    q = pgxp_weld(shared, pgxp_lookup(b, shared));
    CHECK(q && q->dx == 0.8f, "weld leaked across batches");
    /* a word first sent as an integer vertex stays integer for the rest of the batch */
    pgxp_new_batch();
    CHECK(pgxp_weld(shared, NULL) == NULL && pgxp_weld(shared, pgxp_lookup(a, shared)) == NULL,
          "integer-first word got a precise value");
    hires_set_scale(0, 0);
    g_pgxp = false;
}

int main(void)
{
    test_guest_identical();
    test_precision_path();
    test_perspective();
    test_weld();
    printf("test_pgxp: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
