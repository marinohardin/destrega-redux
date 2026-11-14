/* The native high-level renderer's phase 2a core (runtime/hlr/), without the disc:
 *
 *  - switches: --hlr / --hlr-parts parsing and names;
 *  - stage decoding: a synthetic stage (one object of flat quads in the game's layout: root,
 *    command list, vertices, packet buffer, scene ordering table) is predicted by hlr_stage.c
 *    while a stand-in for the game builds it with the guest GTE; the audit must find it exact,
 *    and must notice a wrong word, an unexplained packet and a missing one;
 *  - capture and pairing: a clean frame becomes an HlrFrame (a copy), the linked-list filter
 *    activates it on an OT node, replays its packets, refuses stale ones, and the walk's end
 *    consumes it; a save-state load (on_restore) and a mode change drop captures;
 *  - suppression: a packet drawn by gpu_hires_replay and then sent with gpu_set_node_no_hires
 *    leaves exactly the hi-res target the plain path leaves; suppression alone draws nothing;
 *  - float camera (phase 2b, hlr_cam.h): random cameras in the game's form (rotation with the 0.625
 *    Y scale, translation, H, drawing offset) and vertices through the guest GTE's RTPS; the float
 *    projection and the clip-space mapping of a 4x6 widescreen pass must land within 0.75 1x pixel
 *    of SX/SY.
 */
#include "psx.h"
#include "hires.h"
#include "pgxp.h"
#include "hlr/hlr.h"
#include "hlr_hook.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8 g_ram[RAM_SIZE];
u8 g_scratch[1024];
Cpu g_cpu;
HlrHooks g_hlr;                   /* bus.c in the game */
u32 bus_read8(u32 a) { (void)a; return 0; }
u32 bus_read16(u32 a) { (void)a; return 0; }
u32 bus_read32(u32 a) { (void)a; return 0; }
void bus_write8(u32 a, u32 v) { (void)a; (void)v; }
void bus_write16(u32 a, u32 v) { (void)a; (void)v; }
void bus_write32(u32 a, u32 v) { (void)a; (void)v; }
u32 g_mem_wlimit = 0x800000;
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
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(1);
}

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
               printf(__VA_ARGS__); printf("\n"); }                   \
    } while (0)

static inline u32 rd(u32 a) { return *(u32 *)(g_ram + (a & 0x1ffffc)); }
static inline void wr(u32 a, u32 v) { *(u32 *)(g_ram + (a & 0x1ffffc)) = v; }
static inline void wr16(u32 a, u32 v) { *(u16 *)(g_ram + (a & 0x1ffffe)) = (u16)v; }

/* ---- switches ------------------------------------------------------------------------------- */
static void test_switches(void)
{
    CHECK(hlr_parse_mode("off") == HLR_OFF && hlr_parse_mode("on") == HLR_ON, "modes");
    CHECK(hlr_parse_mode("exact") == HLR_EXACT && hlr_parse_mode("audit") == HLR_AUDIT, "modes");
    CHECK(hlr_parse_mode("native") < 0 && hlr_parse_mode(NULL) < 0, "bad mode refused");
    unsigned p = 99;
    CHECK(hlr_parse_parts("stage", &p) && p == HLR_PART_STAGE, "parts stage");
    CHECK(hlr_parse_parts("stage,chars", &p) && p == (HLR_PART_STAGE | HLR_PART_CHARS), "parts list");
    CHECK(hlr_parse_parts("all", &p) && p == HLR_PARTS_ALL, "parts all");
    CHECK(hlr_parse_parts("none", &p) && p == 0, "parts none");
    CHECK(!hlr_parse_parts("stage,sky", &p), "unknown part refused");
    CHECK(!strcmp(hlr_parts_name(HLR_PART_STAGE | HLR_PART_FX), "stage,fx"), "parts name %s",
          hlr_parts_name(HLR_PART_STAGE | HLR_PART_FX));
    hlr_set_mode(HLR_OFF);
    CHECK(!g_hlr.stage_pre && !g_hlr.ll_filter && !g_hlr.on_restore, "off installs no hook");
    hlr_set_mode(HLR_AUDIT);
    CHECK(g_hlr.stage_pre && !g_hlr.ll_filter && g_hlr.on_restore, "audit: stage hooks, no filter");
    hlr_set_mode(HLR_EXACT);
    CHECK(g_hlr.stage_pre && g_hlr.ll_filter, "exact: filter installed");
    hlr_set_mode(HLR_OFF);
}

/* ---- the synthetic stage ---------------------------------------------------------------------- */
#define GP        0x800bfde8u
#define ROOT      0x80080000u
#define PKTBUF    0x80090000u
#define OT        0x80100000u
#define NQUAD     4

static void gte_camera(void)
{
    gte_init();
    /* identity rotation, no translation (hlr_stage.c loads the same from 800ed4c8) */
    const u32 rt[8] = { 0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 0 };
    for (int i = 0; i < 8; i++) gte_write_ctrl(i, rt[i]);
    gte_write_ctrl(24, 0);                /* OFX, OFY 0: the game's screen is centred on 0 */
    gte_write_ctrl(25, 0);
    gte_write_ctrl(26, 300);              /* H */
}

/* one object (index 0: always drawn) of NQUAD independent flat quads (command 0x28) */
static void build_stage(void)
{
    memset(g_ram, 0, RAM_SIZE);
    memset(g_scratch, 0, sizeof g_scratch);
    wr(GP + 0x5d0, ROOT);                         /* stage root */
    wr(GP + 0x5cc, 0x800a0000u);                  /* normal / colour stream base */
    wr(GP + 0x5e8, PKTBUF);                       /* packet buffer of double-buffer index 0 */
    wr(0x800c0724u, OT);                          /* scene OT of index 0 (scratch 1f800000 = 0) */
    *(u32 *)(g_scratch + 0x0c) = 1000;            /* near / mid / far depth ranges of the OT slots */
    *(u32 *)(g_scratch + 0x10) = 4000;
    *(u32 *)(g_scratch + 0x08) = 16000;
    /* identity rotation, no translation (800ed4c8), lights zero */
    const u32 rt[8] = { 0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 0 };
    for (int i = 0; i < 8; i++) wr(0x800ed4c8u + 4 * (u32)i, rt[i]);
    for (u32 i = 0; i < 2048; i++) wr(OT + 4 * i, 0x00ffffffu);
    wr(ROOT, 1);                                  /* one object */
    const u32 ent = ROOT + 4;
    wr(ent + 0, 0x40);                            /* data at ROOT + 0x40 */
    wr(ent + 4, 0);                               /* packets at PKTBUF + 0 */
    wr(ent + 8, 0);
    wr(ent + 12, 0);                              /* flags */
    const u32 data = ROOT + 0x40;
    wr(data, 1);                                  /* one command */
    wr(data + 4, (u32)NQUAD << 8 | 0x28);         /* F4 x NQUAD, no strips */
    u32 v = data + 8;
    for (int q = 0; q < NQUAD; q++) {
        /* quad q: 40 units wide at depth 1500 + 700 q; the last one far off to the right (culled) */
        const int x0 = q == NQUAD - 1 ? 30000 : -100 + 50 * q, y0 = -20, z = 1500 + 700 * q;
        const int xs[4] = { x0, x0 + 40, x0, x0 + 40 }, ys[4] = { y0, y0, y0 + 40, y0 + 40 };
        for (int k = 0; k < 4; k++) {
            wr16(v, (u32)xs[k]); wr16(v + 2, (u32)ys[k]); wr16(v + 4, (u32)z); wr16(v + 6, 0);
            v += 8;
        }
        wr(PKTBUF + 24 * (u32)q + 4, 0x28000000u | (0x204060u + 0x10 * (u32)q));   /* static colour */
    }
}

static int slot_of(u32 sz)
{
    if (sz < 400) return -1;
    if (sz < 1000) return (int)(sz * 341u / 1001);
    if (sz < 4000) return (int)(((sz - 1000) << 10) / (4000 - 999) + 341);
    if (sz < 16000) return (int)((sz - 4000) * 683u / (16000 - 3999) + 1365);
    return 2047;
}

/* the game's F4 builder for independent quads (800401f8), written plainly on the guest GTE */
static void game_builds(int fault)
{
    const u32 v0 = ROOT + 0x48;
    for (int q = 0; q < NQUAD; q++) {
        const u32 v = v0 + 32 * (u32)q, pk = PKTBUF + 24 * (u32)q;
        for (int i = 0; i < 6; i++) gte_write_data(i, rd(v + 4 * (u32)i));
        gte_command(0x4a280030);
        u32 xy[4] = { gte_read_data(12), gte_read_data(13), gte_read_data(14), 0 };
        gte_write_data(0, rd(v + 24)); gte_write_data(1, rd(v + 28));
        gte_command(0x4a180001);
        xy[3] = gte_read_data(14);
        for (int i = 0; i < 4; i++) wr(pk + 8 + 4 * (u32)i, xy[i]);
        int xmin = 9999, xmax = -9999, ymin = 9999, ymax = -9999;
        for (int i = 0; i < 4; i++) {
            const int x = (s16)(xy[i] & 0xffff), y = (s16)(xy[i] >> 16);
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
        if (xmax < -254 || xmin >= 255 || ymax < -118 || ymin >= 119) continue;
        u32 sz = 0;
        for (int r = 16; r < 20; r++) if ((s32)gte_read_data(r) > (s32)sz) sz = gte_read_data(r);
        const int slot = slot_of(sz);
        if (slot < 0) continue;
        if (fault == 1 && q == 1) wr(pk + 12, xy[1] + 1);              /* a wrong XY word */
        if (fault == 3 && q == 2) continue;                            /* a packet left out */
        const u32 head = OT + 4 * (u32)slot;
        wr(pk, (rd(head) & 0xffffff) | 5u << 24);
        wr(head, pk & 0xffffff);
    }
    if (fault == 2) {                                                   /* a packet nobody predicted */
        const u32 pk = PKTBUF + 0x1000, head = OT + 4 * 100;
        wr(pk + 4, 0x28ffffffu); wr(pk + 8, 0); wr(pk + 12, 0x10); wr(pk + 16, 0x100000); wr(pk + 20, 0x100010);
        wr(pk, (rd(head) & 0xffffff) | 5u << 24);
        wr(head, pk & 0xffffff);
    }
}

static void run_frame(int fault)
{
    build_stage();
    gte_camera();
    if (g_hlr.stage_pre) g_hlr.stage_pre(0);
    game_builds(fault);
    if (g_hlr.stage_post) g_hlr.stage_post();
}

static void test_stage_audit(void)
{
    hlr_set_mode(HLR_AUDIT);
    HlrAuditStats a, b;
    hlr_stage_get_stats(&a);
    run_frame(0);
    hlr_stage_get_stats(&b);
    CHECK(b.frames == a.frames + 1, "audit ran (%llu frames)", (unsigned long long)(b.frames - a.frames));
    CHECK(b.linked - a.linked == NQUAD - 1, "%llu packets linked, expected %d", (unsigned long long)(b.linked - a.linked),
          NQUAD - 1);
    CHECK(b.mismatch == a.mismatch && b.unexpl == a.unexpl && b.missing == a.missing && b.heads == a.heads &&
          b.badwalk == a.badwalk, "clean stage: mismatch %llu unexpl %llu missing %llu heads %llu",
          (unsigned long long)(b.mismatch - a.mismatch), (unsigned long long)(b.unexpl - a.unexpl),
          (unsigned long long)(b.missing - a.missing), (unsigned long long)(b.heads - a.heads));
    a = b; run_frame(1); hlr_stage_get_stats(&b);
    CHECK(b.mismatch == a.mismatch + 1, "a wrong word is a mismatch (%llu)", (unsigned long long)(b.mismatch - a.mismatch));
    a = b; run_frame(2); hlr_stage_get_stats(&b);
    CHECK(b.unexpl == a.unexpl + 1, "an extra packet is unexplained (%llu)", (unsigned long long)(b.unexpl - a.unexpl));
    a = b; run_frame(3); hlr_stage_get_stats(&b);
    CHECK(b.missing == a.missing + 1, "a left-out packet is missing (%llu)", (unsigned long long)(b.missing - a.missing));
    hlr_set_mode(HLR_OFF);
}

/* ---- capture, pairing, replay, restore -------------------------------------------------------- */
static void hires_setup(int s)
{
    gpu_init();
    hires_set_region(0, 0, 512, 480);
    hires_set_scale(0, 0);
    hires_set_scale(s, s);
    gpu_write_gp0(0xE3000000);
    gpu_write_gp0(0xE4000000 | (479u << 10) | 511u);
    gpu_write_gp0(0xE5000000 | (120u << 11) | 256u);   /* drawing offset: the screen centre */
    gpu_write_gp0(0xE6000000);
    /* a reallocated target is seeded from the previous picture: clear it */
    gpu_write_gp0(0x02000000); gpu_write_gp0(0); gpu_write_gp0(480u << 16 | 512u);
}

static u64 target_hash(int s)
{
    HiresView v;
    GpuDisplay d = { 0, 0, 512, 480, false, true };
    if (!hires_get_view(&d, &v)) return 0;
    const u16 *t = v.pixels;
    u64 h = 1469598103934665603ull;
    for (int y = 0; y < 480 * s; y++)
        for (int x = 0; x < 512 * s; x++) h = (h ^ t[y * v.stride + x]) * 1099511628211ull;
    return h;
}

/* the DMA walk of bus.c for the slots of the synthetic OT, through the filter */
static int walk(void)
{
    int drawn = 0;
    for (int s = 2047; s >= 0; s--) {
        u32 node = (OT + 4 * (u32)s) & 0x1ffffc;
        if (g_hlr.ll_filter) g_hlr.ll_filter(node);
        u32 a = rd(0x80000000u | node) & 0xffffff;
        while (a != 0xffffff) {
            const bool nh = g_hlr.ll_filter && g_hlr.ll_filter(a & 0x1ffffc);
            drawn += nh;
            if (nh) gpu_set_node_no_hires(true);
            const u32 hdr = rd(0x80000000u | a);
            for (u32 i = 0; i < hdr >> 24; i++) gpu_write_gp0_tag(rd(0x80000000u | (a + 4 + 4 * i)), 0);
            if (nh) gpu_set_node_no_hires(false);
            a = hdr & 0xffffff;
        }
    }
    if (g_hlr.ll_filter) g_hlr.ll_filter(0xffffffffu);
    return drawn;
}

static void test_capture_and_pairing(void)
{
    /* reference: the HLR off */
    hlr_set_mode(HLR_OFF);
    hires_setup(2);
    run_frame(0);
    walk();
    const u64 ref = target_hash(2);
    CHECK(ref != 0, "reference target");

    hlr_set_mode(HLR_EXACT);
    HlrStats s0, s1;
    hlr_get_stats(&s0);
    hires_setup(2);
    gte_camera();
    run_frame(0);
    const HlrFrame *f0 = hlr_frame_get(0), *f1 = hlr_frame_get(1);
    const HlrFrame *f = f0->valid ? f0 : f1;
    CHECK(f->valid && f->np == NQUAD - 1 && f->ot == (OT & 0x1ffffc), "captured frame: %d packets", f->np);
    CHECK(!(f0->valid && f1->valid), "one frame");
    if (f->valid && f->np > 0) {
        const HlrPkt *p = hlr_frame_find(f, PKTBUF);
        CHECK(p && p->n == 5 && !memcmp(f->w + p->off, g_ram + (PKTBUF & 0x1ffffc) + 4, 20), "a copy of the packet");
        CHECK(f->w < (u32 *)g_ram || f->w >= (u32 *)(g_ram + RAM_SIZE), "the frame never points into guest RAM");
    }
    const int drawn = walk();
    hlr_get_stats(&s1);
    CHECK(drawn == NQUAD - 1, "%d packets drawn by the HLR", drawn);
    CHECK(s1.walked == s0.walked + 1 && !hlr_frame_get(0)->valid && !hlr_frame_get(1)->valid, "the walk consumed it");
    CHECK(target_hash(2) == ref, "replayed frame == plain frame");

    /* a second walk of the same table without a new capture: nothing suppressed */
    CHECK(walk() == 0, "no frame, no suppression");

    /* stale: the packet changed in RAM after the capture */
    hires_setup(2);
    run_frame(0);
    wr(PKTBUF + 4, rd(PKTBUF + 4) ^ 0x10);
    hlr_get_stats(&s0);
    const int d2 = walk();
    hlr_get_stats(&s1);
    CHECK(d2 == NQUAD - 2 && s1.stale_pkts == s0.stale_pkts + 1, "stale packet sent normally (%d drawn)", d2);

    /* save-state load and mode change drop captures */
    run_frame(0);
    CHECK(hlr_frame_get(0)->valid || hlr_frame_get(1)->valid, "captured");
    hlr_get_stats(&s0);
    g_hlr.on_restore();
    hlr_get_stats(&s1);
    CHECK(!hlr_frame_get(0)->valid && !hlr_frame_get(1)->valid && s1.restores == s0.restores + 1, "restore drops");
    CHECK(walk() == 0, "nothing drawn after a restore");
    run_frame(0);
    hlr_set_mode(HLR_AUDIT);
    CHECK(!hlr_frame_get(0)->valid && !hlr_frame_get(1)->valid, "mode change drops");

    /* two tables: built twice before a walk = replaced; another table goes to the other slot */
    hlr_set_mode(HLR_EXACT);
    hlr_get_stats(&s0);
    run_frame(0);
    run_frame(0);
    hlr_get_stats(&s1);
    CHECK(s1.replaced == s0.replaced + 1, "rebuilt table replaces its frame");
    HlrFrame *g = hlr_frame_begin(0x120000);
    const u32 w5[5] = { 1, 2, 3, 4, 5 };
    hlr_frame_add(g, 0x130000, 1, w5, 5);
    hlr_frame_commit(g);
    CHECK(hlr_frame_get(0)->valid && hlr_frame_get(1)->valid, "two tables, two frames");
    CHECK(hlr_frame_find(g, 0x130000) && !hlr_frame_find(g, 0x130004), "find");
    hlr_frames_drop();
    hlr_set_mode(HLR_OFF);
    hires_set_scale(0, 0);
}

/* ---- suppression in gpu.c --------------------------------------------------------------------- */
static void send(const u32 *w, int n) { for (int i = 0; i < n; i++) gpu_write_gp0_tag(w[i], 0); }

static void test_suppression(void)
{
    /* a gouraud triangle, then a flat quad that overlaps it (painter's order matters) */
    const u32 tri[6] = { 0x30ff0000u, (u32)(-20 & 0xffff) | (u32)(-30 & 0xffff) << 16, 0x0000ff00u,
                         60u | 10u << 16, 0x000000ffu, (u32)(-50 & 0xffff) | 70u << 16 };
    const u32 quad[5] = { 0x28808080u, 0u, 80u, 80u << 16, 80u | 80u << 16 };
    hires_setup(2);
    send(tri, 6); send(quad, 5);
    const u64 a = target_hash(2);

    hires_setup(2);
    gpu_hires_replay(tri, 6, NULL);
    gpu_set_node_no_hires(true); send(tri, 6); gpu_set_node_no_hires(false);
    send(quad, 5);
    const u64 b = target_hash(2);
    CHECK(a == b, "replay + 1x-only == plain");
    hires_setup(2);
    gpu_set_node_no_hires(true); send(tri, 6); gpu_set_node_no_hires(false);
    send(quad, 5);
    CHECK(target_hash(2) != a, "1x-only alone leaves the triangle out of the hi-res target");

    hires_setup(2);
    send(quad, 5);
    const u64 q = target_hash(2);
    hires_setup(2);
    gpu_set_node_no_hires(true); send(tri, 6); gpu_set_node_no_hires(false);
    send(quad, 5);
    CHECK(target_hash(2) == q, "suppressed triangle draws nothing in the hi-res target");
    hires_set_scale(0, 0);
}

/* ---- float camera vs the GTE's RTPS --------------------------------------------------------- */
static u32 s_rng = 12345;
static int rnd(int lo, int hi) { s_rng = s_rng * 1103515245u + 12345u; return lo + (int)((s_rng >> 8) % (u32)(hi - lo + 1)); }

static void test_float_camera(void)
{
    int checked = 0, skipped = 0, offscreen = 0, far = 0;
    float worst = 0.0f, worst_clip = 0.0f;
    for (int cam = 0; cam < 200; cam++) {
        gte_init();
        const double a = rnd(0, 6283) / 1000.0, b = rnd(-600, 600) / 1000.0;   /* yaw, pitch */
        double m[3][3] = { { cos(a), 0, -sin(a) }, { sin(a) * sin(b), cos(b), cos(a) * sin(b) },
                           { sin(a) * cos(b), -sin(b), cos(a) * cos(b) } };
        for (int c = 0; c < 3; c++) m[1][c] *= 0.625;                         /* the game's Y scale */
        s16 e[9];
        for (int i = 0; i < 9; i++) e[i] = (s16)lrint(m[i / 3][i % 3] * 4096.0);
        gte_write_ctrl(0, (u16)e[0] | (u32)(u16)e[1] << 16);
        gte_write_ctrl(1, (u16)e[2] | (u32)(u16)e[3] << 16);
        gte_write_ctrl(2, (u16)e[4] | (u32)(u16)e[5] << 16);
        gte_write_ctrl(3, (u16)e[6] | (u32)(u16)e[7] << 16);
        gte_write_ctrl(4, (u16)e[8]);
        gte_write_ctrl(5, (u32)rnd(-2000, 2000));
        gte_write_ctrl(6, (u32)rnd(-1000, 1000));
        gte_write_ctrl(7, (u32)rnd(1500, 8000));
        gte_write_ctrl(24, (u32)(rnd(-8, 8) << 16));
        gte_write_ctrl(25, (u32)(rnd(-8, 8) << 16));
        gte_write_ctrl(26, (u32)rnd(200, 700));
        GteState gs;
        gte_save(&gs);
        HlrCam k;
        hlr_cam_from_gte(&k, &gs);
        /* a 4x6 widescreen pass: guard band 88 columns, region at x 0, drawing offset (256, 120) */
        const float Sx = 4, Sy = 6, ox = -88 * 4, oy = 0, offx = 256, offy = 120;
        const int rect[4] = { 0, 0, (512 + 176) * 4, 240 * 6 };
        float p[4];
        hlr_cam_clip(&k, offx, offy, Sx, Sy, ox, oy, rect, p);
        for (int n = 0; n < 50; n++) {
            const s16 v[3] = { (s16)rnd(-3000, 3000), (s16)rnd(-1500, 1500), (s16)rnd(-3000, 3000) };
            gte_write_data(0, (u16)v[0] | (u32)(u16)v[1] << 16);
            gte_write_data(1, (u32)(u16)v[2]);
            gte_command(0x00080001);                                          /* RTPS, sf = 1 */
            const u32 flag = gte_read_ctrl(31), sxy = gte_read_data(14);
            if (flag & 0x80000000u) { skipped++; continue; }                  /* saturated, too near, overflow */
            /* RTPS truncates (SX = floor): the GTE's pixel is [SX, SX + 1), compare with its centre */
            const float gx = (float)(s16)sxy + 0.5f, gy = (float)(s16)(sxy >> 16) + 0.5f;
            const float fv[3] = { v[0], v[1], v[2] };
            float view[3], sx, sy;
            hlr_cam_view(&k, fv, view);
            if (!hlr_cam_project(&k, view, &sx, &sy)) { skipped++; continue; }
            /* on screen only (16:9: 344 columns either side of the centre, 120 rows): further out
             * the GTE's integer SZ3 / IR steps alone move SX by more (up to 3 px at |SX| ~ 1000) */
            if (fabsf(gx) > 344.0f || fabsf(gy) > 121.0f) { offscreen++; continue; }
            /* the GTE truncates the view position (IR1/IR2) to integers before the division: that
             * alone moves SX/SY by up to H / z px. Within 0.75 px where it is at most 1/4 (z >= 4H,
             * the stage's usual depths), else within 0.5 + H / z + 0.1. */
            const float tol = view[2] >= 4.0f * k.h ? 0.75f : 0.6f + k.h / view[2];
            const float d = fmaxf(fabsf(sx - gx), fabsf(sy - gy));
            if (view[2] >= 4.0f * k.h && d > worst) worst = d;
            CHECK(d <= tol, "camera %d vertex %d: float (%.3f, %.3f), GTE (%g, %g), z %.0f", cam, n, sx, sy, gx, gy, view[2]);
            /* the clip-space form the shaders use, back to 1x screen units */
            const float nx = p[0] * view[0] / view[2] + p[1], ny = p[2] * view[1] / view[2] + p[3];
            const float tx = (float)rect[0] + (nx + 1.0f) * 0.5f * (float)rect[2];
            const float ty = (float)rect[1] + (1.0f - ny) * 0.5f * (float)rect[3];
            const float cx = (tx + ox) / Sx - offx, cy = (ty + oy) / Sy - offy;
            const float dc = fmaxf(fabsf(cx - sx), fabsf(cy - sy));   /* vs the float projection */
            if (dc > worst_clip) worst_clip = dc;
            CHECK(dc <= 0.01f, "camera %d vertex %d: clip space (%.3f, %.3f), float (%.3f, %.3f)", cam, n, cx, cy, sx, sy);
            if (view[2] >= 4.0f * k.h) far++;
            checked++;
        }
    }
    CHECK(checked > 2000, "only %d vertices projected", checked);
    CHECK(far > 1000, "only %d vertices at z >= 4H", far);
    printf("  %d vertices on screen (%d at z >= 4H: worst %.3f px), %d off screen, %d skipped (GTE saturation / behind);"
           " clip space vs float %.4f px\n", checked, far, worst, offscreen, skipped, worst_clip);
}


/* ---- characters (phase 3): a synthetic fighter model through hlr_char.c's audit --------------- */
#define CMB       0x80110000u                 /* model block */
#define CPARTS    0x80110100u
#define CCOORD    0x80110400u                 /* coordinate structs, 0x100 apart */
#define CVERT     0x80111000u
#define CNORM     0x80111400u
#define CCHAIN    0x80111800u
#define CGRP      0x80112000u
#define CHTRI     0x80112100u
#define CHQUAD    0x80113000u
#define CVB       0x800c9d38u
#define CCB       0x800cb7c8u
#define CNV       8                           /* vertices (and normals) per part */
#define CNTRI     8
#define CNQUAD    5

static int s_cv[CNTRI + CNQUAD][4];

static void build_char(int rot)
{
    memset(g_ram, 0, RAM_SIZE);
    memset(g_scratch, 0, sizeof g_scratch);
    *(u32 *)(g_scratch + 0x0c) = 1000;
    *(u32 *)(g_scratch + 0x10) = 4000;
    *(u32 *)(g_scratch + 0x08) = 16000;
    for (u32 i = 0; i < 2048; i++) wr(OT + 4 * i, 0x00ffffffu);
    /* world-screen matrix: identity rotation, the model 1500 units in front */
    const u32 ws[8] = { 0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 1500 };
    for (int i = 0; i < 8; i++) wr(0x800ed4c8u + 4 * (u32)i, ws[i]);
    wr(0x800bfe28u, 0x3c808080u);                 /* RGBC (GT4 code) */
    wr(CMB + 8, 2); wr(CMB + 12, CPARTS); wr(CMB + 16, CGRP); wr(CMB + 20, CCHAIN);
    const double ang[2] = { 0.3 + 0.01 * rot, 0.9 - 0.02 * rot };
    const int tr[2][3] = { { 0, 0, 0 }, { 50, -30, 20 + rot } };
    for (int p = 0; p < 2; p++) {
        const u32 part = CPARTS + 0x70u * (u32)p, coord = CCOORD + 0x100u * (u32)p;
        wr(part, coord);
        /* workm: rotation about Y */
        const int c = (int)lround(4096 * cos(ang[p])), sn = (int)lround(4096 * sin(ang[p]));
        const int m[9] = { c, 0, sn, 0, 4096, 0, -sn, 0, c };
        for (int k = 0; k < 9; k++) wr16(coord + 36 + 2 * (u32)k, (u32)m[k]);
        for (int k = 0; k < 3; k++) wr(coord + 56 + 4 * (u32)k, (u32)tr[p][k]);
        /* light matrix (one light, from the front) and light colours */
        /* LLM row 1 = (0, 0, -4096): L13 is the low half of word 1 */
        wr(part + 36, 0); wr(part + 40, 0xf000u); wr(part + 44, 0); wr(part + 48, 0); wr(part + 52, 0);
        const u32 lcm[5] = { 0x1000, 0, 0x1000, 0, 0x1000 };
        for (int k = 0; k < 5; k++) wr(part + 68 + 4 * (u32)k, lcm[k]);
        const u32 vp = CVERT + 0x100u * (u32)p, np = CNORM + 0x100u * (u32)p;
        wr(part + 0x64, vp); wr(part + 0x68, np);
        wr16(part + 0x6c, CNV); wr16(part + 0x6e, CNV);
        for (int k = 0; k < CNV; k++) {
            const int x = k & 1 ? 60 : -60, y = k & 2 ? 70 : -70, z = k & 4 ? 40 : -40;
            wr16(vp + 8 * (u32)k, (u32)x); wr16(vp + 8 * (u32)k + 2, (u32)y); wr16(vp + 8 * (u32)k + 4, (u32)z);
            const double l = sqrt((double)(x * x + y * y + z * z));
            wr16(np + 8 * (u32)k, (u32)(int)(4096 * x / l)); wr16(np + 8 * (u32)k + 2, (u32)(int)(4096 * y / l));
            wr16(np + 8 * (u32)k + 4, (u32)(int)(4096 * z / l));
        }
    }
    /* one chain on part 1: anchored at its vertex 3, two world points */
    wr(CCHAIN, 1); wr(CCHAIN + 4, 1);
    wr(CCHAIN + 8, 3u << 16 | 2);
    const int pts[2][3] = { { 90, 40, 10 }, { 110, 80, 30 } };
    for (int k = 0; k < 2; k++) {
        const u32 a = CCHAIN + 12 + 8 * (u32)k;
        wr16(a, (u32)pts[k][0]); wr16(a + 2, (u32)pts[k][1]); wr16(a + 4, (u32)pts[k][2]); wr16(a + 6, 40);
    }
    /* faces: one group, a GT3 list and a GT4 list */
    wr(CGRP, 1); wr(CGRP + 4, 2); wr(CGRP + 8, CHTRI); wr(CGRP + 12, CHQUAD);
    wr(CHTRI, 0x1au << 16 | CNTRI); wr(CHQUAD, 0x1bu << 16 | CNQUAD);
    s_rng = 777;
    for (int f = 0; f < CNTRI + CNQUAD; f++) {
        const bool quad = f >= CNTRI;
        for (int j = 0; j < 4; j++) s_cv[f][j] = rnd(0, 2 * CNV + 1);   /* chain vertices 16, 17 too */
        if (quad) {
            const u32 rec = CHQUAD + 4 + 0x78u * (u32)(f - CNTRI);
            wr(rec + 0x68, (u32)s_cv[f][0] | (u32)s_cv[f][1] << 16); wr(rec + 0x6c, (u32)s_cv[f][2] | (u32)s_cv[f][3] << 16);
            const int c0 = s_cv[f][0] % (2 * CNV), c1 = s_cv[f][1] % (2 * CNV), c2 = s_cv[f][2] % (2 * CNV), c3 = s_cv[f][3] % (2 * CNV);
            wr(rec + 0x70, (u32)c0 | (u32)c1 << 16); wr(rec + 0x74, (u32)c2 | (u32)c3 << 16);
            for (int j = 0; j < 4; j++) wr(rec + 12 + 12 * (u32)j, 0x00401020u + (u32)j);    /* uv / clut / tpage */
        } else {
            const u32 rec = CHTRI + 4 + 0x5cu * (u32)f;
            /* packet order A, C, B: w50 = A | C, w54 = B | colour A, w58 = colours C | B */
            const int c0 = s_cv[f][0] % (2 * CNV), c1 = s_cv[f][1] % (2 * CNV), c2 = s_cv[f][2] % (2 * CNV);
            wr(rec + 0x50, (u32)s_cv[f][0] | (u32)s_cv[f][1] << 16);
            wr(rec + 0x54, (u32)s_cv[f][2] | (u32)c0 << 16);
            wr(rec + 0x58, (u32)c1 | (u32)c2 << 16);
            for (int j = 0; j < 3; j++) wr(rec + 12 + 12 * (u32)j, 0x00401020u + (u32)j);
        }
    }
}

/* the game's char_mesh_render (80036b0c, chains 800367d8, face builders 80036138 / 800363c8),
 * written plainly on the guest GTE */
static void game_char(int fault)
{
    u32 ws[8];
    for (int i = 0; i < 8; i++) ws[i] = rd(0x800ed4c8u + 4 * (u32)i);
    int nv = 0, nc = 0;
    for (int p = 0; p < 2; p++) {
        const u32 part = CPARTS + 0x70u * (u32)p, coord = rd(part);
        s16 sm[3][3];
        for (int i = 0; i < 5; i++) gte_write_ctrl(i, ws[i]);
        for (int col = 0; col < 3; col++) {
            for (int r = 0; r < 3; r++) gte_write_data(9 + r, *(u16 *)(g_ram + ((coord + 36 + 6 * (u32)r + 2 * (u32)col) & 0x1fffff)));
            gte_command(0x4a49e012);
            for (int r = 0; r < 3; r++) sm[r][col] = (s16)gte_read_data(9 + r);
        }
        for (int i = 5; i < 8; i++) gte_write_ctrl(i, ws[i]);
        gte_write_data(0, (rd(coord + 56) & 0xffff) | rd(coord + 60) << 16);
        gte_write_data(1, rd(coord + 64));
        gte_command(0x4a480012);
        const u32 t[3] = { gte_read_data(25), gte_read_data(26), gte_read_data(27) };
        gte_write_ctrl(0, (u16)sm[0][0] | (u32)(u16)sm[0][1] << 16); gte_write_ctrl(1, (u16)sm[0][2] | (u32)(u16)sm[1][0] << 16);
        gte_write_ctrl(2, (u16)sm[1][1] | (u32)(u16)sm[1][2] << 16); gte_write_ctrl(3, (u16)sm[2][0] | (u32)(u16)sm[2][1] << 16);
        gte_write_ctrl(4, (u16)sm[2][2]);
        for (int k = 0; k < 3; k++) gte_write_ctrl(5 + k, t[k]);
        const u32 vp = rd(part + 0x64), np = rd(part + 0x68);
        for (int k = 0; k < CNV; k++) {           /* RTPS each (the game uses RTPT in threes: same results) */
            gte_write_data(0, rd(vp + 8 * (u32)k)); gte_write_data(1, rd(vp + 8 * (u32)k + 4));
            gte_command(0x4a180001);
            wr(CVB + 8 * (u32)(nv + k), gte_read_data(14)); wr(CVB + 8 * (u32)(nv + k) + 4, gte_read_data(19));
        }
        for (int k = 0; k < 5; k++) { gte_write_ctrl(8 + k, rd(part + 36 + 4 * (u32)k)); gte_write_ctrl(16 + k, rd(part + 68 + 4 * (u32)k)); }
        gte_write_data(6, rd(0x800bfe28u));
        for (int k = 0; k < CNV; k++) {
            gte_write_data(0, rd(np + 8 * (u32)k)); gte_write_data(1, rd(np + 8 * (u32)k + 4));
            gte_command(0x4b08041b);
            wr(CCB + 4 * (u32)(nc + k), gte_read_data(22));
        }
        nv += CNV; nc += CNV;
    }
    /* chain points: world coordinates projected with WS alone */
    for (int i = 0; i < 8; i++) gte_write_ctrl(i, ws[i]);
    for (int k = 0; k < 2; k++) {
        const u32 a = CCHAIN + 12 + 8 * (u32)k;
        gte_write_data(0, rd(a)); gte_write_data(1, rd(a + 4));
        gte_command(0x4a180001);
        wr(CVB + 8 * (u32)nv, gte_read_data(14)); wr(CVB + 8 * (u32)nv + 4, gte_read_data(19));
        nv++;
    }
    if (fault == 1) wr(CCB + 4 * 5, rd(CCB + 4 * 5) ^ 0x10);          /* a colour the lights do not give */
    if (fault == 3) wr(CVB + 8 * 4, rd(CVB + 8 * 4) + 1);             /* a vertex one pixel off */
    int linked = 0;
    for (int f = 0; f < CNTRI + CNQUAD; f++) {
        const bool quad = f >= CNTRI;
        const int n = quad ? 4 : 3;
        u32 xy[4], z[4], col[4];
        for (int j = 0; j < n; j++) {
            xy[j] = rd(CVB + 8 * (u32)s_cv[f][j]); z[j] = rd(CVB + 8 * (u32)s_cv[f][j] + 4);
            col[j] = rd(CCB + 4 * (u32)(s_cv[f][j] % (2 * CNV)));
        }
        gte_write_data(12, xy[0]); gte_write_data(13, xy[1]); gte_write_data(14, xy[2]);
        gte_command(0x4b400006);                                        /* NCLIP */
        s32 a = (s32)gte_read_data(24);
        bool kept = a > 0;
        u32 sz;
        if (quad) {
            gte_write_data(12, xy[3]); gte_write_data(13, xy[1]); gte_write_data(14, xy[2]);
            gte_command(0x4b400006);
            kept = kept || (s32)gte_read_data(24) < 0;
            sz = z[0];
            for (int j = 1; j < 4; j++) if ((s32)z[j] < (s32)sz) sz = z[j];
        } else sz = (z[0] + z[1] + z[2]) / 3;
        if (!kept) continue;
        if (fault == 2 && linked++ == 1) continue;                      /* a face the rule keeps, not linked */
        const u32 pk = quad ? CHQUAD + 4 + 0x78u * (u32)(f - CNTRI) : CHTRI + 4 + 0x5cu * (u32)f;
        for (int j = 0; j < n; j++) {
            wr(pk + 4 + 12 * (u32)j, j ? col[j] : quad ? col[0] : (col[0] & 0xffffff) | 0x34000000u);
            wr(pk + 8 + 12 * (u32)j, xy[j]);
        }
        const int slot = slot_of(sz) < 0 ? (int)(sz * 0x155u / 1001) : slot_of(sz);
        const u32 head = OT + 4 * (u32)slot;
        wr(pk, (rd(head) & 0xffffff) | (quad ? 12u : 9u) << 24);
        wr(head, pk & 0xffffff);
    }
}

static void char_frame(int fault, int rot)
{
    build_char(rot);
    gte_camera();
    if (g_hlr.char_pre) g_hlr.char_pre(CMB, OT);
    game_char(fault);
    if (g_hlr.char_post) g_hlr.char_post();
}

static void test_char_audit(void)
{
    hlr_set_mode(HLR_AUDIT);
    HlrCharStats a, b;
    hlr_char_get_stats(&a);
    char_frame(0, 0);
    hlr_char_get_stats(&b);
    CHECK(b.audited == a.audited + 1, "model audited (%llu)", (unsigned long long)(b.audited - a.audited));
    CHECK(b.vtx - a.vtx == 2 * CNV + 2 && b.col - a.col == 2 * CNV, "%llu vertices, %llu colours",
          (unsigned long long)(b.vtx - a.vtx), (unsigned long long)(b.col - a.col));
    CHECK(b.faces - a.faces == CNTRI + CNQUAD, "%llu faces", (unsigned long long)(b.faces - a.faces));
    CHECK(b.kept > a.kept && b.kept - a.kept < CNTRI + CNQUAD, "%llu of %d faces kept (some must face away)",
          (unsigned long long)(b.kept - a.kept), CNTRI + CNQUAD);
    CHECK(b.vtx_bad == a.vtx_bad && b.col_bad == a.col_bad && b.link_bad == a.link_bad && b.word_bad == a.word_bad &&
          b.bad_model == a.bad_model && b.frames_bad == a.frames_bad,
          "clean model: vtx %llu col %llu link %llu words %llu bad %llu rejected %llu",
          (unsigned long long)(b.vtx_bad - a.vtx_bad), (unsigned long long)(b.col_bad - a.col_bad),
          (unsigned long long)(b.link_bad - a.link_bad), (unsigned long long)(b.word_bad - a.word_bad),
          (unsigned long long)(b.bad_model - a.bad_model), (unsigned long long)(b.frames_bad - a.frames_bad));
    CHECK(b.proj_n > a.proj_n && b.proj_err_max < 1.5, "float projection of the bones within 1.5 px (max %.3f)", b.proj_err_max);
    a = b; char_frame(1, 0); hlr_char_get_stats(&b);
    CHECK(b.col_bad > a.col_bad && b.frames_bad == a.frames_bad + 1, "a wrong colour is found (%llu) and rejects the model",
          (unsigned long long)(b.col_bad - a.col_bad));
    a = b; char_frame(2, 0); hlr_char_get_stats(&b);
    CHECK(b.link_bad == a.link_bad + 1 && b.frames_bad == a.frames_bad + 1, "an unlinked kept face is found (%llu)",
          (unsigned long long)(b.link_bad - a.link_bad));
    a = b; char_frame(3, 0); hlr_char_get_stats(&b);
    CHECK(b.vtx_bad == a.vtx_bad + 1 && b.frames_bad == a.frames_bad + 1, "a wrong vertex is found (%llu)",
          (unsigned long long)(b.vtx_bad - a.vtx_bad));
    for (int r = 1; r < 6; r++) {
        a = b; char_frame(0, r); hlr_char_get_stats(&b);
        CHECK(b.frames_bad == a.frames_bad && b.audited == a.audited + 1, "pose %d clean", r);
    }
    hlr_set_mode(HLR_OFF);
}

/* ---- shadows (phase 4): a synthetic fighter_shadow_render on the guest GTE ------------------- */
#define SH_FIGHTER 0x80150000u
#define SH_BUF     0x80160000u
#define SH_WORLD_A 0x800e70f0u
#define SH_SCR_A   0x800e6f80u

static u32 sh_slot(u32 z)
{
    const u32 near = 1000, mid = 4000, far = 16000;
    if (z < near) return z * 0x155u / (near + 1);
    if (z < mid) return ((z - near) * 0x400u) / (mid - (near - 1)) + 0x155;
    if (z < far) return ((z - mid) * 0x2abu) / (far - (mid - 1)) + 0x555;
    return 0x7ff;
}

/* fault: 1 a wrong screen vertex, 2 a wrong grey, 3 a triangle not linked, 4 another matrix in RAM */
static void shadow_frame(int fault)
{
    memset(g_ram, 0, RAM_SIZE);
    memset(g_scratch, 0, sizeof g_scratch);
    *(u32 *)(g_scratch + 0x0c) = 1000;
    *(u32 *)(g_scratch + 0x10) = 4000;
    *(u32 *)(g_scratch + 0x08) = 16000;
    const u32 rt[8] = { 0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 0 };
    for (int i = 0; i < 8; i++) wr(0x800ed4c8u + 4 * (u32)i, rt[i]);
    if (fault == 4) wr(0x800ed4c8u, 0x1001);
    for (u32 i = 0; i < 2048; i++) wr(OT + 4 * i, 0x00ffffffu);
    gte_camera();
    const int y0 = 120;
    wr(SH_FIGHTER + 0x3c, (u32)(y0 - 300));       /* 30 above the ground: grey 0x80 - 30 */
    if (g_hlr.shadow_pre) g_hlr.shadow_pre(SH_FIGHTER, OT, SH_BUF);
    /* the game: 45 vertices on a sloped floor, RTPS each */
    u32 sxy[45], sz[45];
    for (int i = 0; i < 45; i++) {
        const int x = i == 0 ? 0 : -200 + 10 * (i % 41), z = 1800 + (i == 0 ? 0 : 20 * (i % 13)), y = y0 + (i == 0 ? 0 : z / 40 - 45);
        wr16(SH_WORLD_A + 8 * (u32)i, (u32)x); wr16(SH_WORLD_A + 8 * (u32)i + 2, (u32)y); wr16(SH_WORLD_A + 8 * (u32)i + 4, (u32)z);
        gte_write_data(0, (u16)x | (u32)(u16)y << 16); gte_write_data(1, (u32)(u16)z);
        gte_command(0x4a180001);
        sxy[i] = gte_read_data(14); sz[i] = gte_read_data(19);
        wr(SH_SCR_A + 8 * (u32)i, sxy[i] + (fault == 1 && i == 5 ? 1 : 0)); wr(SH_SCR_A + 8 * (u32)i + 4, sz[i]);
    }
    const u32 grey = 0x80 - 30;
    for (int t = 0; t < 64; t++) {
        const u32 rec = SH_BUF + 0x58 * (u32)t, pkt = rec;
        const int vi[3] = { 0, 1 + t % 44, 1 + (t + 1) % 44 };
        for (int j = 0; j < 3; j++) wr16(rec + 0x50 + 2 * (u32)j, (u32)vi[j]);
        const u32 g = (fault == 2 && t == 7 ? grey + 1 : grey) * 0x010101u;
        wr(pkt + 4, 0x36000000u | g);  wr(pkt + 8, sxy[vi[0]]);  wr(pkt + 12, 0x7d110000u | 0x0404);
        wr(pkt + 16, g);               wr(pkt + 20, sxy[vi[1]]); wr(pkt + 24, 0x00590000u | 0x0434);
        wr(pkt + 28, g);               wr(pkt + 32, sxy[vi[2]]); wr(pkt + 36, 0x3404);
        if (fault == 3 && t == 20) { wr(pkt, 0x09ffffffu); continue; }
        const u32 slot = sh_slot(sz[vi[0]]);
        wr(pkt, (rd(OT + 4 * slot) & 0xffffff) | 0x09000000u);
        wr(OT + 4 * slot, pkt & 0xffffff);
    }
    if (g_hlr.shadow_post) g_hlr.shadow_post();
}

static void test_shadow_audit(void)
{
    hlr_set_mode(HLR_AUDIT);
    HlrShadowStats a, b;
    hlr_shadow_get_stats(&a);
    shadow_frame(0);
    hlr_shadow_get_stats(&b);
    CHECK(b.audited == a.audited + 1 && b.frames_bad == a.frames_bad, "clean shadow audited (%llu, rejected %llu)",
          (unsigned long long)(b.audited - a.audited), (unsigned long long)(b.frames_bad - a.frames_bad));
    CHECK(b.vtx - a.vtx == 45 && b.faces - a.faces == 64, "%llu vertices, %llu triangles",
          (unsigned long long)(b.vtx - a.vtx), (unsigned long long)(b.faces - a.faces));
    a = b; shadow_frame(1); hlr_shadow_get_stats(&b);
    CHECK(b.vtx_bad == a.vtx_bad + 1 && b.frames_bad == a.frames_bad + 1, "a wrong screen vertex is found");
    a = b; shadow_frame(2); hlr_shadow_get_stats(&b);
    CHECK(b.grey_bad > a.grey_bad && b.frames_bad == a.frames_bad + 1, "a wrong grey is found");
    a = b; shadow_frame(3); hlr_shadow_get_stats(&b);
    CHECK(b.link_bad == a.link_bad + 1 && b.frames_bad == a.frames_bad + 1, "an unlinked triangle is found");
    a = b; shadow_frame(4); hlr_shadow_get_stats(&b);
    CHECK(b.ws_bad == a.ws_bad + 1 && b.frames_bad == a.frames_bad + 1, "another world-screen matrix is found");
    a = b; shadow_frame(0); hlr_shadow_get_stats(&b);
    CHECK(b.audited == a.audited + 1 && b.frames_bad == a.frames_bad, "clean again");
    hlr_set_mode(HLR_OFF);
}

/* the bone filter: smooths jitter, gives way to real motion at once, starts from the input */
static void test_bone_filter(void)
{
    HlrBoneFilter bf;
    memset(&bf, 0, sizeof bf);
    float x[12], o[12];
    for (int i = 0; i < 12; i++) x[i] = i < 9 ? (i % 4 == 0 ? 1.0f : 0.0f) : 100.0f;
    hlr_bone_filter(&bf, x, o);
    CHECK(!memcmp(x, o, sizeof o), "first update passes through");
    /* jitter: +-0.006 on the matrix, +-1.5 units on the translation, alternating */
    double ein = 0, eout = 0;
    for (int u = 0; u < 200; u++) {
        const float s = (u & 1) ? 1.0f : -1.0f, j = (float)((rnd(0, 1000) - 500) / 500.0) * 0.5f + 0.5f;
        float y[12];
        for (int i = 0; i < 12; i++) y[i] = x[i] + (i < 9 ? 0.006f : 1.5f) * s * j;
        hlr_bone_filter(&bf, y, o);
        if (u >= 20) for (int i = 0; i < 12; i++) { ein += fabs(y[i] - x[i]); eout += fabs(o[i] - x[i]); }
    }
    printf("  jitter: output error %.3f vs input %.3f\n", eout, ein);
    CHECK(eout < 0.6 * ein, "jitter reduced: output error %.3f vs input %.3f", eout, ein);
    /* a real move: a 0.5 rotation change and 80 units of translation are followed in one update */
    float y[12];
    memcpy(y, x, sizeof y);
    y[0] = 0.5f; y[9] = 180.0f;
    hlr_bone_filter(&bf, y, o);
    CHECK(fabsf(o[0] - 0.5f) < 1e-4f && fabsf(o[9] - 180.0f) < 1e-3f, "a large change snaps (%.4f, %.3f)", o[0], o[9]);
    /* a steady slow motion is tracked without lag once the prediction is in */
    float lag = 0;
    for (int u = 0; u < 60; u++) {
        y[10] = 100.0f + 2.0f * (float)u;
        hlr_bone_filter(&bf, y, o);
        if (u >= 40) lag = fmaxf(lag, fabsf(o[10] - y[10]));
    }
    printf("  slow motion lag %.3f units\n", lag);
    CHECK(lag < 1.0f, "slow motion followed (lag %.3f units)", lag);
    /* a freeze (pause, hit-stop): the hold puts the output on the input at once and drops the trend,
     * so the next updates with the same input stay there instead of coasting on */
    hlr_bone_filter_hold(&bf, y, o);
    CHECK(!memcmp(o, y, sizeof o), "hold: output = input");
    float drift = 0;
    for (int u = 0; u < 10; u++) {
        hlr_bone_filter(&bf, y, o);
        for (int i = 0; i < 12; i++) drift = fmaxf(drift, fabsf(o[i] - y[i]));
    }
    CHECK(drift == 0.0f, "nothing moves after a hold (drift %.4f)", drift);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    s_verbose = argc > 1 && !strcmp(argv[1], "-v");
    g_pgxp = false;
    printf("switches\n"); test_switches();
    printf("stage audit\n"); test_stage_audit();
    printf("capture and pairing\n"); test_capture_and_pairing();
    printf("suppression\n"); test_suppression();
    printf("float camera\n"); test_float_camera();
    printf("characters\n"); test_char_audit();
    printf("shadows\n"); test_shadow_audit();
    printf("bone filter\n"); test_bone_filter();
    printf("test_hlr: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
