/* HLR stage capture and audit (docs/HLR_PLAN.md 3.2; `--hlr audit|exact|on`).
 *
 * Read-only model of the real pass of stage_render (80044210). Before the original runs, a
 * host-side walker reproduces what the game's stage builders will put into the scene ordering
 * table: it walks the stage root (800c03b8), runs every object's visibility test (80043fd4) and
 * every polygon command through the builders' GTE sequences on a host-owned GteState
 * (gte_exec_on: the guest GTE is never touched) and predicts each packet's address, OT slot,
 * tag (length + link), XY words, colour words and (subdivision packets) UV halves. After the
 * original has run, the prediction is compared with RAM and the ordering table:
 *
 *   - every predicted packet's predicted words against RAM (mismatched packets);
 *   - every node the original added to a slot (walked from the new head back to the head
 *     before the pass) must be a predicted packet (else "unexplained");
 *   - every predicted linked packet must be in its slot's chain (else "missing");
 *   - the final head of every slot against the predicted head.
 *
 * A frame whose audit is clean becomes a capture (hlr.c HlrFrame) when the HLR draws: the
 * predicted words plus, for the words the builders do not compute (UV / CLUT / texture page, flat
 * colours), the words in RAM after the pass. Nothing is written to guest state: RAM and scratchpad
 * are only read, the GTE work happens on a copy. Ported builders (gen code read line by line;
 * analysis/NOTES.md "Stage builders in full"):
 *   0x20 F3 8003fcb8, 0x28 F4 800401f8, 0x34 GT3 lit 80040820, 0x3c GT4 lit 80040e98,
 *   0x35 GT3 prelit 8004163c, 0x3d GT4 prelit 80041c14, visibility 80043fd4; objects with
 *   flag 0x2000 (0x34 / 0x35 / 0x3c / 0x3d): independent polygons 800426c4 (tris) / 800433c4
 *   (quads), which split a polygon wider than 900 px (or taller than 450, or reaching beyond
 *   +-512 vertically) into four through 800423e0 / 80042fec (at most 20 of each per frame).
 */
#include "psx.h"
#include "hlr.h"
#include <stdlib.h>
#include <string.h>

/* ---- guest layout ---------------------------------------------------------------- */
#define GP              0x800bfde8u
#define GP_ROOT         (GP + 0x5d0)       /* 800c03b8: stage root */
#define GP_NORMBASE     (GP + 0x5cc)       /* 800c03b4: base of normal-index / colour streams */
#define GP_PKT0         (GP + 0x5e8)       /* packet buffer, double-buffer index 0 */
#define GP_PKT1         (GP + 0x5e4)       /* packet buffer, index 1 */
#define GP_SUB_ON       (GP + 0x5bc)       /* non-zero: the prologue re-bases the subdivision cursors */
#define GP_SUBT_CUR     (GP + 0x5c0)       /* subdivided triangle packets: cursor */
#define GP_SUBQ_CUR     (GP + 0x5c4)       /* subdivided quad packets: cursor */
#define GP_SUBT_OFF     (GP + 0x5d4)       /* ... their offset in the packet buffer (-1: none) */
#define GP_SUBT_CNT     (GP + 0x5d8)       /* ... count this frame (< 20) */
#define GP_SUBQ_OFF     (GP + 0x5dc)
#define GP_SUBQ_CNT     (GP + 0x5e0)
#define SUB_MAX         20
#define SCR_IDX         0x1f800000u
#define SCR_FAR         0x1f800008u
#define SCR_NEAR        0x1f80000cu
#define SCR_MID         0x1f800010u
#define OT_POINTERS     0x800c0724u
#define WSMATRIX        0x800ed4c8u
#define LLM_SRC         0x800cc6e0u
#define LCM_SRC         0x800cc6c0u
#define SLOTS           HLR_OT_SLOTS

static inline u32 r32(u32 a) { return *(u32 *)(g_ram + (a & 0x1ffffc)); }
static inline s32 r16s(u32 a) { return (s16)*(u16 *)(g_ram + (a & 0x1ffffe)); }
static inline u32 r16(u32 a) { return *(u16 *)(g_ram + (a & 0x1ffffe)); }
static inline u32 s32r(u32 a) { return *(u32 *)(g_scratch + (a & 0x3fc)); }

/* ---- host GTE ------------------------------------------------------------------- */
static GteState G;
#define CMD(x)    gte_exec_on(&G, (x))
#define DW(r, v)  gte_write_data_on(&G, (r), (v))
#define DR(r)     gte_read_data_on(&G, (r))
#define CW(r, v)  gte_write_ctrl_on(&G, (r), (v))
#define LOADV3(a) do { for (int i_ = 0; i_ < 6; i_++) DW(i_, r32((a) + 4 * i_)); } while (0)
#define LOADV1(a) do { DW(0, r32(a)); DW(1, r32((a) + 4)); } while (0)

enum { OP_RTPS = 0x4a180001, OP_RTPT = 0x4a280030, OP_DPCT = 0x4af8002a, OP_DPCS = 0x4a780010,
       OP_NCDT = 0x4af80416, OP_NCDS = 0x4ae80413, OP_MVMVA = 0x4a486012 };

/* ---- prediction ------------------------------------------------------------------ */
enum { K_F3, K_F4, K_GT3, K_GT4, K_N };
static const char *k_name[K_N] = { "F3", "F4", "GT3", "GT4" };
static const int k_len[K_N] = { 4, 5, 9, 12 };            /* tag length byte */

typedef struct {
    u32 addr;           /* & 0xffffff */
    s16 slot;           /* -1: not linked (culled, too near, or a subdivided parent) */
    u8 kind, nw;
    u32 w[13];
    u16 cmp;            /* bit i: word i predicted (tag is word 0) */
    u16 cmplo;          /* bit i: only the low half of word i predicted */
    u8 seen, sub;
} PPkt;

static PPkt *s_p;
static int s_np, s_cap;
static u32 s_ot_pre[SLOTS], s_ot_pred[SLOTS];
static u32 s_ot;                                /* OT base (KSEG0) */
static u32 s_normbase;
static bool s_valid;
static u32 s_tcur, s_qcur;                      /* subdivision cursors / counts (prologue) */
static int s_tcnt, s_qcnt;

/* hash addr -> index (open addressing) */
static int *s_hash;
static int s_hcap;

static u64 st_frames, st_pkts, st_linked, st_culled, st_mismatch, st_unexpl, st_missing, st_head;
static u64 st_objs, st_objs_vis, st_badwalk, st_word_mis[3];
static u64 st_alt_objs, st_alt_tris, st_alt_quads, st_alt_split, st_alt_subs, st_alt_full;
static u64 st_kind[K_N], st_kind_mis[K_N];
static int s_logged;
static u64 st_rejected;

static PPkt *new_pkt(u32 addr, int kind, int nw)
{
    if (s_np == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 4096;
        s_p = realloc(s_p, (size_t)s_cap * sizeof *s_p);
        if (!s_p) rt_fatal("hlr-stage: out of memory");
    }
    PPkt *p = &s_p[s_np++];
    memset(p, 0, sizeof *p);
    p->addr = addr & 0xffffff; p->kind = (u8)kind; p->nw = (u8)nw; p->slot = -1;
    return p;
}

static inline s32 sx(u32 w) { return (s16)(w & 0xffff); }
static inline s32 sy(u32 w) { return (s16)(w >> 16); }

/* screen bounds of n XY words */
typedef struct { s32 xmin, xmax, ymin, ymax; } Box;
static Box box_of(const u32 *xyw, int n)
{
    Box b = { sx(xyw[0]), sx(xyw[0]), sy(xyw[0]), sy(xyw[0]) };
    for (int i = 1; i < n; i++) {
        s32 x = sx(xyw[i]), y = sy(xyw[i]);
        if (x < b.xmin) b.xmin = x; if (x > b.xmax) b.xmax = x;
        if (y < b.ymin) b.ymin = y; if (y > b.ymax) b.ymax = y;
    }
    return b;
}
/* the builders' culling: x max < -254 or x min >= 255, y max < -118 or y min >= 119 (the x
 * bounds are WS_IMM: the real pass, which is the one modelled, runs with the 4:3 values) */
static bool box_on_screen(Box b) { return !(b.xmax < -254 || b.xmin >= 255 || b.ymax < -118 || b.ymin >= 119); }

static bool on_screen(const u32 *w, const int *xy, int n)
{
    u32 t[4];
    for (int i = 0; i < n; i++) t[i] = w[xy[i]];
    return box_on_screen(box_of(t, n));
}

/* SZ -> ordering-table slot (the same three linear ranges in every builder); -1 = too near */
static int ot_slot(u32 sz)
{
    if (sz < 400) return -1;
    u32 nearz = s32r(SCR_NEAR), mid = s32r(SCR_MID), farz = s32r(SCR_FAR);
    if (sz < nearz) return (int)((sz * 341u) / (nearz + 1));
    if (sz < mid) return (int)((((sz - nearz) << 10) / (mid - (nearz - 1))) + 341);
    if (sz < farz) return (int)(((sz - mid) * 683u) / (farz - (mid - 1)) + 1365);
    return 2047;
}

static void link(PPkt *p, int slot)
{
    p->slot = (s16)slot;
    p->w[0] = (s_ot_pred[slot] & 0xffffff) | (u32)k_len[p->kind] << 24;
    p->cmp |= 1;
    s_ot_pred[slot] = p->addr;
}

static u32 smax(u32 a, u32 b) { return (s32)a < (s32)b ? b : a; }

/* ---- triangle builders (F3 8003fcb8, GT3 lit 80040820, GT3 prelit 8004163c) ------ */
enum { C_NONE, C_LIT, C_PRELIT };

static void tri_colour(PPkt *p, int col, u32 s, u32 ir0)
{
    if (col == C_PRELIT) {
        DW(8, ir0);
        DW(20, r32(s)); DW(21, r32(s + 4)); DW(22, r32(s + 8)); DW(6, r32(s + 8));
        CMD(OP_DPCT);
    } else {
        DW(6, 0x34808080);
        u32 n0 = r32(s) + s_normbase, n1 = r32(s + 4) + s_normbase, n2 = r32(s + 8) + s_normbase;
        DW(0, r32(n0)); DW(1, r32(n0 + 4)); DW(2, r32(n1)); DW(3, r32(n1 + 4));
        DW(4, r32(n2)); DW(5, r32(n2 + 4));
        DW(8, ir0);
        CMD(OP_NCDT);
    }
    p->w[1] = DR(20); p->w[4] = DR(21); p->w[7] = DR(22);
    p->cmp |= 1 << 1 | 1 << 4 | 1 << 7;
}

static u32 build_tri(u32 *ctx, int cnt, int strips, int col)
{
    const int f3 = col == C_NONE;
    static const int xy_f3[3] = { 2, 3, 4 }, xy_gt[3] = { 2, 5, 8 };
    const int *xy = f3 ? xy_f3 : xy_gt;
    const int kind = f3 ? K_F3 : K_GT3, nw = f3 ? 5 : 10;
    u32 v = ctx[0], pk = ctx[1], s = ctx[2];
    int n = 0, tot = 0;
    u32 w[13] = { 0 }, sz[3], ir0 = 0;
    for (; strips > 0; strips--) {
        LOADV3(v); CMD(OP_RTPT);
        tot += r16s(v + 6); n++;
        w[xy[0]] = DR(12); w[xy[1]] = DR(13); w[xy[2]] = DR(14);
        if (col == C_LIT) ir0 = DR(8);
        sz[0] = DR(17); sz[1] = DR(18); sz[2] = DR(19);
        v += 24;
        int idx = 0;
        for (;;) {
            PPkt *p = new_pkt(pk, kind, nw);
            for (int i = 0; i < 3; i++) { p->w[xy[i]] = w[xy[i]]; p->cmp |= 1 << xy[i]; }
            int slot = -2;
            if (on_screen(w, xy, 3)) slot = ot_slot(smax(sz[0], smax(sz[1], sz[2])));
            if (slot >= 0) {
                if (col != C_NONE) tri_colour(p, col, s, col == C_LIT ? ir0 : DR(8));
                link(p, slot);
            }
            pk += (u32)nw * 4; s += 12;
            if (!(n < tot)) break;
            n++;
            LOADV1(v); CMD(OP_RTPS);
            u32 a = w[xy[2]], b = w[xy[1]];
            w[xy[0]] = a; w[xy[1]] = b; w[xy[2]] = DR(14);
            sz[idx] = DR(19); idx ^= 2;
            v += 8;
        }
    }
    while (n < cnt) {
        LOADV3(v); CMD(OP_RTPT);
        v += 24; n++;
        w[xy[0]] = DR(12); w[xy[1]] = DR(13); w[xy[2]] = DR(14);
        PPkt *p = new_pkt(pk, kind, nw);
        for (int i = 0; i < 3; i++) { p->w[xy[i]] = w[xy[i]]; p->cmp |= 1 << xy[i]; }
        int slot = -2;
        if (on_screen(w, xy, 3)) slot = ot_slot(smax(DR(17), smax(DR(18), DR(19))));
        if (slot >= 0) {
            if (col != C_NONE) tri_colour(p, col, s, DR(8));
            link(p, slot);
        }
        pk += (u32)nw * 4; s += 12;
    }
    ctx[1] = pk; ctx[2] = f3 ? ctx[2] : s;
    return v;
}

/* ---- quad builders (F4 800401f8, GT4 lit 80040e98, GT4 prelit 80041c14) ---------- */
static void quad_colour(PPkt *p, int col, u32 s, u32 ir0)
{
    if (col == C_PRELIT) {
        DW(8, ir0);
        DW(20, r32(s)); DW(21, r32(s + 4)); DW(22, r32(s + 8)); DW(6, r32(s + 8));
        CMD(OP_DPCT);
        p->w[1] = DR(20); p->w[4] = DR(21); p->w[7] = DR(22);
        DW(6, r32(s + 12));
        CMD(OP_DPCS);
        p->w[10] = DR(22);
    } else {
        DW(8, ir0);
        DW(6, 0x3c808080);
        u32 n3 = r32(s + 12) + s_normbase;
        DW(0, r32(n3)); DW(1, r32(n3 + 4));
        CMD(OP_NCDS);
        p->w[10] = DR(22);
        u32 n0 = r32(s) + s_normbase, n1 = r32(s + 4) + s_normbase, n2 = r32(s + 8) + s_normbase;
        DW(0, r32(n0)); DW(1, r32(n0 + 4)); DW(2, r32(n1)); DW(3, r32(n1 + 4));
        DW(4, r32(n2)); DW(5, r32(n2 + 4));
        CMD(OP_NCDT);
        p->w[1] = DR(20); p->w[4] = DR(21); p->w[7] = DR(22);
    }
    p->cmp |= 1 << 1 | 1 << 4 | 1 << 7 | 1 << 10;
}

static u32 build_quad(u32 *ctx, int cnt, int strips, int col)
{
    const int f4 = col == C_NONE;
    static const int xy_f4[4] = { 2, 3, 4, 5 }, xy_gt[4] = { 2, 5, 8, 11 };
    const int *xy = f4 ? xy_f4 : xy_gt;
    const int kind = f4 ? K_F4 : K_GT4, nw = f4 ? 6 : 13;
    u32 v = ctx[0], pk = ctx[1], s = ctx[2];
    int n = 0, tot = 0;
    u32 w[13] = { 0 };
    for (; strips > 0; strips--) {
        LOADV1(v); CMD(OP_RTPS);
        tot += r16s(v + 6);
        v += 8;
        while (n < tot) {
            w[xy[0]] = DR(14);
            LOADV3(v); CMD(OP_RTPT);
            n++; v += 24;
            w[xy[1]] = DR(12); w[xy[2]] = DR(13); w[xy[3]] = DR(14);
            PPkt *p = new_pkt(pk, kind, nw);
            for (int i = 0; i < 4; i++) { p->w[xy[i]] = w[xy[i]]; p->cmp |= 1 << xy[i]; }
            int slot = -2;
            if (on_screen(w, xy, 4))
                slot = ot_slot(smax(DR(16), smax(DR(17), smax(DR(18), DR(19)))));
            if (slot >= 0) {
                if (col != C_NONE) quad_colour(p, col, s, DR(8));
                link(p, slot);
            }
            pk += (u32)nw * 4; s += 16;
        }
    }
    while (n < cnt) {
        LOADV3(v); CMD(OP_RTPT);
        n++;
        LOADV1(v + 24);
        w[xy[0]] = DR(12); w[xy[1]] = DR(13); w[xy[2]] = DR(14);
        u32 ir0 = DR(8);
        CMD(OP_RTPS);
        v += 32;
        w[xy[3]] = DR(14);
        PPkt *p = new_pkt(pk, kind, nw);
        for (int i = 0; i < 4; i++) { p->w[xy[i]] = w[xy[i]]; p->cmp |= 1 << xy[i]; }
        int slot = -2;
        if (on_screen(w, xy, 4))
            slot = ot_slot(smax(DR(16), smax(DR(17), smax(DR(18), DR(19)))));
        if (slot >= 0) {
            if (col != C_NONE) quad_colour(p, col, s, ir0);
            link(p, slot);
        }
        pk += (u32)nw * 4; s += 16;
    }
    ctx[1] = pk; ctx[2] = f4 ? ctx[2] : s;
    return v;
}

/* ---- object visibility (80043fd4) ------------------------------------------------ */
static bool visible(u32 flags, u32 ptr, u32 mode)
{
    bool vis = true;
    if (flags & 5) {
        LOADV3(ptr);
        if (flags & 1) {
            CMD(OP_MVMVA);
            if (!((s32)DR(27) > 0)) vis = false;        /* (the game also prints a debug line) */
        } else {
            CMD(OP_RTPT);
            u32 xyw[4] = { DR(12), DR(13), DR(14), 0 };
            LOADV1(ptr + 24); CMD(OP_RTPS);
            xyw[3] = DR(14);
            s32 xmin = sx(xyw[0]), xmax = xmin, ymin = sy(xyw[0]), ymax = ymin;
            for (int i = 1; i < 4; i++) {
                s32 x = sx(xyw[i]), y = sy(xyw[i]);
                if (x < xmin) xmin = x; if (x > xmax) xmax = x;
                if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            }
            if (xmax < -256 || xmin >= 257 || ymax < -120 || ymin >= 121) vis = false;
        }
    }
    if (flags & 0x4000) vis = false;
    else if ((flags & 8) && !(flags & 0x8000)) vis = false;
    if (mode < 2) { if (flags & 0x40) vis = false; }
    else if (mode == 2) { if (flags & 0x100) vis = false; }
    return vis;
}

/* ---- independent polygons with subdivision (objects with flag 0x2000) ---------------- */
/* A vertex of a subdivided polygon as 800426c4 / 800433c4 build it on their stack (12 bytes):
 * the model vertex's x, y, z, the parent packet's u / v and colour word. */
typedef struct { s32 x, y, z; u32 uv; u32 col; } SubV;

static SubV subv(u32 vtx, u32 pkt, int uvw, int colw)
{
    SubV r = { r16s(vtx), r16s(vtx + 2), r16s(vtx + 4), r16(pkt + 4 * (u32)uvw), 0 };
    r.col = colw;            /* placeholder, set by the caller from the predicted colour */
    return r;
}
/* the midpoint: x / y / z with C's truncating division, u / v / r / g / b (a + b) >> 1 per byte,
 * the code byte (colour bits 24-31) from a */
static SubV mid(SubV a, SubV b)
{
    SubV m;
    m.x = (s16)((a.x + b.x) / 2); m.y = (s16)((a.y + b.y) / 2); m.z = (s16)((a.z + b.z) / 2);
    m.uv = (((a.uv & 0xff) + (b.uv & 0xff)) >> 1) | ((((a.uv >> 8) & 0xff) + ((b.uv >> 8) & 0xff)) >> 1) << 8;
    m.col = a.col & 0xff000000u;
    for (int sh = 0; sh < 24; sh += 8) m.col |= ((((a.col >> sh) & 0xff) + ((b.col >> sh) & 0xff)) >> 1) << sh;
    return m;
}
static void subv_load(int base, SubV v)
{
    DW(base, ((u32)v.x & 0xffff) | (u32)v.y << 16);
    DW(base + 1, ((u32)v.z & 0xffff) | v.uv << 16);
}

/* 800423e0: one quarter of a subdivided triangle into the next packet of the subdivision buffer */
static void sub_tri(SubV a, SubV b, SubV c)
{
    if (s_tcnt >= SUB_MAX) return;
    subv_load(0, a); subv_load(2, b); subv_load(4, c);
    CMD(OP_RTPT);
    static const int xy[3] = { 2, 5, 8 };
    u32 w[13] = { 0 };
    w[2] = DR(12); w[5] = DR(13); w[8] = DR(14);
    if (!on_screen(w, xy, 3)) return;          /* the slot is overwritten by the next one */
    int slot = ot_slot(smax(DR(17), smax(DR(18), DR(19))));
    if (slot < 0) return;
    PPkt *p = new_pkt(s_tcur, K_GT3, 10);
    p->sub = 1;
    p->w[2] = w[2]; p->w[5] = w[5]; p->w[8] = w[8];
    p->w[1] = a.col; p->w[4] = b.col; p->w[7] = c.col;
    p->w[3] = a.uv; p->w[6] = b.uv; p->w[9] = c.uv;
    p->cmp |= 1 << 1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 3 | 1 << 6 | 1 << 9;
    p->cmplo |= 1 << 3 | 1 << 6 | 1 << 9;
    link(p, slot);
    s_tcur += 40; s_tcnt++;
    st_alt_subs++;
}

/* 80042fec: one quarter of a subdivided quad */
static void sub_quad(SubV a, SubV b, SubV c, SubV d)
{
    if (s_qcnt >= SUB_MAX) return;
    subv_load(0, a); subv_load(2, b); subv_load(4, c);
    CMD(OP_RTPT);
    u32 w[13] = { 0 };
    w[2] = DR(12); w[5] = DR(13); w[8] = DR(14);
    subv_load(0, d);
    CMD(OP_RTPS);
    w[11] = DR(14);
    static const int xy[4] = { 2, 5, 8, 11 };
    if (!on_screen(w, xy, 4)) return;
    int slot = ot_slot(smax(DR(16), smax(DR(17), DR(18))));    /* SZ of a, b, c only */
    if (slot < 0) return;
    PPkt *p = new_pkt(s_qcur, K_GT4, 13);
    p->sub = 1;
    p->w[2] = w[2]; p->w[5] = w[5]; p->w[8] = w[8]; p->w[11] = w[11];
    p->w[1] = a.col; p->w[4] = b.col; p->w[7] = c.col; p->w[10] = d.col;
    p->w[3] = a.uv; p->w[6] = b.uv; p->w[9] = c.uv; p->w[12] = d.uv;
    p->cmp |= 0x1ffe;
    p->cmplo |= 1 << 3 | 1 << 6 | 1 << 9 | 1 << 12;
    link(p, slot);
    s_qcur += 52; s_qcnt++;
    st_alt_subs++;
}

/* 800426c4: independent triangles; one wider than 900 px (or taller than 450, or beyond +-512
 * vertically) is not linked itself but drawn as four halves-of-edges triangles */
static u32 alt_tri(u32 *ctx, int cnt, int col)
{
    u32 v = ctx[0], pk = ctx[1], s = ctx[2];
    static const int xy[3] = { 2, 5, 8 };
    for (int n = 0; n < cnt; n++, v += 24, pk += 40, s += 12) {
        LOADV3(v); CMD(OP_RTPT);
        PPkt *p = new_pkt(pk, K_GT3, 10);
        p->w[2] = DR(12); p->w[5] = DR(13); p->w[8] = DR(14);
        p->cmp |= 1 << 2 | 1 << 5 | 1 << 8;
        st_alt_tris++;
        u32 t[3] = { p->w[2], p->w[5], p->w[8] };
        Box b = box_of(t, 3);
        if (!box_on_screen(b)) continue;
        bool big = b.xmax - b.xmin >= 901;
        u32 ir0 = DR(8);
        int slot = ot_slot(smax(DR(17), smax(DR(18), DR(19))));
        if (slot < 0) continue;
        if (b.ymax - b.ymin >= 451 || b.ymax >= 513 || b.ymin < -512) big = true;
        tri_colour(p, col, s, ir0);
        if (!big) { link(p, slot); continue; }
        st_alt_split++;
        const u32 pa = 0x80000000u | p->addr;
        SubV A = subv(v, pa, 3, 0), B = subv(v + 8, pa, 6, 0), C = subv(v + 16, pa, 9, 0);
        A.col = p->w[1]; B.col = p->w[4]; C.col = p->w[7];
        SubV AB = mid(A, B), BC = mid(B, C), CA = mid(C, A);
        sub_tri(A, AB, CA); sub_tri(BC, AB, B); sub_tri(BC, C, CA); sub_tri(AB, BC, CA);
        (void)xy;
    }
    ctx[1] = pk; ctx[2] = s;
    return v;
}

/* 800433c4: independent quads, subdivided the same way into four quads */
static u32 alt_quad(u32 *ctx, int cnt, int col)
{
    u32 v = ctx[0], pk = ctx[1], s = ctx[2];
    for (int n = 0; n < cnt; n++, v += 32, pk += 52, s += 16) {
        LOADV3(v); CMD(OP_RTPT);
        LOADV1(v + 24);
        PPkt *p = new_pkt(pk, K_GT4, 13);
        p->w[2] = DR(12); p->w[5] = DR(13); p->w[8] = DR(14);
        u32 ir0 = DR(8);
        CMD(OP_RTPS);
        p->w[11] = DR(14);
        p->cmp |= 1 << 2 | 1 << 5 | 1 << 8 | 1 << 11;
        st_alt_quads++;
        u32 t[4] = { p->w[2], p->w[5], p->w[8], p->w[11] };
        Box b = box_of(t, 4);
        if (!box_on_screen(b)) continue;
        bool big = b.xmax - b.xmin >= 901;
        int slot = ot_slot(smax(DR(16), smax(DR(17), smax(DR(18), DR(19)))));
        if (slot < 0) continue;
        if (b.ymax - b.ymin >= 451 || b.ymax >= 513 || b.ymin < -512) big = true;
        quad_colour(p, col, s, ir0);
        if (!big) { link(p, slot); continue; }
        st_alt_split++;
        const u32 pa = 0x80000000u | p->addr;
        SubV A = subv(v, pa, 3, 0), B = subv(v + 8, pa, 6, 0), C = subv(v + 16, pa, 9, 0), D = subv(v + 24, pa, 12, 0);
        A.col = p->w[1]; B.col = p->w[4]; C.col = p->w[7]; D.col = p->w[10];
        SubV AB = mid(A, B), AC = mid(A, C), AD = mid(A, D), BD = mid(B, D), DC = mid(D, C);
        sub_quad(A, AB, AC, AD); sub_quad(B, AB, BD, AD); sub_quad(D, DC, BD, AD); sub_quad(C, DC, AC, AD);
    }
    ctx[1] = pk; ctx[2] = s;
    return v;
}

/* ---- the walk (stage_render 80044210 up to its builder calls) ---------------------- */
static bool s_capture;              /* this pass becomes an HlrFrame (hlr_draws()) */

void hlr_stage_pre(u32 mode)
{
    s_np = 0; s_valid = false;
    s_capture = hlr_draws() && (hlr_get_parts() & HLR_PART_STAGE);
    if (!s_capture && hlr_get_mode() != HLR_AUDIT) return;
    const u32 idx = s32r(SCR_IDX);
    s_ot = r32(OT_POINTERS + idx * 0x14);
    if ((s_ot & 0xff000000u) != 0x80000000u) return;
    for (int i = 0; i < SLOTS; i++) s_ot_pre[i] = s_ot_pred[i] = r32(s_ot + 4 * (u32)i) & 0xffffff;
    s_normbase = r32(GP_NORMBASE);
    const u32 pktbuf = r32(idx == 0 ? GP_PKT0 : GP_PKT1);

    gte_save(&G);                       /* the guest's GTE as stage_render finds it */
    for (int i = 0; i < 5; i++) CW(8 + i, r32(LLM_SRC + 4 * (u32)i));
    for (int i = 0; i < 5; i++) CW(16 + i, r32(LCM_SRC + 4 * (u32)i));
    for (int i = 0; i < 5; i++) CW(i, r32(WSMATRIX + 4 * (u32)i));
    for (int i = 0; i < 3; i++) CW(5 + i, r32(WSMATRIX + 0x14 + 4 * (u32)i));

    /* the prologue: subdivision counts start at 0, the cursors are re-based on this frame's
     * packet buffer when 800c03a4 is set (else they keep the previous frame's value) */
    s_tcnt = s_qcnt = 0;
    s_tcur = r32(GP_SUBT_CUR); s_qcur = r32(GP_SUBQ_CUR);
    if (r32(GP_SUB_ON)) {
        const u32 q = r32(GP_SUBQ_OFF), t = r32(GP_SUBT_OFF);
        if (q != 0xffffffffu) s_qcur = q + pktbuf;
        if (t != 0xffffffffu) s_tcur = t + pktbuf;
    }

    const u32 root = r32(GP_ROOT);
    const u32 nobj = r32(root);
    if (nobj > 4096) return;
    for (u32 o = 0; o < nobj; o++) {
        const u32 ent = root + 4 + 16 * o;
        const u32 flags = r32(ent + 12);
        u32 data = r32(ent) + root, start = data;
        if (flags & 4) data += 32; else if (flags & 1) data += 8;
        const u32 ncmd = r32(data);
        data += 4;
        u32 vtx = data + ncmd * 4;
        st_objs++;
        if (o != 0 && !visible(flags, (flags & 5) ? start : vtx, mode)) continue;
        st_objs_vis++;
        const bool alt = (flags & 0x2000) != 0;
        if (alt) st_alt_objs++;
        u32 ctx[3] = { vtx, r32(ent + 4) + pktbuf, r32(ent + 8) + s_normbase };
        for (u32 c = 0; c < ncmd; c++) {
            const u32 cmd = r32(data + 4 * c);
            const int type = (int)(cmd & 0xff), cnt = (int)((cmd >> 8) & 0xfff), strips = (int)(cmd >> 20);
            switch (type) {
            case 0x20: ctx[0] = build_tri(ctx, cnt, strips, C_NONE); break;
            case 0x28: ctx[0] = build_quad(ctx, cnt, strips, C_NONE); break;
            case 0x34: ctx[0] = alt ? alt_tri(ctx, cnt, C_LIT) : build_tri(ctx, cnt, strips, C_LIT); break;
            case 0x35: ctx[0] = alt ? alt_tri(ctx, cnt, C_PRELIT) : build_tri(ctx, cnt, strips, C_PRELIT); break;
            case 0x3c: ctx[0] = alt ? alt_quad(ctx, cnt, C_LIT) : build_quad(ctx, cnt, strips, C_LIT); break;
            case 0x3d: ctx[0] = alt ? alt_quad(ctx, cnt, C_PRELIT) : build_quad(ctx, cnt, strips, C_PRELIT); break;
            default: break;                                     /* the game skips it too */
            }
        }
    }
    s_valid = true;
}

static int find(u32 addr)
{
    u32 h = (addr * 2654435761u) & (u32)(s_hcap - 1);
    while (s_hash[h] >= 0) {
        if (s_p[s_hash[h]].addr == addr) return s_hash[h];
        h = (h + 1) & (u32)(s_hcap - 1);
    }
    return -1;
}

static void capture(void)
{
    HlrFrame *f = hlr_frame_begin(s_ot & 0x1fffff);
    if (!f) return;
    u32 words[13];
    for (int i = 0; i < s_np; i++) {
        const PPkt *p = &s_p[i];
        if (p->slot < 0) continue;
        /* the predicted words, the rest (UV, CLUT, texture page) as the game left them; the audit
         * has just shown the two agree, so this is the packet the DMA will send */
        for (int k = 1; k < p->nw; k++) {
            u32 m = r32((0x80000000u | p->addr) + 4 * (u32)k);
            if (p->cmplo >> k & 1) words[k] = (m & 0xffff0000u) | (p->w[k] & 0xffff);
            else if (p->cmp >> k & 1) words[k] = p->w[k];
            else words[k] = m;
        }
        hlr_frame_add(f, p->addr, p->kind, words + 1, p->nw - 1);
    }
    hlr_frame_commit(f);
}

void hlr_stage_post(void)
{
    if (!s_valid) return;
    s_valid = false;
    st_frames++;
    const u64 bad0 = st_mismatch + st_unexpl + st_missing + st_badwalk + st_head;
    /* index the linked predictions */
    int need = 1;
    while (need < s_np * 2 + 16) need <<= 1;
    if (need > s_hcap) {
        s_hcap = need;
        s_hash = realloc(s_hash, (size_t)s_hcap * sizeof *s_hash);
        if (!s_hash) rt_fatal("hlr-stage: out of memory");
    }
    for (int i = 0; i < s_hcap; i++) s_hash[i] = -1;
    for (int i = 0; i < s_np; i++) {
        PPkt *p = &s_p[i];
        st_pkts++;
        if (p->slot < 0) { st_culled++; continue; }
        st_linked++; st_kind[p->kind]++;
        if (p->sub) st_alt_full++;
        u32 h = (p->addr * 2654435761u) & (u32)(s_hcap - 1);
        while (s_hash[h] >= 0) {
            if (s_p[s_hash[h]].addr == p->addr) break;      /* the same packet linked twice: a model error */
            h = (h + 1) & (u32)(s_hcap - 1);
        }
        if (s_hash[h] >= 0) { st_badwalk++; continue; }
        s_hash[h] = i;
        bool bad = false;
        for (int k = 0; k < p->nw; k++) {
            if (!(p->cmp >> k & 1)) continue;
            u32 m = r32((0x80000000u | p->addr) + 4 * (u32)k);
            const u32 mask = (p->cmplo >> k & 1) ? 0xffffu : 0xffffffffu;
            if ((m ^ p->w[k]) & mask) {
                bad = true;
                st_word_mis[k == 0 ? 0 : (k == 1 || k == 4 || k == 7 || k == 10) && p->kind >= K_GT3 ? 2 : 1]++;
                if (s_logged < 20) {
                    s_logged++;
                    rt_log("hlr-audit: frame %llu %s%s packet %06x slot %d word %d: game %08x, predicted %08x",
                           (unsigned long long)st_frames, p->sub ? "sub " : "", k_name[p->kind], p->addr, p->slot,
                           k, m, p->w[k]);
                }
            }
        }
        if (bad) { st_mismatch++; st_kind_mis[p->kind]++; }
    }
    /* walk the chains the original added */
    for (int s = 0; s < SLOTS; s++) {
        u32 head = r32(s_ot + 4 * (u32)s) & 0xffffff;
        if (head != s_ot_pred[s]) st_head++;
        u32 node = head;
        int guard = 0;
        while (node != s_ot_pre[s]) {
            if (++guard > 100000 || node == 0xffffff) { st_badwalk++; break; }
            int i = find(node);
            if (i >= 0) s_p[i].seen = 1;
            else {
                st_unexpl++;
                if (s_logged < 20) {
                    s_logged++;
                    rt_log("hlr-audit: frame %llu slot %d: unexplained packet %06x (tag %08x)",
                           (unsigned long long)st_frames, s, node, r32(0x80000000u | node));
                }
            }
            node = r32(0x80000000u | node) & 0xffffff;
        }
    }
    for (int i = 0; i < s_np; i++)
        if (s_p[i].slot >= 0 && !s_p[i].seen) {
            st_missing++;
            if (s_logged < 20) {
                s_logged++;
                rt_log("hlr-audit: frame %llu: predicted %s packet %06x slot %d not in the table",
                       (unsigned long long)st_frames, k_name[s_p[i].kind], s_p[i].addr, s_p[i].slot);
            }
        }
    if (!s_capture) return;
    if (st_mismatch + st_unexpl + st_missing + st_badwalk + st_head != bad0) { st_rejected++; return; }
    capture();
}

void hlr_stage_get_stats(HlrAuditStats *st)
{
    st->frames = st_frames; st->linked = st_linked; st->mismatch = st_mismatch; st->unexpl = st_unexpl;
    st->missing = st_missing; st->heads = st_head; st->badwalk = st_badwalk;
    st->alt_tris = st_alt_tris; st->alt_quads = st_alt_quads; st->alt_subs = st_alt_subs;
}

void hlr_stage_report(void)
{
    if (!st_frames) return;
    rt_log("hlr-audit: %llu items, %llu packets, %llu mismatched, %llu unexplained packets"
           " (%llu frames; %llu linked, %llu culled; %llu missing; %llu slot heads differ;"
           " %llu/%llu objects visible; %llu bad walks; %llu frames not captured)",
           (unsigned long long)st_objs, (unsigned long long)st_linked, (unsigned long long)st_mismatch,
           (unsigned long long)st_unexpl, (unsigned long long)st_frames, (unsigned long long)st_linked,
           (unsigned long long)st_culled, (unsigned long long)st_missing, (unsigned long long)st_head,
           (unsigned long long)st_objs_vis, (unsigned long long)st_objs, (unsigned long long)st_badwalk,
           (unsigned long long)st_rejected);
    rt_log("hlr-audit-kinds: F3 %llu/%llu F4 %llu/%llu GT3 %llu/%llu GT4 %llu/%llu (mismatched/linked);"
           " word mismatches: tag %llu, xy %llu, colour %llu",
           (unsigned long long)st_kind_mis[0], (unsigned long long)st_kind[0],
           (unsigned long long)st_kind_mis[1], (unsigned long long)st_kind[1],
           (unsigned long long)st_kind_mis[2], (unsigned long long)st_kind[2],
           (unsigned long long)st_kind_mis[3], (unsigned long long)st_kind[3],
           (unsigned long long)st_word_mis[0], (unsigned long long)st_word_mis[1],
           (unsigned long long)st_word_mis[2]);
    rt_log("hlr-audit-alt: %llu alt objects, %llu triangles, %llu quads, %llu split, %llu quarters linked"
           " (%llu sub packets audited)",
           (unsigned long long)st_alt_objs, (unsigned long long)st_alt_tris, (unsigned long long)st_alt_quads,
           (unsigned long long)st_alt_split, (unsigned long long)st_alt_subs, (unsigned long long)st_alt_full);
}
