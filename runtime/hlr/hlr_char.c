/* HLR character capture and audit (docs/HLR_PLAN.md phase 3; `--hlr audit|exact|on`).
 *
 * Read-only model of char_mesh_render (80036b0c, one call per fighter model and per attachment;
 * analysis/NOTES.md "Character renderer"). The wrapper in overrides/char_mesh.c calls
 * hlr_char_pre with the model block and the scene ordering table before the original runs and
 * hlr_char_post after it. Per bone part (model block +0xc, 0x70 bytes each, count at +8) the game
 *
 *   - builds the screen matrix S = WS * workm and TR' = WS * t + WS.t (MVMVA; WS = the world-screen
 *     matrix at 800ed4c8, workm / t the part's coordinate system at *(part) + 0x24 / + 0x38),
 *   - projects the part's vertices (part +0x64, count s16 +0x6c) with RTPT / RTPS into the vertex
 *     buffer 800c9d38 ({SXY, SZ} per vertex, all parts one after the other),
 *   - lights the part's normals (+0x68, count +0x6e) with NCCT / NCCS, LLM = part +0x24, LCM = part
 *     +0x44 (both from bone_light_matrices 800338d8), RGBC = 800bfe28, BK as the GTE holds it,
 *     into the colour buffer 800cb7c8 (one word per normal),
 *
 * then appends the "chain" vertices (char_mesh_chains 800367d8: hair / cloth points simulated in
 * world coordinates and written back to RAM) to the vertex buffer, and fills and links the faces
 * (model block +0x10: groups of GT3 lists, type 0x1a, 80036138, and GT4 lists, type 0x1b,
 * 800363c8; overrides/char_mesh.c has the record layouts and the integer backface rule).
 *
 * Audit (every mode but off): a host GteState (the guest GTE is never touched) reproduces the
 * vertex buffer word for word from the model in RAM, the colour buffer from the normals and the
 * lights, and the set of faces the game linked: a face is in its slot's chain exactly when its
 * integer NCLIP passes, with the predicted colour / XY words. A clean call becomes character faces
 * of the stage's frame for the same table (hlr.c HlrFrame.chr) when the HLR draws natively: world
 * positions (workm * v / 4096 + t, in float, through the bone filter below), the game's colours,
 * the packet's texture words. hlr_native.c draws them with the stage, in its depth buffer.
 *
 * Bone filter (--hlr-bones smooth, the default; presentation only): the jitter of a fighter's face
 * comes from the animation itself (NOTES.md "Floor tile seams and face shimmer"), so
 * every part's workm and translation (part 0's in the world, the others relative to part 0: the
 * root's bob is most of the jitter) go through Holt's linear smoothing (level gain 0.3, trend gain 0.15): the level follows a
 * steady motion with no lag (a plain moving average trails by (1 - a) / a updates; test_hlr checks
 * < 0.01 unit on a ramp) while jitter around it is cut to about a quarter (test_hlr). A large
 * change (a fast attack, a new animation: matrix elements moving > 0.03..0.20 or the relative
 * translation > 6..40 units in one update) raises both gains towards 1, so real motion is never
 * delayed or overshot. Milena repro (frames 3880-3950, every vertex of a fighter):
 * second difference of the projected positions 0.193 px per update with the game's bones, 0.045
 * filtered (DESTREGA_HLR_BONE_DIAG=3880-3950 prints it). When none of a model's parts changed since
 * its last update (pause, hit-stop, any frozen frame) every part snaps to its input and its trend
 * is zeroed: the filter would otherwise coast on its trend and settle over several updates, so
 * the fighters visibly kept moving after the game froze. The filter state is dropped on a save-state load, a mode change and when a model was not drawn
 * for a few updates; it never feeds back into emulated state. */
#include "psx.h"
#include "hlr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WSMATRIX        0x800ed4c8u
#define VBUF            0x800c9d38u
#define VBUF_MAX        ((0x800cb7c8u - VBUF) / 8)     /* the colour buffer follows the vertex buffer */
#define CBUF            0x800cb7c8u
#define CBUF_MAX        2048
#define CHAR_RGBC       0x800bfe28u
#define SCR_IDX         0x1f800000u
#define SCR_FAR         0x1f800008u
#define SCR_NEAR        0x1f80000cu
#define SCR_MID         0x1f800010u
#define MAX_PARTS       64
#define SLOTS           HLR_OT_SLOTS

enum { OP_RTPS = 0x4a180001, OP_RTPT = 0x4a280030, OP_NCCT = 0x4b18043f, OP_NCCS = 0x4b08041b,
       OP_MV_IR = 0x4a49e012, OP_MV_TR = 0x4a480012 };

static inline u32 r32(u32 a) { return *(u32 *)(g_ram + (a & 0x1ffffc)); }
static inline s32 r16s(u32 a) { return (s16)*(u16 *)(g_ram + (a & 0x1ffffe)); }
static inline u32 r16(u32 a) { return *(u16 *)(g_ram + (a & 0x1ffffe)); }
static inline u32 scr(u32 a) { return *(u32 *)(g_scratch + (a & 0x3fc)); }
static inline bool in_ram(u32 a) { return (a & 0x1fffffff) < 0x200000; }

static int s_bones = HLR_BONES_SMOOTH;
static HlrCharStats S;
static GteState G0, G;
static u32 s_mb, s_ot;
static bool s_run, s_draw;
static int s_model_in_update;
static u64 s_last_seq;
static bool s_diag;
static u32 s_diag_lo, s_diag_hi = ~0u;

void hlr_set_bones(int m) { s_bones = m == HLR_BONES_GAME ? HLR_BONES_GAME : HLR_BONES_SMOOTH; hlr_char_reset(); }
int hlr_get_bones(void) { return s_bones; }
void hlr_char_get_stats(HlrCharStats *st) { *st = S; }

/* ---- bone filter ------------------------------------------------------------------------ */
static float ramp(float d, float lo, float hi) { return d <= lo ? 0.0f : d >= hi ? 1.0f : (d - lo) / (hi - lo); }

#define ALPHA     0.3f         /* level gain */
#define BETA      0.15f        /* trend gain */
#define SNAP_M_LO 0.03f     /* matrix elements (1.0 = 4096): change per update where smoothing gives way */
#define SNAP_M_HI 0.20f
#define SNAP_T_LO 6.0f      /* translation (part 0: world; others: relative to part 0), units per update */
#define SNAP_T_HI 40.0f

void hlr_bone_filter(HlrBoneFilter *bf, const float x[12], float out[12])
{
    if (!bf->n) {
        memcpy(bf->s, x, sizeof bf->s); memset(bf->b, 0, sizeof bf->b);
        memcpy(out, x, 12 * sizeof *out);
        bf->n = 1;
        return;
    }
    float dm = 0.0f, dt = 0.0f;
    for (int i = 0; i < 9; i++) { const float d = fabsf(x[i] - bf->s[i]); if (d > dm) dm = d; }
    for (int i = 9; i < 12; i++) dt += (x[i] - bf->s[i]) * (x[i] - bf->s[i]);
    dt = sqrtf(dt);
    const float rm = ramp(dm, SNAP_M_LO, SNAP_M_HI), rt = ramp(dt, SNAP_T_LO, SNAP_T_HI), r = rm > rt ? rm : rt;
    if (r >= 1.0f) S.snaps++;
    /* Holt's linear smoothing: level s and trend b (bf->b holds the trend). The level of a
     * steady motion has no lag (a plain average would trail it by (1 - a) / a updates); a large
     * change raises both gains to 1, so the output is the input and the trend restarts from it. */
    const float a = ALPHA + (1.0f - ALPHA) * r, be = BETA + (1.0f - BETA) * r;
    for (int i = 0; i < 12; i++) {
        const float s0 = bf->s[i];
        bf->s[i] = a * x[i] + (1.0f - a) * (s0 + bf->b[i]);
        bf->b[i] = be * (bf->s[i] - s0) + (1.0f - be) * bf->b[i];
        if (r >= 1.0f) bf->b[i] = 0.0f;
        out[i] = bf->s[i];
    }
    memcpy(bf->x, x, sizeof bf->x);
    bf->n++;
}

void hlr_bone_filter_hold(HlrBoneFilter *bf, const float x[12], float out[12])
{
    memcpy(bf->s, x, sizeof bf->s); memset(bf->b, 0, sizeof bf->b); memcpy(bf->x, x, sizeof bf->x);
    memcpy(out, x, 12 * sizeof *out);
    bf->n++;
    S.holds++;
}

/* the filter input of one part: workm (1.0 = 4096) and the translation (part 0: world; the others
 * relative to part 0's, t0) */
static void part_input(u32 coord, const float t0[3], int p, float x[12], float t[3])
{
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) x[3 * r + c] = (float)r16s(coord + 36 + 6 * (u32)r + 2 * (u32)c) / 4096.0f;
    t[0] = (float)r16s(coord + 56); t[1] = (float)r16s(coord + 60); t[2] = (float)r16s(coord + 64);
    for (int c = 0; c < 3; c++) x[9 + c] = t[c] - (p ? t0[c] : 0.0f);
}

typedef struct {
    u32 mb;             /* model block (0 = unused) */
    int nparts;
    u64 seq;            /* stage frame of its last update */
    HlrBoneFilter f[MAX_PARTS];
    float out[MAX_PARTS][12];
} Model;
static Model s_models[8];

void hlr_char_reset(void)
{
    for (size_t i = 0; i < sizeof s_models / sizeof s_models[0]; i++) s_models[i].mb = 0;
    S.resets++;
}

static Model *model_for(u32 mb, int nparts, u64 seq)
{
    const int nm = (int)(sizeof s_models / sizeof s_models[0]);
    Model *m = NULL;
    for (int i = 0; i < nm; i++) if (s_models[i].mb == mb) m = &s_models[i];
    if (m && (m->nparts != nparts || seq - m->seq > 3)) { m->mb = 0; m = NULL; }  /* another model, or not drawn for a while */
    if (!m) {
        for (int i = 0; i < nm && !m; i++) if (!s_models[i].mb) m = &s_models[i];
        if (!m) { m = &s_models[0]; for (int i = 1; i < nm; i++) if (s_models[i].seq < m->seq) m = &s_models[i]; }
        memset(m, 0, sizeof *m);
        m->mb = mb; m->nparts = nparts;
    }
    return m;
}

/* ---- host GTE helpers --------------------------------------------------------------------- */
#define CMD(x)    gte_exec_on(&G, (x))
#define DW(r, v)  gte_write_data_on(&G, (r), (v))
#define DR(r)     gte_read_data_on(&G, (r))
#define CW(r, v)  gte_write_ctrl_on(&G, (r), (v))

static u32 ot_slot(u32 z)
{
    const u32 near = scr(SCR_NEAR), mid = scr(SCR_MID), far = scr(SCR_FAR);
    if (z < near) return (z * 0x155u) / (near + 1);
    if (z < mid) return ((z - near) * 0x400u) / (mid - (near - 1)) + 0x155;
    if (z < far) return ((z - mid) * 0x2abu) / (far - (mid - 1)) + 0x555;
    return 0x7ff;
}

static s32 nclip(u32 a, u32 b, u32 c)
{
    const s64 x0 = (s16)(a & 0xffff), y0 = (s16)(a >> 16), x1 = (s16)(b & 0xffff), y1 = (s16)(b >> 16);
    const s64 x2 = (s16)(c & 0xffff), y2 = (s16)(c >> 16);
    return (s32)(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
}

/* is pkt in the chain of ordering-table slot `slot`? */
static bool linked_at(u32 slot, u32 pkt)
{
    const u32 ot = s_ot;
    u32 a = r32(ot + 4 * slot) & 0x1ffffc;
    for (int guard = 0; guard < 65536; guard++) {
        if (a >= ot && a < ot + 4 * SLOTS) return false;   /* the next slot */
        if (a == pkt) return true;
        const u32 h = r32(a);
        if ((h & 0xffffff) == 0xffffff) return false;
        a = h & 0x1ffffc;
    }
    return false;
}

/* ---- the hooks ------------------------------------------------------------------------------ */
void hlr_char_pre(u32 mb, u32 ot)
{
    s_run = false;
    if (hlr_get_mode() == HLR_OFF) return;
    s_draw = hlr_chars_native();
    const char *dg = getenv("DESTREGA_HLR_BONE_DIAG");    /* "1" or "LO-HI" (libetc vblank count) */
    s_diag = dg != NULL;
    if (dg && strchr(dg, '-')) { s_diag_lo = (u32)strtoul(dg, NULL, 10); s_diag_hi = (u32)strtoul(strchr(dg, '-') + 1, NULL, 10); }
    gte_save(&G0);
    s_mb = mb; s_ot = ot & 0x1ffffc;
    s_run = true;
}

/* scratch of one call */
static u32 s_vb[VBUF_MAX][2];           /* predicted {SXY, SZ} */
static float s_wp[VBUF_MAX][3];         /* world positions (bone filter applied) */
static float s_wg[VBUF_MAX][3];         /* world positions with the game's matrices (projection check) */

void hlr_char_post(void)
{
    if (!s_run) return;
    s_run = false;
    S.calls++;
    const u32 mb = s_mb;
    if (!in_ram(mb)) { S.bad_model++; return; }
    const int nparts = (int)r32(mb + 8);
    const u32 parts = r32(mb + 12);
    if (nparts < 0 || nparts > MAX_PARTS || (nparts && !in_ram(parts))) { S.bad_model++; return; }

    HlrFrame *f = s_draw ? hlr_frame_for_ot(s_ot) : NULL;
    if (s_draw && !f) S.no_frame++;
    if (f && f->seq != s_last_seq) { s_last_seq = f->seq; s_model_in_update = 0; }

    /* world-screen matrix as the renderer loads it */
    u32 ws[8];
    for (int i = 0; i < 8; i++) ws[i] = r32(WSMATRIX + 4 * (u32)i);
    G = G0;
    bool bad = false;

    /* 1. parts: matrices, vertices, colours */
    Model *mdl = f && s_bones == HLR_BONES_SMOOTH ? model_for(mb, nparts, f->seq) : NULL;
    const bool step = mdl && mdl->seq != f->seq;
    float t0[3] = { 0, 0, 0 }, t0f[3] = { 0, 0, 0 };   /* part 0's translation: raw, filtered */
    /* frozen: every part's input equals its input at the model's last update */
    bool frozen = step;
    for (int p = 0; p < nparts && frozen; p++) {
        const u32 coord = r32(parts + 0x70u * (u32)p);
        float x[12], t[3];
        if (!in_ram(coord) || !mdl->f[p].n) { frozen = false; break; }
        part_input(coord, t0, p, x, t);
        if (p == 0) memcpy(t0, t, sizeof t0);
        if (memcmp(x, mdl->f[p].x, sizeof x)) frozen = false;
    }
    int nv = 0, nc = 0;
    const u32 rgbc = r32(CHAR_RGBC);
    for (int p = 0; p < nparts && !bad; p++) {
        const u32 part = parts + 0x70u * (u32)p, coord = r32(part);
        const u32 vp = r32(part + 0x64), np_ = r32(part + 0x68);
        const int vcount = r16s(part + 0x6c), ncount = r16s(part + 0x6e);
        if (!in_ram(coord) || vcount < 0 || ncount < 0 || nv + vcount > (int)VBUF_MAX || nc + ncount > CBUF_MAX ||
            (vcount && !in_ram(vp)) || (ncount && !in_ram(np_))) { bad = true; S.bad_model++; break; }
        /* S = WS * workm (column by column), TR' = WS * t + WS.t */
        for (int i = 0; i < 5; i++) CW(i, ws[i]);
        s16 sm[9];
        for (int j = 0; j < 3; j++) {
            DW(9, r16(coord + 36 + 2 * (u32)j)); DW(10, r16(coord + 42 + 2 * (u32)j)); DW(11, r16(coord + 48 + 2 * (u32)j));
            CMD(OP_MV_IR);
            sm[j] = (s16)DR(9); sm[3 + j] = (s16)DR(10); sm[6 + j] = (s16)DR(11);
        }
        for (int i = 5; i < 8; i++) CW(i, ws[i]);
        DW(0, r16(coord + 56) | r16(coord + 60) << 16);
        DW(1, r32(coord + 64));
        CMD(OP_MV_TR);
        const u32 trp[3] = { DR(25), DR(26), DR(27) };
        CW(0, (u16)sm[0] | (u32)(u16)sm[1] << 16); CW(1, (u16)sm[2] | (u32)(u16)sm[3] << 16);
        CW(2, (u16)sm[4] | (u32)(u16)sm[5] << 16); CW(3, (u16)sm[6] | (u32)(u16)sm[7] << 16); CW(4, (u16)sm[8]);
        CW(5, trp[0]); CW(6, trp[1]); CW(7, trp[2]);
        int i = 0;
        for (; i + 3 <= vcount; i += 3) {
            for (int k = 0; k < 6; k++) DW(k, r32(vp + 8 * (u32)i + 4 * (u32)k));
            CMD(OP_RTPT);
            for (int k = 0; k < 3; k++) { s_vb[nv + i + k][0] = DR(12 + k); s_vb[nv + i + k][1] = DR(17 + k); }
        }
        for (; i < vcount; i++) {
            DW(0, r32(vp + 8 * (u32)i)); DW(1, r32(vp + 8 * (u32)i + 4));
            CMD(OP_RTPS);
            s_vb[nv + i][0] = DR(14); s_vb[nv + i][1] = DR(19);
        }
        /* lighting */
        for (int k = 0; k < 5; k++) { CW(8 + k, r32(part + 36 + 4 * (u32)k)); CW(16 + k, r32(part + 68 + 4 * (u32)k)); }
        DW(6, rgbc);
        i = 0;
        for (; i + 3 <= ncount; i += 3) {
            for (int k = 0; k < 6; k++) DW(k, r32(np_ + 8 * (u32)i + 4 * (u32)k));
            CMD(OP_NCCT);
            for (int k = 0; k < 3; k++) {
                S.col++;
                if (DR(20 + k) != r32(CBUF + 4 * (u32)(nc + i + k))) { S.col_bad++; bad = true; }
            }
        }
        for (; i < ncount; i++) {
            DW(0, r32(np_ + 8 * (u32)i)); DW(1, r32(np_ + 8 * (u32)i + 4));
            CMD(OP_NCCS);
            S.col++;
            if (DR(22) != r32(CBUF + 4 * (u32)(nc + i))) { S.col_bad++; bad = true; }
        }
        /* world positions: workm * v / 4096 + t, through the bone filter */
        float x[12], o[12], t[3];
        part_input(coord, t0, p, x, t);
        if (p == 0) memcpy(t0, t, sizeof t0);
        if (mdl) {
            if (step) {
                if (frozen) hlr_bone_filter_hold(&mdl->f[p], x, o); else hlr_bone_filter(&mdl->f[p], x, o);
                memcpy(mdl->out[p], o, sizeof o);
            }
            else memcpy(o, mdl->out[p], sizeof o);
            if (p == 0) memcpy(t0f, o + 9, sizeof t0f);
            else for (int c = 0; c < 3; c++) o[9 + c] += t0f[c];
        } else {
            memcpy(o, x, sizeof o);
            for (int c = 0; c < 3; c++) o[9 + c] = t[c];
        }
        for (int k = 0; k < vcount; k++) {
            const float v[3] = { (float)r16s(vp + 8 * (u32)k), (float)r16s(vp + 8 * (u32)k + 2), (float)r16s(vp + 8 * (u32)k + 4) };
            for (int r = 0; r < 3; r++) {
                s_wp[nv + k][r] = o[3 * r] * v[0] + o[3 * r + 1] * v[1] + o[3 * r + 2] * v[2] + o[9 + r];
                s_wg[nv + k][r] = x[3 * r] * v[0] + x[3 * r + 1] * v[1] + x[3 * r + 2] * v[2] + t[r];
            }
        }
        nv += vcount; nc += ncount;
    }
    if (mdl && step) mdl->seq = f->seq;

    /* 2. chains: world points in RAM, projected with WS alone; moved along with their anchor's filter */
    const u32 ch = r32(mb + 20);
    if (!bad && ch) {
        if (!in_ram(ch)) { bad = true; S.bad_model++; }
        const int cp = bad ? 0 : (int)r32(ch), nch = bad ? 0 : (int)r32(ch + 4);
        if (!bad && (cp < 0 || cp >= nparts || nch < 0 || nch > 64)) { bad = true; S.bad_model++; }
        for (int i = 0; i < 8; i++) CW(i, ws[i]);
        u32 s = ch + 8;
        int base = 0;
        for (int p = 0; p < cp && !bad; p++) base += r16s(parts + 0x70u * (u32)p + 0x6c);
        for (int c = 0; c < nch && !bad; c++) {
            const u32 w = r32(s); s += 4;
            const int anchor = (s32)w >> 16, cnt = (int)(w & 0xffff);
            const int pv = r16s(parts + 0x70u * (u32)cp + 0x6c);
            if (anchor < 0 || anchor >= pv || nv + cnt > (int)VBUF_MAX) { bad = true; S.bad_model++; break; }
            float d[3];
            for (int r = 0; r < 3; r++) d[r] = s_wp[base + anchor][r] - s_wg[base + anchor][r];
            for (int k = 0; k < cnt; k++, s += 8) {
                DW(0, r32(s)); DW(1, r32(s + 4));
                CMD(OP_RTPS);
                s_vb[nv][0] = DR(14); s_vb[nv][1] = DR(19);
                const float v[3] = { (float)r16s(s), (float)r16s(s + 2), (float)r16s(s + 4) };
                for (int r = 0; r < 3; r++) { s_wg[nv][r] = v[r]; s_wp[nv][r] = v[r] + d[r]; }
                nv++;
            }
        }
    }
    if (bad) { S.frames_bad++; return; }

    /* 3. the vertex buffer against the prediction; the float projection of the game's bones */
    for (int i = 0; i < 8; i++) CW(i, ws[i]);
    HlrCam cam;
    hlr_cam_from_gte(&cam, &G);
    for (int i = 0; i < nv; i++) {
        S.vtx++;
        if (s_vb[i][0] != r32(VBUF + 8 * (u32)i) || s_vb[i][1] != r32(VBUF + 8 * (u32)i + 4)) { S.vtx_bad++; bad = true; }
        float o[3], sx, sy;
        hlr_cam_view(&cam, s_wg[i], o);
        const int ix = (s16)(s_vb[i][0] & 0xffff), iy = (s16)(s_vb[i][0] >> 16);
        if (ix > -1000 && ix < 1000 && iy > -1000 && iy < 1000 && hlr_cam_project(&cam, o, &sx, &sy)) {
            const double e = hypot(sx - ix, sy - iy);
            S.proj_err_sum += e; S.proj_n++;
            if (e > S.proj_err_max) S.proj_err_max = e;
        }
    }
    S.audited++;
    const u32 vbc = r32(0x800bf2b4u);   /* libetc's vblank count: only to pick the diagnostic's range */
    if (s_diag && mdl && step && s_model_in_update == 0 && vbc >= s_diag_lo && vbc <= s_diag_hi) {
        /* DESTREGA_HLR_BONE_DIAG: the first model of each update, every vertex projected with the
         * game's bones and with the filtered ones: step per update and second difference (jitter) */
        static float hist[2][3][VBUF_MAX][2];
        static int hn, hnv;
        if (nv != hnv) { hn = 0; hnv = nv; }
        for (int w = 0; w < 2; w++) {
            memmove(hist[w][0], hist[w][1], sizeof hist[w][0] * 2);
            for (int i = 0; i < nv; i++) {
                float o[3], sx = 0, sy = 0;
                hlr_cam_view(&cam, w ? s_wp[i] : s_wg[i], o);
                hlr_cam_project(&cam, o, &sx, &sy);
                hist[w][2][i][0] = sx; hist[w][2][i][1] = sy;
            }
        }
        if (!S.dg_vb0) S.dg_vb0 = vbc;
        S.dg_vb1 = vbc;
        if (++hn >= 3)
            for (int w = 0; w < 2; w++)
                for (int i = 0; i < nv; i++) {
                    const float *a = hist[w][0][i], *b = hist[w][1][i], *c = hist[w][2][i];
                    S.dg_step[w] += hypot(c[0] - b[0], c[1] - b[1]);
                    S.dg_jit[w] += hypot(b[0] - 0.5 * (a[0] + c[0]), b[1] - 0.5 * (a[1] + c[1]));
                    if (!w) S.dg_n++;
                }
    }

    /* 4. faces: kept by the integer rule <=> linked at the predicted slot, with the predicted words */
    const u32 grp = r32(mb + 16);
    if (!in_ram(grp)) { S.frames_bad++; return; }
    const bool second = scr(SCR_IDX) != 0;
    const int ng = (int)r32(grp);
    u32 g = grp + 4;
    const int first_chr = f ? f->nc : 0, first_pkt = f ? f->np : 0;
    for (int gi = 0; gi < ng && gi < 256 && !bad; gi++) {
        const int n = (int)r32(g);
        if (n < 0 || n > 4096) { bad = true; break; }
        for (int e = 0; e < n && !bad; e++) {
            const u32 h = r32(g + 4 + 4 * (u32)e);
            if (!in_ram(h)) { bad = true; break; }
            const int cnt = (int)r16(h), type = (int)(r32(h) >> 16 & 0xff);
            if (type != 0x1a && type != 0x1b) continue;
            const bool quad = type == 0x1b;
            const u32 rsz = quad ? 0x78 : 0x5c, half = second ? (quad ? 0x34 : 0x28) : 0;
            for (int k = 0; k < cnt && !bad; k++) {
                const u32 rec = h + 4 + rsz * (u32)k, pkt = rec + half;
                int vi[4], ci[4];
                if (quad) {
                    const u32 a = r32(rec + 0x68), b = r32(rec + 0x6c), c0 = r32(rec + 0x70), c1 = r32(rec + 0x74);
                    vi[0] = (s16)a; vi[1] = (s16)(a >> 16); vi[2] = (s16)b; vi[3] = (s16)(b >> 16);
                    ci[0] = (s16)c0; ci[1] = (s16)(c0 >> 16); ci[2] = (s16)c1; ci[3] = (s16)(c1 >> 16);
                } else {      /* packet order A, C, B */
                    const u32 a = r32(rec + 0x50), b = r32(rec + 0x54), c = r32(rec + 0x58);
                    vi[0] = (s16)a; vi[1] = (s16)(a >> 16); vi[2] = (s16)b;
                    ci[0] = (s16)(b >> 16); ci[1] = (s16)c; ci[2] = (s16)(c >> 16);
                }
                const int nvx = quad ? 4 : 3;
                for (int j = 0; j < nvx; j++)
                    if (vi[j] < 0 || vi[j] >= nv || ci[j] < 0 || ci[j] >= nc) { bad = true; S.bad_model++; }
                if (bad) break;
                u32 sxy[4], col[4];
                for (int j = 0; j < nvx; j++) { sxy[j] = s_vb[vi[j]][0]; col[j] = r32(CBUF + 4 * (u32)ci[j]); }
                bool kept;
                u32 slot;
                if (quad) {
                    kept = nclip(sxy[3], sxy[1], sxy[2]) < 0 || nclip(sxy[0], sxy[1], sxy[2]) > 0;
                    s32 z = (s32)s_vb[vi[1]][1];
                    if ((s32)s_vb[vi[2]][1] < z) z = (s32)s_vb[vi[2]][1];
                    if ((s32)s_vb[vi[3]][1] < z) z = (s32)s_vb[vi[3]][1];
                    if ((s32)s_vb[vi[0]][1] < z) z = (s32)s_vb[vi[0]][1];
                    slot = ot_slot((u32)z);
                } else {
                    kept = nclip(sxy[0], sxy[1], sxy[2]) > 0;
                    slot = ot_slot((s_vb[vi[0]][1] + s_vb[vi[1]][1] + s_vb[vi[2]][1]) / 3);
                }
                S.faces++;
                const u32 w0 = quad ? col[0] : (col[0] & 0xffffff) | 0x34000000u;
                const bool linked = linked_at(slot & 0x7ff, pkt & 0x1ffffc);
                if (kept) S.kept++;
                if (linked != kept) { S.link_bad++; bad = true; break; }
                u32 words[12];
                if (kept) {
                    if ((r32(pkt) >> 24) != (u32)(quad ? 12 : 9)) { S.word_bad++; bad = true; break; }
                    for (int j = 0; j < nvx; j++) {
                        const u32 cw = j ? col[j] : w0;
                        if (r32(pkt + 4 + 12 * (u32)j) != cw || r32(pkt + 8 + 12 * (u32)j) != sxy[j]) { S.word_bad++; bad = true; }
                    }
                    if (bad) break;
                    for (int j = 0; j < 3 * nvx; j++) words[j] = r32(pkt + 4 + 4 * (u32)j);
                }
                if (!f) continue;
                HlrChr *hc = hlr_frame_add_chr(f);
                for (int j = 0; j < nvx; j++) {
                    memcpy(hc->v[j], s_wp[vi[j]], sizeof hc->v[j]);
                    hc->key[j] = 1ull << 62 | (u64)(mb & 0x1fffff) << 16 | (u64)(u16)vi[j];
                    hc->col[j] = col[j] & 0xffffff;
                    hc->uv[j] = (u16)r32(pkt + 12 + 12 * (u32)j);
                }
                hc->col[0] |= w0 & 0xff000000u;
                hc->clut = (u16)(r32(pkt + 12) >> 16);
                hc->tpage = (u16)(r32(pkt + 24) >> 16);
                hc->pkt = pkt & 0x1ffffc;
                hc->nv = (u8)nvx; hc->code = (u8)(w0 >> 24); hc->linked = kept;
                hc->model = (u8)s_model_in_update;
                hc->slot = (s16)(slot & 0x7ff);
                if (kept) {
                    HlrPkt *hp = hlr_frame_add(f, pkt, quad ? 3 : 2, words, 3 * nvx);
                    hp->chr = f->nc - 1;
                }
            }
        }
        g += 4 * (u32)n + 4;
    }
    if (bad) {
        S.frames_bad++;
        if (f) { f->nc = first_chr; f->np = first_pkt; }     /* nothing of this call is drawn natively */
        return;
    }
    if (f) {
        /* the stage's camera must be the one the characters were projected with */
        bool same = true;
        HlrCam k = cam;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) same &= k.rt[r][c] == f->cam.rt[r][c];
            same &= k.tr[r] == f->cam.tr[r];
        }
        same &= k.h == f->cam.h && k.ofx == f->cam.ofx && k.ofy == f->cam.ofy;
        if (!same) { S.cam_diff++; f->nc = first_chr; f->np = first_pkt; return; }
        hlr_frame_reindex(f);
        S.attached++;
        s_model_in_update++;
        /* motion: every vertex of the model, also those no face of this update uses (a face that turns
         * towards the camera in the next update needs its vertices' positions here; HlrChr.key) */
        if (hlr_get_motion())
            for (int i = 0; i < nv; i++) hlr_frame_add_kpos(f, 1ull << 62 | (u64)(mb & 0x1fffff) << 16 | (u64)(u16)i, s_wp[i]);
    }
}

void hlr_char_report(void)
{
    if (!S.calls) return;
    rt_log("hlr-chars: %llu models drawn by the game, %llu audited; vertices %llu, %llu mismatched; colours %llu,"
           " %llu mismatched; faces %llu (%llu kept by the game), %llu linked differently, %llu with other words;"
           " %llu bad models, %llu rejected; float projection of the game's bones vs SXY: mean %.3f px, max %.2f px",
           (unsigned long long)S.calls, (unsigned long long)S.audited, (unsigned long long)S.vtx,
           (unsigned long long)S.vtx_bad, (unsigned long long)S.col, (unsigned long long)S.col_bad,
           (unsigned long long)S.faces, (unsigned long long)S.kept, (unsigned long long)S.link_bad,
           (unsigned long long)S.word_bad, (unsigned long long)S.bad_model, (unsigned long long)S.frames_bad,
           S.proj_n ? S.proj_err_sum / (double)S.proj_n : 0.0, S.proj_err_max);
    if (hlr_get_mode() == HLR_ON)
        rt_log("hlr-chars: %llu models attached to a native frame, %llu without a frame, %llu with another camera;"
               " bones %s (%llu filter snaps, %llu part updates held while frozen, %llu resets)",
               (unsigned long long)S.attached, (unsigned long long)S.no_frame, (unsigned long long)S.cam_diff,
               s_bones == HLR_BONES_GAME ? "game" : "smooth", (unsigned long long)S.snaps, (unsigned long long)S.holds,
               (unsigned long long)S.resets);
    if (S.dg_n)
        rt_log("hlr-bones-diag: %llu vertex updates; step per update game %.3f px, filtered %.3f px;"
               " jitter (second difference) game %.3f px, filtered %.3f px (vblank count %u..%u)", (unsigned long long)S.dg_n,
               S.dg_step[0] / (double)S.dg_n, S.dg_step[1] / (double)S.dg_n, S.dg_jit[0] / (double)S.dg_n,
               S.dg_jit[1] / (double)S.dg_n, S.dg_vb0, S.dg_vb1);
}
