/* HLR fighter shadows: capture and audit (docs/HLR_PLAN.md phase 4; `--hlr audit|exact|on`).
 *
 * Read-only model of fighter_shadow_render (8007ec5c; analysis/NOTES.md "Shadows"). Per fighter
 * the game drapes a 45-vertex mesh over the floor (shadow_world_verts 800e70f0, {x, y, z} shorts,
 * y from ground_height under each vertex), projects every vertex with RTPS and the world-screen
 * matrix (800ed4c8) into shadow_screen_verts 800e6f80 ({SXY, SZ}), and fills 64 semi-transparent
 * gouraud triangles (packet records of 0x58 bytes at a2, this frame's half at + idx * 0x28; vertex
 * indices as shorts at record + 0x50 / 0x52 / 0x54): words 2 / 5 / 8 = the three SXY, the colour
 * bytes of words 1 / 4 / 7 = one grey (0x80 - height above the ground / 10, at least 0x18), the
 * UV / CLUT / page words and the command byte are set up once elsewhere. Every triangle is linked
 * (NCLIP is computed and ignored) at the OT slot of its first vertex's SZ (the 3-segment map).
 *
 * The wrapper in overrides/char_mesh.c calls hlr_shadow_pre with a0..a2 and hlr_shadow_post after
 * the original. Audit (every mode but off; the guest GTE is never touched): the world-screen matrix
 * in the GTE equals RAM's, a host RTPS of each world vertex reproduces its {SXY, SZ} word for word,
 * every packet holds the predicted XY words and grey and is linked at the predicted slot. A clean
 * call becomes 64 decal faces of the stage's frame (HlrChr with decal set) when the shadows are
 * drawn natively: hlr_native.c draws them on the native floor with a depth bias, after the floor
 * under them, without writing depth (so the fighter's feet stay in front). */
#include "psx.h"
#include "hlr.h"
#include <string.h>

#define WSMATRIX        0x800ed4c8u
#define SH_WORLD        0x800e70f0u     /* shadow_world_verts */
#define SH_SCREEN       0x800e6f80u     /* shadow_screen_verts */
#define SH_NV           45
#define SH_NT           64
#define SH_REC          0x58u
#define SCR_IDX         0x1f800000u
#define SCR_FAR         0x1f800008u
#define SCR_NEAR        0x1f80000cu
#define SCR_MID         0x1f800010u
#define OP_RTPS         0x4a180001

static inline u32 r32(u32 a) { return *(u32 *)(g_ram + (a & 0x1ffffc)); }
static inline s32 r16s(u32 a) { return (s16)*(u16 *)(g_ram + (a & 0x1ffffe)); }
static inline u32 scr(u32 a) { return *(u32 *)(g_scratch + (a & 0x3fc)); }
static inline bool in_ram(u32 a) { return (a & 0x1fffffff) < 0x200000; }

static HlrShadowStats S;
static GteState G;
static u32 s_fighter, s_ot, s_buf;
static bool s_run, s_draw;
static u64 s_last_seq;
static int s_in_update;
static u32 s_code, s_tpage, s_clut, s_kinds;    /* the packets' command / page / CLUT seen (bit 0: one, bit 1: several) */

void hlr_shadow_get_stats(HlrShadowStats *st) { *st = S; }

static u32 ot_slot(u32 z)
{
    const u32 near = scr(SCR_NEAR), mid = scr(SCR_MID), far = scr(SCR_FAR);
    if (z < near) return (z * 0x155u) / (near + 1);
    if (z < mid) return ((z - near) * 0x400u) / (mid - (near - 1)) + 0x155;
    if (z < far) return ((z - mid) * 0x2abu) / (far - (mid - 1)) + 0x555;
    return 0x7ff;
}

static bool linked_at(u32 slot, u32 pkt)
{
    const u32 ot = s_ot;
    u32 a = r32(ot + 4 * slot) & 0x1ffffc;
    for (int guard = 0; guard < 65536; guard++) {
        if (a >= ot && a < ot + 4 * HLR_OT_SLOTS) return false;
        if (a == pkt) return true;
        const u32 h = r32(a);
        if ((h & 0xffffff) == 0xffffff) return false;
        a = h & 0x1ffffc;
    }
    return false;
}

void hlr_shadow_pre(u32 fighter, u32 ot, u32 buf)
{
    s_run = false;
    if (hlr_get_mode() == HLR_OFF) return;
    s_draw = hlr_shadows_native();
    s_fighter = fighter; s_ot = ot & 0x1ffffc; s_buf = buf;
    s_run = true;
}

void hlr_shadow_post(void)
{
    if (!s_run) return;
    s_run = false;
    S.calls++;
    if (!in_ram(s_fighter) || !in_ram(s_buf) || !in_ram(s_ot)) { S.frames_bad++; return; }
    /* the GTE as the function left it: RT / TR = the world-screen matrix it loaded, H, OFX / OFY
     * and the depth cue as the frame set them (read only: a copy) */
    gte_save(&G);
    bool bad = false;
    for (int i = 0; i < 8; i++)
        if (gte_read_ctrl_on(&G, i) != r32(WSMATRIX + 4 * (u32)i)) { S.ws_bad++; bad = true; break; }
    if (bad) { S.frames_bad++; return; }

    /* 1. the 45 vertices: host RTPS == shadow_screen_verts */
    u32 sxy[SH_NV], sz[SH_NV];
    for (int i = 0; i < SH_NV; i++) {
        gte_write_data_on(&G, 0, r32(SH_WORLD + 8 * (u32)i));
        gte_write_data_on(&G, 1, r32(SH_WORLD + 8 * (u32)i + 4));
        gte_exec_on(&G, OP_RTPS);
        sxy[i] = gte_read_data_on(&G, 14); sz[i] = gte_read_data_on(&G, 19);
        S.vtx++;
        if (sxy[i] != r32(SH_SCREEN + 8 * (u32)i) || sz[i] != r32(SH_SCREEN + 8 * (u32)i + 4)) { S.vtx_bad++; bad = true; }
    }
    if (bad) { S.frames_bad++; return; }

    /* 2. the grey: 0x80 - (ground height under the centre - fighter y) / 10, at least 0x18 */
    s32 grey = 0x80 - (r16s(SH_WORLD + 2) - (s32)r32(s_fighter + 0x3c)) / 10;
    if (grey < 0x19) grey = 0x18;
    const u32 g3 = (u32)(grey & 0xff) * 0x010101u;

    /* 3. the triangles */
    HlrFrame *f = s_draw ? hlr_frame_for_ot(s_ot) : NULL;
    if (s_draw && !f) S.no_frame++;
    if (f && f->seq != s_last_seq) { s_last_seq = f->seq; s_in_update = 0; }
    const int first_chr = f ? f->nc : 0, first_pkt = f ? f->np : 0;
    const u32 half = scr(SCR_IDX) ? 0x28u : 0u;
    for (int t = 0; t < SH_NT && !bad; t++) {
        const u32 rec = s_buf + SH_REC * (u32)t, pkt = (rec + half) & 0x1ffffc;
        const int vi[3] = { r16s(rec + 0x50), r16s(rec + 0x52), r16s(rec + 0x54) };
        for (int j = 0; j < 3; j++) if (vi[j] < 0 || vi[j] >= SH_NV) { S.idx_bad++; bad = true; }
        if (bad) break;
        S.faces++;
        if ((r32(pkt) >> 24) != 9) { S.word_bad++; bad = true; break; }
        for (int j = 0; j < 3; j++) {
            if (r32(pkt + 8 + 12 * (u32)j) != sxy[vi[j]]) { S.word_bad++; bad = true; }
            if ((r32(pkt + 4 + 12 * (u32)j) & 0xffffff) != g3) { S.grey_bad++; bad = true; }
        }
        if (bad) break;
        const u32 slot = ot_slot(sz[vi[0]]) & 0x7ff;
        if (!linked_at(slot, pkt)) { S.link_bad++; bad = true; break; }
        u32 words[9];
        for (int j = 0; j < 9; j++) words[j] = r32(pkt + 4 + 4 * (u32)j);
        {
            const u32 c = words[0] >> 24, tp = words[5] >> 16, cl = words[2] >> 16;
            if (!s_kinds) { s_code = c; s_tpage = tp; s_clut = cl; s_kinds = 1; }
            else if (c != s_code || tp != s_tpage || cl != s_clut) s_kinds |= 2;
        }
        if (!f) continue;
        HlrChr *hc = hlr_frame_add_chr(f);
        memset(hc, 0, sizeof *hc);
        for (int j = 0; j < 3; j++) {
            const u32 w = SH_WORLD + 8 * (u32)vi[j];
            hc->v[j][0] = (float)r16s(w); hc->v[j][1] = (float)r16s(w + 2); hc->v[j][2] = (float)r16s(w + 4);
            hc->col[j] = g3;
            hc->uv[j] = (u16)words[2 + 3 * j];
        }
        hc->col[0] |= words[0] & 0xff000000u;
        hc->clut = (u16)(words[2] >> 16);
        hc->tpage = (u16)(words[5] >> 16);
        hc->pkt = pkt;
        hc->nv = 3; hc->code = (u8)(words[0] >> 24); hc->linked = 1;
        hc->model = (u8)(128 + (s_in_update & 63));
        hc->decal = 1;
        hc->slot = (s16)slot;
        HlrPkt *hp = hlr_frame_add(f, pkt, 2, words, 9);
        hp->chr = f->nc - 1;
    }
    if (bad) {
        S.frames_bad++;
        if (f) { f->nc = first_chr; f->np = first_pkt; }
        return;
    }
    S.audited++;
    if (f) {
        HlrCam k;
        hlr_cam_from_gte(&k, &G);
        bool same = true;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) same &= k.rt[r][c] == f->cam.rt[r][c];
            same &= k.tr[r] == f->cam.tr[r];
        }
        same &= k.h == f->cam.h && k.ofx == f->cam.ofx && k.ofy == f->cam.ofy;
        if (!same) { S.cam_diff++; f->nc = first_chr; f->np = first_pkt; return; }
        hlr_frame_reindex(f);
        S.attached++;
        s_in_update++;
    }
}

void hlr_shadow_report(void)
{
    if (!S.calls) return;
    rt_log("hlr-shadows: %llu shadows drawn by the game, %llu audited; vertices %llu, %llu mismatched;"
           " triangles %llu, %llu with other words, %llu with another grey, %llu linked elsewhere, %llu bad indices;"
           " %llu with another camera matrix, %llu rejected",
           (unsigned long long)S.calls, (unsigned long long)S.audited, (unsigned long long)S.vtx,
           (unsigned long long)S.vtx_bad, (unsigned long long)S.faces, (unsigned long long)S.word_bad,
           (unsigned long long)S.grey_bad, (unsigned long long)S.link_bad, (unsigned long long)S.idx_bad,
           (unsigned long long)S.ws_bad, (unsigned long long)S.frames_bad);
    if (s_kinds)
        rt_log("hlr-shadows: command %02x, texture page %04x (semi mode %u, %u-bit), CLUT %04x%s",
               s_code, s_tpage, (s_tpage >> 5) & 3, 4u << ((s_tpage >> 7) & 3), s_clut,
               (s_kinds & 2) ? "; other values seen too" : "");
    if (hlr_get_mode() == HLR_ON)
        rt_log("hlr-shadows: %llu attached to a native frame, %llu without a frame, %llu with another camera",
               (unsigned long long)S.attached, (unsigned long long)S.no_frame, (unsigned long long)S.cam_diff);
}
