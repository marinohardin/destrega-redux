/* HLR effects: capture and audit of the effect builders (docs/HLR_PLAN.md phase 5; `--hlr audit|exact|on`).
 *
 * Read-only model of the ten effect builders 80055820 .. 80056b40 (analysis/NOTES.md "Effects",
 * "Effect builders in full"). Every builder takes a0 = the effect object, a1 = the scene ordering
 * table, and works the same way:
 *   - optional facing test: MVMVA (RT * V0, no translation) on the SVECTOR at obj + T; nothing is
 *     drawn unless MAC3 > 0;
 *   - the packet is this frame's half of the object's two: obj + idx * S (idx = scratch 1f800000,
 *     S = 4 * (len + 1));
 *   - RTPT on the three SVECTORs at obj + 2S, the XY words straight into the packet; quads: RTPS on
 *     obj + 2S + 0x18 for the fourth;
 *   - OT slot = the 3-segment map (1f80000c / 1f800010 / 1f800008) of the smallest SZ, no x / y /
 *     near culling; the packet is linked at that slot (tag = len << 24);
 *   - the untextured builders link a one-word draw-mode prim (obj + idx * 8 + M, the caller's e1
 *     word) after the packet in the same slot, so the walk meets it first: its semi mode is the
 *     packet's.
 * The vertex colours, UV / CLUT / page words and the command byte are written by the caller.
 *
 * The wrappers in overrides/effects.c call hlr_fx_pre with the builder's address and a0 / a1 and
 * hlr_fx_post after the original. Audit (every mode but off; on a copy of the GTE taken at entry):
 * the facing test, every XY word, the command byte's polygon type, the tag, the slot and the
 * draw-mode prim. A clean call becomes one effect polygon of the stage's frame (HlrChr with fx set)
 * when effects are drawn natively: world positions from the GTE's own view-space result (RT * v +
 * TR, in float) through the inverse of the frame's camera, so the native camera puts them where
 * the game did, at any aspect ratio; hlr_native.c draws them depth tested, never writing depth, in
 * the PS1's order among the effects. */
#include "psx.h"
#include "hlr.h"
#include "pgxp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_IDX         0x1f800000u
#define SCR_FAR         0x1f800008u
#define SCR_NEAR        0x1f80000cu
#define SCR_MID         0x1f800010u
#define OP_RTPS         0x4a180001
#define OP_RTPT         0x4a280030
#define OP_FACING       0x4a486012      /* MVMVA sf=1, RT * V0, no translation */

enum { K_FT3, K_FT4, K_GT4, K_G3, K_G4, K_F3 };
typedef struct {
    u32 addr;
    u8 kind, nv, len;
    u8 code;            /* polygon type: command byte & 0xfc */
    u16 test;           /* offset of the facing test's SVECTOR, 0 = none */
    u16 mode;           /* offset of the draw-mode prims, 0 = none */
    u8 xy[4];           /* packet word index of each vertex's XY */
} Builder;

static const Builder k_b[HLR_FX_BUILDERS] = {
    { 0x80055820, K_FT3, 3, 7, 0x24, 0, 0, { 2, 4, 6 } },
    { 0x800559b0, K_FT3, 3, 7, 0x24, 0x58, 0, { 2, 4, 6 } },
    { 0x80055b70, K_FT4, 4, 9, 0x2c, 0, 0, { 2, 4, 6, 8 } },
    { 0x80055d48, K_FT4, 4, 9, 0x2c, 0x70, 0, { 2, 4, 6, 8 } },
    { 0x80055f54, K_GT4, 4, 12, 0x3c, 0, 0, { 2, 5, 8, 11 } },
    { 0x80056348, K_G3, 3, 6, 0x30, 0, 0x74, { 2, 4, 6 } },
    { 0x8005650c, K_G3, 3, 6, 0x30, 0x50, 0x74, { 2, 4, 6 } },
    { 0x80056704, K_G4, 4, 8, 0x38, 0, 0x94, { 2, 4, 6, 8 } },
    { 0x80056908, K_G4, 4, 8, 0x38, 0x68, 0x94, { 2, 4, 6, 8 } },
    { 0x80056b40, K_F3, 3, 4, 0x20, 0, 0x4c, { 2, 3, 4 } },
};

static inline u32 r32(u32 a) { return *(u32 *)(g_ram + (a & 0x1ffffc)); }
static inline u32 scr(u32 a) { return *(u32 *)(g_scratch + (a & 0x3fc)); }
static inline bool in_ram(u32 a) { return (a & 0x1fffffff) < 0x200000; }

static HlrFxStats S;
static GteState G;
static int s_b = -1;
static u32 s_obj, s_ot;
static bool s_draw;
static u32 s_codes[HLR_FX_BUILDERS];    /* command bytes seen per builder (bit n: code & 0x03 == n) */
static u32 s_semi_modes;                /* semi modes of the semi-transparent effect polygons (bit n: mode n) */

void hlr_fx_get_stats(HlrFxStats *st) { *st = S; }

static u32 ot_slot(u32 z)
{
    const u32 near = scr(SCR_NEAR), mid = scr(SCR_MID), far = scr(SCR_FAR);
    if (z < near) return (z * 0x155u) / (near + 1);
    if (z < mid) return ((z - near) * 0x400u) / (mid - (near - 1)) + 0x155;
    if (z < far) return ((z - mid) * 0x2abu) / (far - (mid - 1)) + 0x555;
    return 0x7ff;
}

void hlr_fx_pre(u32 builder, u32 obj, u32 ot)
{
    s_b = -1;
    if (hlr_get_mode() == HLR_OFF) return;
    for (int i = 0; i < HLR_FX_BUILDERS; i++) if (k_b[i].addr == builder) s_b = i;
    if (s_b < 0) return;
    s_draw = hlr_fx_native();
    s_obj = obj; s_ot = ot & 0x1ffffc;
    gte_save(&G);           /* RT / TR, H, OFX / OFY as the builder will use them (read only: a copy) */
}

/* world = cam.rt^-1 * (view - cam.tr), in double */
static bool cam_inverse(const HlrCam *k, double inv[3][3])
{
    const double a = k->rt[0][0], b = k->rt[0][1], c = k->rt[0][2];
    const double d = k->rt[1][0], e = k->rt[1][1], f = k->rt[1][2];
    const double g = k->rt[2][0], h = k->rt[2][1], i = k->rt[2][2];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (det > -1e-9 && det < 1e-9) return false;
    inv[0][0] = A / det; inv[0][1] = -(b * i - c * h) / det; inv[0][2] = (b * f - c * e) / det;
    inv[1][0] = B / det; inv[1][1] = (a * i - c * g) / det; inv[1][2] = -(a * f - c * d) / det;
    inv[2][0] = C / det; inv[2][1] = -(a * h - b * g) / det; inv[2][2] = (a * e - b * d) / det;
    return true;
}

static double fx_lift(void)
{
    static double l = -1.0;
    if (l < 0.0) { const char *e = getenv("DESTREGA_HLR_FX_LIFT"); l = e ? atof(e) : 1.0; if (l < 0.0) l = 0.0; }
    return l;
}

void hlr_fx_post(void)
{
    if (s_b < 0) return;
    const Builder *b = &k_b[s_b];
    const int bi = s_b;
    s_b = -1;
    S.calls[bi]++;
    if (!in_ram(s_obj) || !in_ram(s_ot)) { S.frames_bad++; return; }

    /* 1. the facing test: nothing drawn unless MAC3 > 0 */
    if (b->test) {
        GteState t = G;
        gte_write_data_on(&t, 0, r32(s_obj + b->test));
        gte_write_data_on(&t, 1, r32(s_obj + b->test + 4));
        gte_exec_on(&t, OP_FACING);
        if ((s32)gte_read_data_on(&t, 27) <= 0) { S.skipped[bi]++; return; }
    }

    /* 2. the vertices: host RTPT (+ RTPS) == the packet's XY words */
    GteState t = G;
    const u32 S4 = 4u * (b->len + 1u), V = s_obj + 2 * S4, idx = scr(SCR_IDX);
    const u32 pkt = (s_obj + idx * S4) & 0x1ffffc;
    for (int j = 0; j < 6; j++) gte_write_data_on(&t, j, r32(V + 4 * (u32)j));
    gte_exec_on(&t, OP_RTPT);
    u32 sxy[4], sz[4];
    for (int j = 0; j < 3; j++) { sxy[j] = gte_read_data_on(&t, 12 + j); sz[j] = gte_read_data_on(&t, 17 + j); }
    if (b->nv == 4) {
        gte_write_data_on(&t, 0, r32(V + 0x18));
        gte_write_data_on(&t, 1, r32(V + 0x1c));
        gte_exec_on(&t, OP_RTPS);
        sxy[3] = gte_read_data_on(&t, 14); sz[3] = gte_read_data_on(&t, 19);
    }
    bool bad = false;
    for (int j = 0; j < b->nv; j++) {
        S.vtx++;
        if (r32(pkt + 4 * (u32)b->xy[j]) != sxy[j]) { S.vtx_bad++; bad = true; }
    }
    if (bad) { S.frames_bad++; return; }

    /* 3. tag, polygon type, slot, draw-mode prim */
    const u32 w1 = r32(pkt + 4), code = w1 >> 24;
    if ((r32(pkt) >> 24) != b->len || (code & 0xfc) != b->code) { S.word_bad++; S.frames_bad++; return; }
    s32 zmin = (s32)sz[0];
    for (int j = 1; j < b->nv; j++) if ((s32)sz[j] < zmin) zmin = (s32)sz[j];
    const u32 slot = ot_slot((u32)zmin) & 0x7ff, head = r32(s_ot + 4 * slot) & 0xffffff;
    u32 mode_word = 0;
    if (b->mode) {
        const u32 mp = (s_obj + idx * 8 + b->mode) & 0x1ffffc;
        if (head != (mp & 0xffffff) || (r32(mp) & 0xffffff) != (pkt & 0xffffff) || (r32(mp) >> 24) != 1) {
            S.link_bad++; S.frames_bad++; return;
        }
        mode_word = r32(mp + 4);
        if ((mode_word >> 24) != 0xe1) { S.mode_bad++; S.frames_bad++; return; }
    } else if (head != (pkt & 0xffffff)) { S.link_bad++; S.frames_bad++; return; }
    S.audited++;
    S.drawn[bi]++;
    s_codes[bi] |= 1u << (code & 3);
    const bool tex = b->kind <= K_GT4;

    /* 4. the capture (effects drawn natively) */
    if (!s_draw) return;
    HlrFrame *f = hlr_frame_for_ot(s_ot);
    if (!f) { S.no_frame++; return; }
    HlrCam k;
    hlr_cam_from_gte(&k, &G);
    if (k.h != f->cam.h || k.ofx != f->cam.ofx || k.ofy != f->cam.ofy) { S.cam_diff++; return; }
    double inv[3][3];
    if (!cam_inverse(&f->cam, inv)) { S.cam_diff++; return; }
    u32 words[12];
    for (int j = 0; j < b->len; j++) words[j] = r32(pkt + 4 + 4 * (u32)j);
    /* view-space vertices, then the lift: the PS1 sorts an effect into the OT and draws it over
     * whatever came before, so a billboard that reaches into the floor (most do: they are screen
     * aligned and as tall as a fighter) is never cut by it. Here every vertex slides along its own
     * view ray towards the camera by the polygon's size (its longest diagonal, times
     * DESTREGA_HLR_FX_LIFT, default 1): the projection is unchanged (x/z, y/z stay the same), and
     * only geometry nearer than the effect's own extent can hide it (a wall between it and the
     * camera still does). */
    float o[4][3];
    for (int j = 0; j < b->nv; j++) {
        const u32 a = j < 3 ? V + 8 * (u32)j : V + 0x18;
        const float v[3] = { (float)(s16)r32(a), (float)(s16)(r32(a) >> 16), (float)(s16)r32(a + 4) };
        hlr_cam_view(&k, v, o[j]);
    }
    double ext = 0.0;
    for (int i = 0; i < b->nv; i++)
        for (int j = i + 1; j < b->nv; j++) {
            const double dx = o[i][0] - o[j][0], dy = o[i][1] - o[j][1], dz = o[i][2] - o[j][2];
            const double d = dx * dx + dy * dy + dz * dz;
            if (d > ext) ext = d;
        }
    ext = sqrt(ext) * fx_lift();
    HlrChr *hc = hlr_frame_add_chr(f);
    for (int j = 0; j < b->nv; j++) {
        const double z = o[j][2];
        double sc = 1.0;
        if (z > 0.0) {
            double zt = z - ext;
            if (zt < z * 0.25) zt = z * 0.25;          /* never nearer than a quarter of its depth */
            if (zt < 32.0) zt = z < 32.0 ? z : 32.0;    /* nor into the near plane (16) */
            sc = zt / z;
            if (sc < 1.0) S.lifted++;
        }
        const double q[3] = { o[j][0] * sc - f->cam.tr[0], o[j][1] * sc - f->cam.tr[1], o[j][2] * sc - f->cam.tr[2] };
        for (int r = 0; r < 3; r++) hc->v[j][r] = (float)(inv[r][0] * q[0] + inv[r][1] * q[1] + inv[r][2] * q[2]);
    }
    /* colours (0x00BBGGRR), UV, CLUT, page by packet layout (word index - 1 into words[]) */
    switch (b->kind) {
    case K_FT3: case K_FT4:
        for (int j = 0; j < b->nv; j++) {
            hc->col[j] = w1 & 0xffffff;
            hc->uv[j] = (u16)words[2 + 2 * j];
        }
        hc->clut = (u16)(words[2] >> 16); hc->tpage = (u16)(words[4] >> 16);
        break;
    case K_GT4:
        for (int j = 0; j < 4; j++) { hc->col[j] = words[3 * j] & 0xffffff; hc->uv[j] = (u16)words[2 + 3 * j]; }
        hc->clut = (u16)(words[2] >> 16); hc->tpage = (u16)(words[5] >> 16);
        break;
    case K_G3: case K_G4:
        for (int j = 0; j < b->nv; j++) hc->col[j] = words[2 * j] & 0xffffff;
        hc->tpage = (u16)mode_word;
        break;
    case K_F3:
        for (int j = 0; j < 3; j++) hc->col[j] = w1 & 0xffffff;
        hc->tpage = (u16)mode_word;
        break;
    }
    if (code & 2) s_semi_modes |= 1u << ((hc->tpage >> 5) & 3);
    (void)tex;
    hc->col[0] |= w1 & 0xff000000u;
    hc->pkt = pkt;
    hc->nv = b->nv; hc->code = (u8)code; hc->linked = 1;
    hc->model = 255; hc->fx = 1;
    hc->slot = (s16)slot;
    HlrPkt *hp = hlr_frame_add(f, pkt, 3, words, b->len);
    hp->chr = f->nc - 1;
    hlr_frame_reindex(f);
    S.attached++;
    {   /* DESTREGA_HLR_FX_FRAMES=1: one log line per vblank with effects attached (picks frames to look at) */
        static int on = -1; static u32 last = ~0u, n;
        if (on < 0) { const char *e = getenv("DESTREGA_HLR_FX_FRAMES"); on = e && *e == '1'; }
        if (on) {
            if (g_pgxp_frame != last && n) { rt_log("hlr-fx-frame: %u effect polygons attached", n); n = 0; }
            last = g_pgxp_frame; n++;
        }
    }
}

void hlr_fx_report(void)
{
    u64 calls = 0, skipped = 0;
    for (int i = 0; i < HLR_FX_BUILDERS; i++) { calls += S.calls[i]; skipped += S.skipped[i]; }
    if (!calls) return;
    rt_log("hlr-fx: %llu effect polygons built by the game, %llu audited, %llu facing away (not drawn); vertices %llu,"
           " %llu mismatched; %llu with other words, %llu linked elsewhere, %llu without a draw-mode word, %llu rejected",
           (unsigned long long)calls, (unsigned long long)S.audited, (unsigned long long)skipped,
           (unsigned long long)S.vtx, (unsigned long long)S.vtx_bad, (unsigned long long)S.word_bad,
           (unsigned long long)S.link_bad, (unsigned long long)S.mode_bad, (unsigned long long)S.frames_bad);
    char buf[512]; int n = 0;
    buf[0] = 0;
    for (int i = 0; i < HLR_FX_BUILDERS && n < (int)sizeof buf - 48; i++)
        if (S.calls[i])
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%08x %llu (codes %02x)", n ? ", " : "", k_b[i].addr,
                          (unsigned long long)S.calls[i], s_codes[i]);
    rt_log("hlr-fx: per builder: %s", buf);
    if (hlr_get_mode() == HLR_ON)
        rt_log("hlr-fx: %llu attached to a native frame, %llu without a frame, %llu with another camera; semi modes %x;"
               " %llu vertices lifted towards the camera (x %.2f of the polygon's size)",
               (unsigned long long)S.attached, (unsigned long long)S.no_frame, (unsigned long long)S.cam_diff, s_semi_modes,
               (unsigned long long)S.lifted, fx_lift());
}
