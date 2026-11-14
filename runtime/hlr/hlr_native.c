/* Native 3D stage drawing: scheduling the passes into the GPU linked-list walk (docs/HLR_PLAN.md 2b,
 * `--hlr on`). Presentation only: reads guest RAM and the GPU's drawing state, never writes either.
 *
 * The stage is drawn with a real camera and a depth buffer (hlr_gpu.c), but everything else the
 * game puts into the same ordering table (characters, shadows, effects, the HUD) is still drawn
 * by the original packets in the PS1's painter's order, without depth. The two are interleaved
 * like this, when the walk enters a captured table (hlr.c hlr_ll_filter):
 *
 *   1. pre-walk: the rest of the linked list is read from RAM (the DMA walks the same list right
 *      after; nothing writes RAM in between) and every node gets a position k = 0, 1, 2, ... :
 *      table slots (slotpos[slot]), captured stage packets whose words still equal the capture
 *      (their native polygon's position), the background strips, and "others": everything else,
 *      with the bounding box of its GP0 commands (draw offset changes in the list followed; lines,
 *      VRAM copies / uploads and unknown commands count as the whole screen);
 *   2. every native polygon P has a position: the smallest of the positions of its packets (a
 *      polygon the PS1 subdivided: its quarters) or, if the PS1 did not link it (culled, too
 *      near, an object outside the 4:3 view), its depth's slot node. With its projected bounding
 *      box, lo(P) = the last overlapping other before P, hi(P) = the first one after it;
 *   3. P may be drawn anywhere in (lo, hi]: after every other the PS1 drew under it, before every
 *      other the PS1 drew over it. Polygons with no earlier overlap go into the pass at the table
 *      entry; the rest are covered greedily (sorted by hi, a pass at hi stabs every interval it
 *      lies in), so the number of passes is the minimum for this rule;
 *   4. at each pass point an external pass (HiresBackend.external) draws its polygons with the
 *      depth buffer of the frame (cleared by the first pass, loaded by the others): opaque ones
 *      depth-tested in walk order, then semi-transparent ones in walk order (tested, not written).
 *      The matching stage packets reach the 1x GPU only (the filter returns true for them).
 *
 * So the stage is depth-correct among itself (no painter's-order sorting errors, no gaps between
 * subdivided pieces) and a character stays in front of / behind a stage polygon exactly when the
 * PS1 drew it after / before that polygon. What this cannot express: a character the PS1 drew
 * between two stage polygons that overlap each other in depth (it would need per-pixel depth for
 * the character); such a character gets the order of the pass it falls into.
 *
 * Phase 3 (characters, `--hlr-parts` chars): the faces hlr_char.c attached to the frame are items
 * like the stage polygons (position = their packet's node, or their OT slot when the game did not
 * link them) and are drawn opaque in the same passes, writing the shared depth buffer. A face drawn
 * natively (or found invisible by the float backface / frustum test) is no longer an "other", so
 * characters no longer constrain where stage polygons go; the stage and the fighters then meet by
 * per-pixel depth, which removes the limit above for them. Shadows, effects and the HUD stay
 * others in painter's order. Two rules keep the pass count down (bracket_box): an other constrains an
 * item only if their boxes really overlap (the tile is only the index), and a semi-transparent other
 * that the PS1 sorted in between a fighter's own faces (the shadow's GT3 pieces among the feet) does
 * not constrain that fighter's faces. */
#include "hlr.h"
#include "hlr_gpu.h"
#include "../hires.h"
#include "../hires_gpu.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define INF_POS 0x7fffffff
#define TILE 32
#define STRIP_G4 0x0cc510u          /* background gradient (G4, code 0x38) */
#define STRIP_F4 0x0cc540u          /* background floor band (F4, code 0x28) */

enum { W_SLOT, W_STAGE, W_STRIP, W_OTHER, W_NONE, W_CHAR };
typedef struct { u32 node; u8 kind; s32 nat; } WNode;
typedef struct { int x0, y0, x1, y1; } IBox;      /* 1x VRAM pixels, inclusive */
typedef struct { int pos; IBox b; int chr; bool semi; } Other;   /* chr: a character packet (-1 = none); semi: a semi-transparent primitive */
typedef struct {
    int pos, lo, hi, t;
    int ilo, ihi;                   /* character face: its model's first / last linked face (walk positions) */
    IBox b;
    s32 nat;                        /* native polygon, -1 = a strip or a character face */
    int strip;                      /* W node index of the strip, -1 = none */
    s32 chr;                        /* character face (HlrFrame.chr), -1 = none */
    u8 tris;                        /* character face: its triangles that face the camera (bit 0, bit 1) */
    u32 info;                       /* layer / TEX / RAW / shade */
    int blend;                      /* -1 opaque, 0..3 semi mode */
    bool tex_semi;                  /* textured + semi: opaque texels and STP texels separately */
    bool decal;                     /* a shadow triangle: in the latest pass its interval allows, after the
                                     * opaque draw, depth biased and never writing depth */
} Item;

static int s_near = HLR_NEAR_CLIP, s_look = HLR_LOOK_ENHANCED;
static int s_diag = -1;   /* DESTREGA_HLR_DIAG: polygons still to log (-1: not read yet) */
static WNode *s_w; static int s_nw, s_wcap;
static Other *s_o; static int s_no, s_ocap;
static int *s_full; static int s_nfull, s_fcap;
static int *s_tstart, *s_tlist; static int s_tlcap, s_tcap;  /* tile grid (CSR) */
static int s_gx0, s_gy0, s_gnx, s_gny;
static Item *s_it; static int s_ni, s_icap;
static int *s_natpos; static int s_npcap;
static int *s_chrpos; static int s_cpcap;
static bool *s_chrdone; static int s_cdcap;              /* character face drawn natively (its packet suppressed) */
static int s_slotpos[HLR_OT_SLOTS];
static bool *s_sup; static int s_supcap;                     /* per position: suppress */
static int *s_pt; static int s_npt, s_ptcap;                 /* pass points (positions), INF_POS = end */
static HlrGpuPass **s_pp; static int s_ppcap;
static int s_pos, s_next;                                    /* walk position, next pass index */
static bool s_on;                                            /* a native walk is in progress */
static HlrNativeStats S;
/* depth bias of the shadow decals, world units towards the camera (DESTREGA_HLR_SHADOW_BIAS): the
 * shadow mesh is draped over the floor at its vertices only, so between them it lies up to a few
 * units under or over the native floor's triangles */
static float s_decal_bias = -1.0f;

void hlr_set_near(int m) { s_near = m == HLR_NEAR_PS1 ? HLR_NEAR_PS1 : HLR_NEAR_CLIP; }
int hlr_get_near(void) { return s_near; }
void hlr_set_look(int m) { s_look = m == HLR_LOOK_PS1 ? HLR_LOOK_PS1 : HLR_LOOK_ENHANCED; }
int hlr_get_look(void) { return s_look; }
void hlr_native_get_stats(HlrNativeStats *st) { *st = S; }

#define GROW(p, cap, need, init) do { if ((need) > (cap)) { int c_ = (cap) ? (cap) : (init); while (c_ < (need)) c_ *= 2; \
    void *q_ = realloc((p), (size_t)c_ * sizeof *(p)); if (!q_) rt_fatal("hlr-native: out of memory"); (p) = q_; (cap) = c_; } } while (0)

static inline u32 rw(u32 a) { return *(const u32 *)(g_ram + (a & 0x1ffffc)); }
static inline int sext11(u32 v) { return (int)((v & 0x7ff) ^ 0x400) - 0x400; }

/* ---- material texture: the one substitution point ----------------------------------------------
 * Every native textured polygon gets its texture here, keyed by what the PS1 would sample (page,
 * CLUT, depth, texture window). The texture cache returns a layer of decoded texels plus a
 * fingerprint of their content (hires_gpu_tc_layer): a texture replacement pack would look the
 * fingerprint up here and return its own layer instead. */
typedef struct { int depth; unsigned tpx, tpy, cx, cy; u32 tw; } MatKey;
typedef struct { MatKey k; int layer; } MatMemo;
static MatMemo s_mm[64]; static int s_nmm;

static int material_texture(const MatKey *k)
{
    for (int i = 0; i < s_nmm; i++)
        if (!memcmp(&s_mm[i].k, k, sizeof *k)) return s_mm[i].layer;
    unsigned long long content = 0;
    const int layer = hires_gpu_tc_layer(k->depth, k->tpx, k->tpy, k->cx, k->cy, k->tw, &content);
    (void)content;      /* the replacement-pack key */
    if (s_nmm < (int)(sizeof s_mm / sizeof s_mm[0])) { s_mm[s_nmm].k = *k; s_mm[s_nmm].layer = layer; s_nmm++; }
    S.lookups++;
    return layer;
}

/* ---- GP0 bounding boxes of the other packets ------------------------------------------------- */
typedef struct { int offx, offy, cx1, cy1, cx2, cy2; } Env;
enum { BB_NONE, BB_BOX, BB_FULL };

static void box_add(IBox *b, bool *any, int x, int y)
{
    if (!*any) { b->x0 = b->x1 = x; b->y0 = b->y1 = y; *any = true; return; }
    if (x < b->x0) b->x0 = x;
    if (x > b->x1) b->x1 = x;
    if (y < b->y0) b->y0 = y;
    if (y > b->y1) b->y1 = y;
}

static int gp0_bbox(u32 node, int n, Env *e, IBox *out)
{
    bool any = false;
    int i = 0;
    while (i < n) {
        const u32 w0 = rw(node + 4 + 4 * (u32)i), op = w0 >> 24;
        if (op >= 0x20 && op < 0x40) {
            const int nv = (op & 8) ? 4 : 3;
            int k = i + 1;
            for (int v = 0; v < nv; v++) {
                if ((op & 0x10) && v > 0) k++;
                if (k >= n) return BB_FULL;
                const u32 xy = rw(node + 4 + 4 * (u32)k++);
                box_add(out, &any, e->offx + sext11(xy), e->offy + sext11(xy >> 16));
                if (op & 4) k++;
            }
            i = k;
        } else if (op >= 0x60 && op < 0x80) {
            const int size = (op >> 3) & 3;
            int k = i + 1;
            if (k >= n) return BB_FULL;
            const u32 xy = rw(node + 4 + 4 * (u32)k++);
            if (op & 4) k++;
            int w = size == 1 ? 1 : size == 2 ? 8 : 16, h = w;
            if (size == 0) {
                if (k >= n) return BB_FULL;
                const u32 wh = rw(node + 4 + 4 * (u32)k++);
                w = (int)(wh & 0x3ff); h = (int)((wh >> 16) & 0x1ff);
            }
            const int x = e->offx + sext11(xy), y = e->offy + sext11(xy >> 16);
            if (w > 0 && h > 0) { box_add(out, &any, x, y); box_add(out, &any, x + w - 1, y + h - 1); }
            i = k;
        } else if (op == 0x02) {
            if (i + 2 >= n) return BB_FULL;
            const u32 xy = rw(node + 4 + 4 * (u32)(i + 1)), wh = rw(node + 4 + 4 * (u32)(i + 2));
            const int x = (int)(xy & 0x3f0), y = (int)((xy >> 16) & 0x1ff);
            const int w = (int)(((wh & 0x3ff) + 15) & ~15u), h = (int)((wh >> 16) & 0x1ff);
            if (w > 0 && h > 0) { box_add(out, &any, x, y); box_add(out, &any, x + w - 1, y + h - 1); }
            i += 3;
        } else if (op >= 0xe1 && op <= 0xe6) {
            if (op == 0xe3) { e->cx1 = (int)(w0 & 0x3ff); e->cy1 = (int)((w0 >> 10) & 0x1ff); }
            else if (op == 0xe4) { e->cx2 = (int)(w0 & 0x3ff); e->cy2 = (int)((w0 >> 10) & 0x1ff); }
            else if (op == 0xe5) { e->offx = sext11(w0); e->offy = sext11(w0 >> 11); }
            i++;
        } else if (op == 0x00 || op == 0x01 || (op >= 0x03 && op < 0x20) || op == 0xe0 || op >= 0xe7) {
            i++;    /* NOPs, cache clear */
        } else if (op >= 0xc0 && op < 0xe0) {
            i += 3; /* VRAM -> CPU: draws nothing */
        } else {
            return BB_FULL;     /* lines, VRAM copies / uploads, anything unknown */
        }
    }
    return any ? BB_BOX : BB_NONE;
}

/* ---- the tile grid of the others ---------------------------------------------------------------- */
static void grid_build(int x0, int y0, int x1, int y1)
{
    s_gx0 = x0; s_gy0 = y0;
    s_gnx = (x1 - x0 + TILE) / TILE; s_gny = (y1 - y0 + TILE) / TILE;
    const int nt = s_gnx * s_gny;
    GROW(s_tstart, s_tcap, nt + 1, 512);
    memset(s_tstart, 0, (size_t)(nt + 1) * sizeof *s_tstart);
    /* count, prefix, fill (positions in increasing order: the others are listed in walk order) */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int sum = 0;
            for (int t = 0; t <= nt; t++) { const int c = s_tstart[t]; s_tstart[t] = sum; sum += c; }
            static int *fill; static int fcap;
            GROW(fill, fcap, nt + 1, 512);
            memcpy(fill, s_tstart, (size_t)(nt + 1) * sizeof *fill);
            GROW(s_tlist, s_tlcap, sum + 1, 4096);
            for (int i = 0; i < s_no; i++) {
                const IBox *b = &s_o[i].b;
                if (b->x0 > b->x1 || b->y0 > b->y1) continue;     /* outside the pass (not counted above) */
                const int tx0 = (b->x0 - x0) / TILE, tx1 = (b->x1 - x0) / TILE, ty0 = (b->y0 - y0) / TILE, ty1 = (b->y1 - y0) / TILE;
                for (int ty = ty0; ty <= ty1; ty++)
                    for (int tx = tx0; tx <= tx1; tx++) s_tlist[fill[ty * s_gnx + tx]++] = i;   /* others in walk order */
            }
            return;
        }
        for (int i = 0; i < s_no; i++) {
            IBox *b = &s_o[i].b;
            if (b->x0 < x0) b->x0 = x0;
            if (b->y0 < y0) b->y0 = y0;
            if (b->x1 > x1) b->x1 = x1;
            if (b->y1 > y1) b->y1 = y1;
            if (b->x0 > b->x1 || b->y0 > b->y1) { b->x0 = 1; b->x1 = 0; continue; }
            const int tx0 = (b->x0 - x0) / TILE, tx1 = (b->x1 - x0) / TILE, ty0 = (b->y0 - y0) / TILE, ty1 = (b->y1 - y0) / TILE;
            for (int ty = ty0; ty <= ty1; ty++)
                for (int tx = tx0; tx <= tx1; tx++) s_tstart[ty * s_gnx + tx]++;
        }
    }
}

/* lo / hi of a position against a sorted list: the last entry < pos and the first > pos */
static void bracket(const int *l, int n, int pos, int *lo, int *hi)
{
    int a = 0, b = n;
    while (a < b) { const int m = (a + b) / 2; if (l[m] < pos) a = m + 1; else b = m; }
    if (a > 0 && l[a - 1] > *lo) *lo = l[a - 1];
    while (a < n && l[a] <= pos) a++;
    if (a < n && l[a] < *hi) *hi = l[a];
}

/* the same against a tile's list of others (indices into s_o, in walk order), counting only the
 * others whose box really overlaps b (grown by OVL_MARGIN 1x pixels: PGXP moves an other's hi-res
 * coverage by up to ~1.5 px from its integer GP0 box). The tile alone over-constrains: a shadow's
 * 5x2-pixel pieces between a fighter's faces in the OT split every face of their 32x32 tile into
 * passes of its own (Hills fight: 19.6 passes per frame instead of ~4). */
#define OVL_MARGIN 2
static bool ovl(const IBox *a, const IBox *b)
{
    return a->x0 <= b->x1 + OVL_MARGIN && b->x0 <= a->x1 + OVL_MARGIN && a->y0 <= b->y1 + OVL_MARGIN && b->y0 <= a->y1 + OVL_MARGIN;
}
/* skip: a semi-transparent other strictly inside (ilo, ihi), i.e. one the PS1 sorted in between
 * the faces of the same fighter: its shadow (fighter_shadow_render's GT3 pieces sit in the OT among
 * the feet). It does not constrain the faces: the depth-tested fighter decides, so the shadow may
 * end up under the feet where the PS1 painted a piece over them (a few pixels at the contact).
 * Effects the PS1 drew over the whole fighter (sparks, trails: after its last face) still count. */
static inline bool skip(const Other *o, const Item *it) { return o->semi && o->pos > it->ilo && o->pos < it->ihi; }
static void bracket_box(const int *l, int n, const Item *it, int *lo, int *hi)
{
    const int pos = it->pos;
    int a = 0, e = n;
    while (a < e) { const int m = (a + e) / 2; if (s_o[l[m]].pos < pos) a = m + 1; else e = m; }
    for (int i = a - 1; i >= 0 && s_o[l[i]].pos > *lo; i--)
        if (ovl(&s_o[l[i]].b, &it->b) && !skip(&s_o[l[i]], it)) { *lo = s_o[l[i]].pos; break; }
    while (a < n && s_o[l[a]].pos <= pos) a++;
    for (int i = a; i < n && s_o[l[i]].pos < *hi; i++)
        if (ovl(&s_o[l[i]].b, &it->b) && !skip(&s_o[l[i]], it)) { *hi = s_o[l[i]].pos; break; }
}

static void interval(Item *it)
{
    it->lo = -1; it->hi = INF_POS;
    bracket(s_full, s_nfull, it->pos, &it->lo, &it->hi);
    IBox b = it->b;
    const int x1 = s_gx0 + s_gnx * TILE - 1, y1 = s_gy0 + s_gny * TILE - 1;
    if (b.x0 < s_gx0) b.x0 = s_gx0;
    if (b.y0 < s_gy0) b.y0 = s_gy0;
    if (b.x1 > x1) b.x1 = x1;
    if (b.y1 > y1) b.y1 = y1;
    if (b.x0 > b.x1 || b.y0 > b.y1) return;
    for (int ty = (b.y0 - s_gy0) / TILE; ty <= (b.y1 - s_gy0) / TILE; ty++)
        for (int tx = (b.x0 - s_gx0) / TILE; tx <= (b.x1 - s_gx0) / TILE; tx++) {
            const int t = ty * s_gnx + tx;
            bracket_box(s_tlist + s_tstart[t], s_tstart[t + 1] - s_tstart[t], it, &it->lo, &it->hi);
        }
}

static int cmp_hi(const void *a, const void *b)
{
    const Item *x = *(Item *const *)a, *y = *(Item *const *)b;
    return x->hi != y->hi ? (x->hi < y->hi ? -1 : 1) : (x->pos < y->pos ? -1 : x->pos > y->pos);
}
static int cmp_int(const void *a, const void *b) { const int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
static int cmp_pos(const void *a, const void *b)
{
    const Item *x = *(Item *const *)a, *y = *(Item *const *)b;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static void put_strip(HlrGpuFrame *gf, const Env *e, u32 node, const int rect[4]);

/* ---- vertices ------------------------------------------------------------------------------------ */
static void put_nat(HlrGpuFrame *gf, const HlrFrame *f, const Item *it, int sel)
{
    const HlrNat *n = &f->nat[it->nat];
    static const int tri[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    const int ntri = n->nv == 4 ? 2 : 1;
    HlrVtx *v = hlr_gpu_frame_vtx(gf, (u32)ntri * 3);
    const bool tex = n->kind >= 2;
    u32 uv[4] = { 0 }, uvr = 0;
    if (tex) {
        unsigned umin = 255, umax = 0, vmin = 255, vmax = 0;
        for (int i = 0; i < n->nv; i++) {
            uv[i] = rw(0x80000000u | (n->pkt + 4 * (u32)(3 + 3 * i))) & 0xffff;
            const unsigned u = uv[i] & 0xff, w = uv[i] >> 8;
            if (u < umin) umin = u;
            if (u > umax) umax = u;
            if (w < vmin) vmin = w;
            if (w > vmax) vmax = w;
        }
        uvr = umin | umax << 8 | vmin << 16 | vmax << 24;
    }
    const u32 info = it->info | (u32)sel << HV_SEL_SH;
    for (int t = 0; t < ntri; t++)
        for (int k = 0; k < 3; k++, v++) {
            const int i = tri[t][k];
            v->pos[0] = n->v[i][0]; v->pos[1] = n->v[i][1]; v->pos[2] = n->v[i][2];
            v->nrm[0] = n->n[i][0] / 4096.0f; v->nrm[1] = n->n[i][1] / 4096.0f; v->nrm[2] = n->n[i][2] / 4096.0f;
            const u32 c = n->shade == HLR_SH_FLAT ? n->col[0] : n->col[i];
            v->col[0] = (u8)c; v->col[1] = (u8)(c >> 8); v->col[2] = (u8)(c >> 16); v->col[3] = 255;
            v->uv[0] = (float)(uv[i] & 0xff) + 0.5f; v->uv[1] = (float)(uv[i] >> 8) + 0.5f;
            v->uvr = uvr;
            v->info = info;
        }
}

/* a character face: world positions, the game's vertex colours (gouraud, no fog: NCCT has no depth cue) */
static void put_chr(HlrGpuFrame *gf, const HlrFrame *f, const Item *it, int sel)
{
    const HlrChr *c = &f->chr[it->chr];
    static const int tri[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    const int ntri = (it->tris & 1) + ((it->tris >> 1) & 1);
    HlrVtx *v = hlr_gpu_frame_vtx(gf, (u32)ntri * 3);
    unsigned umin = 255, umax = 0, vmin = 255, vmax = 0;
    for (int i = 0; i < c->nv; i++) {
        const unsigned u = c->uv[i] & 0xff, w = c->uv[i] >> 8;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (w < vmin) vmin = w;
        if (w > vmax) vmax = w;
    }
    const u32 uvr = umin | umax << 8 | vmin << 16 | vmax << 24;
    const u32 info = it->info | (u32)sel << HV_SEL_SH;
    for (int t = 0; t < 2; t++) {
        if (!(it->tris >> t & 1)) continue;
        for (int k = 0; k < 3; k++, v++) {
            const int i = tri[t][k];
            v->pos[0] = c->v[i][0]; v->pos[1] = c->v[i][1]; v->pos[2] = c->v[i][2];
            v->nrm[0] = v->nrm[1] = v->nrm[2] = 0.0f;
            const u32 col = c->col[i];
            v->col[0] = (u8)col; v->col[1] = (u8)(col >> 8); v->col[2] = (u8)(col >> 16); v->col[3] = 255;
            v->uv[0] = (float)(c->uv[i] & 0xff) + 0.5f; v->uv[1] = (float)(c->uv[i] >> 8) + 0.5f;
            v->uvr = uvr;
            v->info = info;
        }
    }
}

static void put_item(HlrGpuFrame *gf, const HlrFrame *f, const Env *se, const int rect[4], const Item *it, int sel)
{
    if (it->strip >= 0) put_strip(gf, se, s_w[it->strip].node, rect);
    else if (it->chr >= 0) put_chr(gf, f, it, sel);
    else put_nat(gf, f, it, sel);
}

/* a background strip, widened across the pass when it spans the 4:3 draw area */
static void put_strip(HlrGpuFrame *gf, const Env *e, u32 node, const int rect[4])
{
    const u32 w0 = rw(node + 4), op = w0 >> 24;
    const bool g = (op & 0x10) != 0;
    int x[4], y[4]; u32 c[4];
    for (int i = 0; i < 4; i++) {
        const u32 xy = rw(node + 4 + 4 * (u32)(g ? 1 + 2 * i : 1 + i));
        c[i] = g ? rw(node + 4 + 4 * (u32)(2 * i)) : w0;
        x[i] = e->offx + sext11(xy); y[i] = e->offy + sext11(xy >> 16);
    }
    int xmin = x[0], xmax = x[0];
    for (int i = 1; i < 4; i++) { if (x[i] < xmin) xmin = x[i]; if (x[i] > xmax) xmax = x[i]; }
    const bool wide = xmin <= e->cx1 && xmax >= e->cx2;    /* spans the 4:3 draw area */
    HiresGpuInfo gi;
    hires_gpu_info(&gi);
    static const int tri[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    HlrVtx *v = hlr_gpu_frame_vtx(gf, 6);
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 3; k++, v++) {
            const int i = tri[t][k];
            float px = (float)(x[i] * gi.Sx - gi.ox - rect[0]);
            if (wide && x[i] == xmin) px = 0.0f;
            if (wide && x[i] == xmax) px = (float)rect[2];
            const float qy = (float)(y[i] * gi.Sy - gi.oy - rect[1]);
            v->pos[0] = 2.0f * px / (float)rect[2] - 1.0f;
            v->pos[1] = 1.0f - 2.0f * qy / (float)rect[3];
            v->col[0] = (u8)c[i]; v->col[1] = (u8)(c[i] >> 8); v->col[2] = (u8)(c[i] >> 16); v->col[3] = 255;
            v->info = HV_SCREEN;
        }
}

/* ---- the walk ------------------------------------------------------------------------------------ */
static void emit(int k)
{
    if (k < s_npt && s_pp[k]) { g_hires->external(hlr_gpu_external, s_pp[k]); s_pp[k] = NULL; S.passes++; }
}

static void finish(void)
{
    for (; s_next < s_npt; s_next++) emit(s_next);
    s_on = false;
}

bool hlr_native_begin(HlrFrame *f, u32 entry)
{
    if (s_on) finish();
    if (!f->nn || !g_hires || !g_hires->external) return false;
    HiresGpuInfo gi;
    if (!hires_gpu_info(&gi)) return false;
    S.frames++;
    s_nmm = 0;
    /* 1. pre-walk */
    GpuDrawEnv de;
    gpu_get_draw_env(&de);
    Env e = { de.off_x, de.off_y, de.clip_x1, de.clip_y1, de.clip_x2, de.clip_y2 }, se = e;
    bool se_set = false;
    for (int i = 0; i < HLR_OT_SLOTS; i++) s_slotpos[i] = -1;
    GROW(s_natpos, s_npcap, f->nn, 4096);
    for (int i = 0; i < f->nn; i++) s_natpos[i] = -1;
    GROW(s_chrpos, s_cpcap, f->nc + 1, 1024);
    GROW(s_chrdone, s_cdcap, f->nc + 1, 1024);
    for (int i = 0; i < f->nc; i++) { s_chrpos[i] = -1; s_chrdone[i] = false; }
    s_nw = 0; s_no = 0; s_nfull = 0;
    u32 a = entry & 0x1ffffc;
    for (int guard = 0;; guard++) {
        if (guard > 0x40000) break;
        GROW(s_w, s_wcap, s_nw + 1, 8192);
        WNode *w = &s_w[s_nw];
        const u32 hdr = rw(a), n = hdr >> 24;
        w->node = a; w->kind = W_NONE; w->nat = -1;
        if (a >= f->ot && a < f->ot + 4 * HLR_OT_SLOTS) {
            const int sl = (int)((a - f->ot) / 4);
            if (s_slotpos[sl] < 0) s_slotpos[sl] = s_nw;
            w->kind = W_SLOT;
        }
        const HlrPkt *p = w->kind == W_SLOT ? NULL : hlr_frame_find(f, a);
        if (p && n == p->n && !memcmp(g_ram + a + 4, f->w + p->off, (size_t)n * 4) && p->nat >= 0) {
            w->kind = W_STAGE; w->nat = p->nat;
            if (s_natpos[p->nat] < 0) s_natpos[p->nat] = s_nw;
            if (!se_set) { se = e; se_set = true; }
        } else if (p && n == p->n && !memcmp(g_ram + a + 4, f->w + p->off, (size_t)n * 4) && p->chr >= 0) {
            w->kind = W_CHAR; w->nat = p->chr;
            if (s_chrpos[p->chr] < 0) s_chrpos[p->chr] = s_nw;
        } else if (w->kind != W_SLOT && n == 8 && a == STRIP_G4 && (rw(a + 4) >> 24) == 0x38) w->kind = W_STRIP;
        else if (w->kind != W_SLOT && n == 5 && a == STRIP_F4 && (rw(a + 4) >> 24) == 0x28) w->kind = W_STRIP;
        if ((w->kind == W_SLOT || w->kind == W_OTHER || w->kind == W_NONE) && n) {
            IBox b;
            const int r = gp0_bbox(a, (int)n, &e, &b);
            if (r != BB_NONE) {
                w->kind = W_OTHER;
                if (r == BB_FULL) { GROW(s_full, s_fcap, s_nfull + 1, 256); s_full[s_nfull++] = s_nw; }
                else { GROW(s_o, s_ocap, s_no + 1, 1024); s_o[s_no].pos = s_nw; s_o[s_no].b = b; s_o[s_no].chr = -1;
                       const u32 op0 = rw(a + 4) >> 24; s_o[s_no].semi = op0 >= 0x20 && op0 < 0x80 && (op0 & 2); s_no++; }
            }
        } else if (w->kind == W_CHAR) {
            /* an other unless the face is drawn natively (decided below, before the grid is built) */
            IBox b;
            if (gp0_bbox(a, (int)n, &e, &b) == BB_BOX) {
                GROW(s_o, s_ocap, s_no + 1, 1024); s_o[s_no].pos = s_nw; s_o[s_no].b = b; s_o[s_no].chr = w->nat; s_o[s_no].semi = false; s_no++;
            }
        }
        if (w->kind == W_STRIP && !se_set) { se = e; se_set = true; }
        s_nw++;
        if (hdr & 0x800000) break;
        a = hdr & 0x1ffffc;
    }
    if (!se_set) se = e;

    /* 2. the pass rectangle: the stage's draw area, widened by the guard band where it reaches the
     * region's edges, in 1x VRAM pixels and in target pixels */
    int x0 = se.cx1, x1 = se.cx2, y0 = se.cy1, y1 = se.cy2;
    if (x0 <= gi.rx) x0 = gi.rx - gi.gx;
    if (x1 >= gi.rx + gi.rw - 1) x1 = gi.rx + gi.rw - 1 + gi.gx;
    if (x0 < gi.rx - gi.gx) x0 = gi.rx - gi.gx;
    if (x1 > gi.rx + gi.rw - 1 + gi.gx) x1 = gi.rx + gi.rw - 1 + gi.gx;
    if (y0 < gi.ry) y0 = gi.ry;
    if (y1 > gi.ry + gi.rh - 1) y1 = gi.ry + gi.rh - 1;
    if (x0 > x1 || y0 > y1) return false;
    const int rect[4] = { x0 * gi.Sx - gi.ox, y0 * gi.Sy - gi.oy, (x1 - x0 + 1) * gi.Sx, (y1 - y0 + 1) * gi.Sy };
    const HlrCam *k = &f->cam;
    const float near = 16.0f;
    s_ni = 0;

    /* 3a. character faces (phase 3): float backface test per triangle, the frustum, the texture. A
     * face handled here is drawn natively (or found invisible) and its packet, if the game linked it,
     * is suppressed and no longer an "other"; the rest keep their packets and stay others. */
    int mlo[256], mhi[256];          /* per model: walk positions of its first / last linked face */
    for (int m = 0; m < 256; m++) { mlo[m] = INF_POS; mhi[m] = -1; }
    for (int i = 0; i < f->nc; i++)
        if (s_chrpos[i] >= 0) {
            const int m = f->chr[i].model;
            if (s_chrpos[i] < mlo[m]) mlo[m] = s_chrpos[i];
            if (s_chrpos[i] > mhi[m]) mhi[m] = s_chrpos[i];
        }
    for (int i = 0; i < f->nc; i++) {
        const HlrChr *c = &f->chr[i];
        int pos = s_chrpos[i];
        if (pos < 0) {
            if (c->linked) { S.stale++; continue; }        /* its packet changed: drawn the PS1 way */
            pos = s_slotpos[c->slot] >= 0 ? s_slotpos[c->slot] : 0;
        }
        float sx[4], sy[4];
        bool behind = false;
        IBox b = { 0, 0, -1, -1 };
        bool any = false;
        int out_l = 0, out_r = 0, out_t = 0, out_b = 0;
        for (int j = 0; j < c->nv; j++) {
            float o[3];
            hlr_cam_view(k, c->v[j], o);
            if (o[2] <= near) { behind = true; break; }
            hlr_cam_project(k, o, &sx[j], &sy[j]);
            sx[j] += (float)se.offx; sy[j] += (float)se.offy;
            out_l += sx[j] < (float)x0; out_r += sx[j] > (float)(x1 + 1); out_t += sy[j] < (float)y0; out_b += sy[j] > (float)(y1 + 1);
            const float fx = sx[j] < -4096.0f ? -4096.0f : sx[j] > 4096.0f ? 4096.0f : sx[j];
            const float fy = sy[j] < -4096.0f ? -4096.0f : sy[j] > 4096.0f ? 4096.0f : sy[j];
            box_add(&b, &any, (int)floorf(fx), (int)floorf(fy));
            box_add(&b, &any, (int)floorf(fx) + 1, (int)floorf(fy) + 1);
        }
        if (behind) { S.near_dropped++; continue; }         /* too near: the PS1's packet (if any) */
        if (out_l == c->nv || out_r == c->nv || out_t == c->nv || out_b == c->nv) { S.culled++; s_chrdone[i] = true; continue; }
        static const int tri[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
        u8 tris = 0;
        if (c->decal) tris = 1;          /* a shadow: the game links every triangle (no backface test) */
        else for (int t = 0; t < (c->nv == 4 ? 2 : 1); t++) {
            const int p0 = tri[t][0], p1 = tri[t][1], p2 = tri[t][2];
            const float area = sx[p0] * sy[p1] + sx[p1] * sy[p2] + sx[p2] * sy[p0] - sx[p0] * sy[p2] - sx[p1] * sy[p0] - sx[p2] * sy[p1];
            if (area > 0.0f) tris |= (u8)(1 << t);
        }
        if (!tris) { S.chr_back++; s_chrdone[i] = true; continue; }
        const u32 tp = c->tpage, clut = c->clut;
        MatKey key = { (int)((tp >> 7) & 3), (tp & 0xf) * 64u, ((tp >> 4) & 1) * 256u, (clut & 0x3f) * 16u, (clut >> 6) & 0x1ff, (u32)de.tw };
        if (key.depth == 3) key.depth = 2;
        const int layer = material_texture(&key);
        if (layer < 0 || layer > 0xff) { S.no_texture++; continue; }
        u32 info = (u32)HLR_SH_FLAT << HV_SHADE_SH | (u32)layer | HV_TEX | ((c->code & 1) ? HV_RAW : 0) | (c->decal ? HV_DECAL : 0);
        GROW(s_it, s_icap, s_ni + 1, 4096);
        Item *it = &s_it[s_ni++];
        memset(it, 0, sizeof *it);
        it->pos = pos; it->b = b; it->nat = -1; it->strip = -1; it->chr = i; it->tris = tris; it->info = info;
        it->ilo = mlo[c->model]; it->ihi = mhi[c->model];
        it->blend = -1;
        if (c->code & 2) { it->blend = (int)((tp >> 5) & 3); it->tex_semi = true; }
        it->decal = c->decal;
        s_chrdone[i] = true;
        if (c->decal) S.shadow_items++; else S.chr_items++;
    }
    for (int i = 0; i < s_no; i++)
        if (s_o[i].chr >= 0 && s_chrdone[s_o[i].chr]) { s_o[i].b.x0 = 1; s_o[i].b.x1 = 0; }
    grid_build(x0, y0, x1, y1);

    /* 3b. items: the native polygons this pass can draw, and the strips */
    if (s_diag < 0) { const char *d = getenv("DESTREGA_HLR_DIAG"); s_diag = d ? atoi(d) : 0; }
    for (int i = 0; i < f->nn; i++) {
        const HlrNat *n = &f->nat[i];
        if ((n->flags & HLR_NAT_NEAR) && s_near == HLR_NEAR_PS1) { S.near_dropped++; continue; }
        int pos = s_natpos[i];
        if (pos < 0) {
            if ((n->flags & HLR_NAT_LINKED)) { S.stale++; continue; }      /* its packet changed: drawn the PS1 way */
            pos = s_slotpos[n->slot] >= 0 ? s_slotpos[n->slot] : 0;
        }
        /* project: screen box (1x VRAM pixels) and frustum test */
        IBox b = { 0, 0, -1, -1 };
        bool any = false, behind = false, all_behind = true;
        int out_l = 0, out_r = 0, out_t = 0, out_b = 0;
        for (int j = 0; j < n->nv; j++) {
            const float v[3] = { n->v[j][0], n->v[j][1], n->v[j][2] };
            float o[3], sx, sy;
            hlr_cam_view(k, v, o);
            if (o[2] <= near) { behind = true; continue; }
            all_behind = false;
            hlr_cam_project(k, o, &sx, &sy);
            const float fx = (float)se.offx + sx, fy = (float)se.offy + sy;
            out_l += fx < (float)x0; out_r += fx > (float)(x1 + 1); out_t += fy < (float)y0; out_b += fy > (float)(y1 + 1);
            const int ix = (int)floorf(fx < -4096.0f ? -4096.0f : fx > 4096.0f ? 4096.0f : fx);
            const int iy = (int)floorf(fy < -4096.0f ? -4096.0f : fy > 4096.0f ? 4096.0f : fy);
            box_add(&b, &any, ix, iy);
            box_add(&b, &any, ix + 1, iy + 1);
        }
        if (all_behind) { S.culled++; continue; }
        if (!behind && (out_l == n->nv || out_r == n->nv || out_t == n->nv || out_b == n->nv)) { S.culled++; continue; }
        if (behind) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
        if (s_diag > 0 && (long)(b.x1 - b.x0) * (b.y1 - b.y0) > (long)(x1 - x0) * (y1 - y0) / 4) {
            s_diag--;   /* DESTREGA_HLR_DIAG=N: the first N polygons covering > 1/4 of the pass */
            rt_log("hlr-diag: nat %d kind %d shade %d flags %x pkt %06x slot %d pos %d behind %d box %d,%d-%d,%d",
                   i, n->kind, n->shade, n->flags, n->pkt, n->slot, pos, behind, b.x0, b.y0, b.x1, b.y1);
            for (int j = 0; j < n->nv; j++) {
                const float v[3] = { n->v[j][0], n->v[j][1], n->v[j][2] };
                float o[3];
                hlr_cam_view(k, v, o);
                rt_log("hlr-diag:   v %d %d %d -> view %.0f %.0f %.0f", n->v[j][0], n->v[j][1], n->v[j][2], o[0], o[1], o[2]);
            }
        }
        u32 info = (u32)n->shade << HV_SHADE_SH;
        const u32 code = n->shade == HLR_SH_LIT ? (n->nv == 3 ? 0x34u : 0x3cu)
                       : n->shade == HLR_SH_PRELIT ? n->col[2] >> 24 : n->col[0] >> 24;
        int blend = -1;
        bool tex_semi = false;
        if (n->kind >= 2) {
            const u32 pk = 0x80000000u | n->pkt;
            const u32 clut = rw(pk + 12) >> 16, tp = rw(pk + 24) >> 16;
            /* an object the PS1 skipped this frame (HIDDEN: outside the 4:3 view) did not write its
             * packets: their UV / CLUT / page words are what the buffer held from the last frame it was
             * drawn, or never-written memory. Use them only if the command byte is the polygon's and the
             * page is a texture page (never the framebuffer region, which the game does not texture
             * from); else leave the polygon out (Throne: a never-drawn object sampled the framebuffer
             * across the whole screen). */
            if (n->flags & HLR_NAT_HIDDEN) {
                const u32 want = n->kind == 2 ? 0x34u : 0x3cu, cb = (rw(pk + 4) >> 24) & 0xfcu;
                const u32 tx = (tp & 0xf) * 64u, ty = ((tp >> 4) & 1) * 256u;
                if (cb != want || (tx < (u32)(gi.rx + gi.rw) && ty < (u32)(gi.ry + gi.rh))) { S.hidden_bad++; continue; }
            }
            const MatKey mk = { (int)((tp >> 7) & 3), (tp & 0xf) * 64u, ((tp >> 4) & 1) * 256u,
                                (clut & 0x3f) * 16u, (clut >> 6) & 0x1ff, 0 };
            MatKey key = mk;
            key.tw = (u32)(de.tw);
            if (key.depth == 3) key.depth = 2;
            const int layer = material_texture(&key);
            if (layer < 0 || layer > 0xff) { S.no_texture++; continue; }    /* its packet (if any) is drawn the PS1 way */
            info |= (u32)layer | HV_TEX | ((code & 1) ? HV_RAW : 0);
            if (code & 2) { blend = (int)((tp >> 5) & 3); tex_semi = true; }
        } else if (code & 2) blend = 0;
        GROW(s_it, s_icap, s_ni + 1, 4096);
        Item *it = &s_it[s_ni++];
        memset(it, 0, sizeof *it);
        it->pos = pos; it->b = b; it->nat = i; it->strip = -1; it->chr = -1; it->info = info; it->blend = blend; it->tex_semi = tex_semi;
    }
    for (int i = 0; i < s_nw; i++)
        if (s_w[i].kind == W_STRIP) {
            GROW(s_it, s_icap, s_ni + 1, 4096);
            Item *it = &s_it[s_ni++];
            memset(it, 0, sizeof *it);
            it->pos = i; it->nat = -1; it->strip = i; it->chr = -1; it->blend = -1;
            it->b.x0 = x0; it->b.y0 = y0; it->b.x1 = x1; it->b.y1 = y1;
        }
    if (!s_ni) return false;

    /* 4. intervals and pass points */
    static Item **ord; static int ocap;
    GROW(ord, ocap, s_ni, 4096);
    int nr = 0;
    static int *pts; static int ptcap;
    GROW(pts, ptcap, s_ni + 1, 4096);
    int np = 0;
    bool zero = false;
    for (int i = 0; i < s_ni; i++) {
        Item *it = &s_it[i];
        interval(it);
        if (it->decal) continue;         /* placed after the others (below) */
        if (it->lo < 0) { it->t = 0; zero = true; }
        else ord[nr++] = it;
    }
    qsort(ord, (size_t)nr, sizeof *ord, cmp_hi);
    int cp = -1;
    for (int i = 0; i < nr; i++) {
        Item *it = ord[i];
        if (!(cp > it->lo)) { cp = it->hi; pts[np++] = cp; }
        it->t = cp;
    }
    if (zero) pts[np++] = 0;
    qsort(pts, (size_t)np, sizeof *pts, cmp_int);
    int u = 0;
    for (int i = 0; i < np; i++) if (!u || pts[i] != pts[u - 1]) pts[u++] = pts[i];
    np = u;
    /* decals (shadows): a pass point in (lo, hi] as late as possible (the floor under a shadow is
     * drawn before it: an earlier pass, or the same pass's stage draw) but not after the first pass
     * holding fighter faces, so the fighters are drawn after their shadows (their feet over the
     * depth-biased decal, as the PS1's order has it); a new point at hi if none */
    {
        int added = 0, minct = INT32_MAX;
        for (int i = 0; i < s_ni; i++)
            if (s_it[i].chr >= 0 && !s_it[i].decal && s_it[i].t < minct) minct = s_it[i].t;
        for (int i = 0; i < s_ni; i++) {
            Item *it = &s_it[i];
            if (!it->decal) continue;
            int t = -1;
            for (int pass = 0; pass < 2 && t < 0; pass++) {
                const int top = pass == 0 ? (it->hi < minct ? it->hi : minct) : it->hi;
                int a = 0, b = np;      /* the last point <= top */
                while (a < b) { const int m = (a + b) / 2; if (pts[m] <= top) a = m + 1; else b = m; }
                if (a > 0 && pts[a - 1] > it->lo) t = pts[a - 1];
            }
            if (t >= 0) it->t = t;
            else { it->t = it->hi; GROW(pts, ptcap, np + added + 1, 4096); pts[np + added++] = it->hi; }
        }
        if (added) {
            np += added;
            qsort(pts, (size_t)np, sizeof *pts, cmp_int);
            u = 0;
            for (int i = 0; i < np; i++) if (!u || pts[i] != pts[u - 1]) pts[u++] = pts[i];
            np = u;
        }
    }

    {   /* DESTREGA_HLR_PASS_DIAG=N: the walk nodes at the pass points of the first N frames with > 6 passes */
        static int pd = -1;
        if (pd < 0) { const char *d = getenv("DESTREGA_HLR_PASS_DIAG"); pd = d ? atoi(d) : 0; }
        if (pd > 0 && np > 6) {
            pd--;
            int nchr = 0; for (int i = 0; i < s_ni; i++) nchr += s_it[i].chr >= 0;
            rt_log("hlr-pass-diag: %d passes, %d items (%d character faces), %d others, %d full", np, s_ni, nchr, s_no, s_nfull);
            for (int i = 0; i < np; i++) {
                const int q = pts[i];
                int nc = 0, ns = 0; for (int j = 0; j < s_ni; j++) if (s_it[j].t == q) { if (s_it[j].chr >= 0) nc++; else ns++; }
                int oi = -1; for (int j = 0; j < s_no; j++) if (s_o[j].pos == q) { oi = j; break; }
                bool full = false; for (int j = 0; j < s_nfull; j++) full |= s_full[j] == q;
                const u32 nd = q < s_nw ? s_w[q].node : 0;
                const u32 hdr = nd ? *(u32 *)(g_ram + (nd & 0x1ffffc)) : 0, w0 = nd ? *(u32 *)(g_ram + ((nd + 4) & 0x1ffffc)) : 0;
                rt_log("hlr-pass-diag:   pt %d kind %d node %06x n %u op %02x%s box %d,%d-%d,%d chr %d; items %d chr + %d stage", q,
                       q < s_nw ? s_w[q].kind : -1, nd, hdr >> 24, w0 >> 24, full ? " FULL" : "",
                       oi >= 0 ? s_o[oi].b.x0 : 0, oi >= 0 ? s_o[oi].b.y0 : 0, oi >= 0 ? s_o[oi].b.x1 : 0, oi >= 0 ? s_o[oi].b.y1 : 0,
                       oi >= 0 ? s_o[oi].chr : -9, nc, ns);
            }
        }
    }

    /* 5. one HlrGpuFrame for the whole walk, one pass per point */
    HlrGpuFrame *gf = hlr_gpu_frame_new();
    {
        HlrUniforms *U = &gf->u;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            U->rt[r][c] = k->rt[r][c]; U->llm[r][c] = k->llm[r][c]; U->lcm[r][c] = k->lcm[r][c];
        }
        for (int c = 0; c < 3; c++) { U->tr[c] = k->tr[c]; U->bk[c] = k->bk[c]; U->fc[c] = k->fc[c] / 255.0f; }
        hlr_cam_clip(k, (float)se.offx, (float)se.offy, (float)gi.Sx, (float)gi.Sy, (float)gi.ox, (float)gi.oy, rect, U->proj);
        U->misc[0] = near;
        U->misc[1] = s_look == HLR_LOOK_ENHANCED ? 1.0f : 0.0f;
        U->misc[2] = gi.full_colour ? 0.0f : 1.0f;
        if (s_decal_bias < 0) { const char *e = getenv("DESTREGA_HLR_SHADOW_BIAS"); s_decal_bias = e ? (float)atof(e) : 40.0f; }
        U->misc[3] = s_decal_bias;
        U->fog[0] = k->h * 65536.0f; U->fog[1] = k->dqa; U->fog[2] = k->dqb; U->fog[3] = 1.0f;
        gf->px0 = rect[0]; gf->py0 = rect[1]; gf->pw = rect[2]; gf->ph = rect[3];
        gf->samples = hlr_gpu_msaa_for(gi.target_format);
        gf->nearest_tex = s_look == HLR_LOOK_PS1;
    }
    GROW(s_pp, s_ppcap, np, 64);
    static Item **lst; static int lcap;
    GROW(lst, lcap, s_ni, 4096);
    for (int pi = 0; pi < np; pi++) {
        int nl = 0;
        for (int i = 0; i < s_ni; i++) if (s_it[i].t == pts[pi]) lst[nl++] = &s_it[i];
        qsort(lst, (size_t)nl, sizeof *lst, cmp_pos);
        HlrGpuPass *ps = calloc(1, sizeof *ps);
        if (!ps) rt_fatal("hlr-native: out of memory");
        ps->d = calloc(3 * (size_t)nl + 3, sizeof *ps->d);   /* a decal can take two draws */
        if (!ps->d) rt_fatal("hlr-native: out of memory");
        ps->f = gf; gf->refs++;
        /* opaque: strips, opaque polygons and the opaque texels of textured semi ones; the stage
         * first, then the shadows on it, then the fighters */
        HlrDraw *d = &ps->d[ps->nd++];
        d->first = gf->nv; d->blend = -1;
        for (int i = 0; i < nl; i++) {
            const Item *it = lst[i];
            if (it->decal || it->chr >= 0) continue;
            if (it->blend < 0) put_item(gf, f, &se, rect, it, 0);
            else if (it->tex_semi) put_item(gf, f, &se, rect, it, 1);
        }
        d->count = gf->nv - d->first;
        for (int i = 0; i < nl; i++) {
            const Item *it = lst[i];
            if (!it->decal) continue;
            /* its texels without STP opaque (depth tested, not written), then the STP ones blended */
            for (int part = it->decal && (it->tex_semi || it->blend < 0) ? 0 : 1; part < 2; part++) {
                const int bl = part == 0 ? HLR_DRAW_OPAQUE_NOZ : it->blend;
                if (part == 1 && it->blend < 0) break;
                const int sel = part == 0 ? (it->tex_semi ? 1 : 0) : (it->tex_semi ? 2 : 0);
                d = &ps->d[ps->nd - 1];
                if (d->blend != bl || d->first + d->count != gf->nv) {
                    d = &ps->d[ps->nd++];
                    d->first = gf->nv; d->count = 0; d->blend = bl;
                }
                put_item(gf, f, &se, rect, it, sel);
                d->count = gf->nv - d->first;
            }
        }
        d = &ps->d[ps->nd++];
        d->first = gf->nv; d->count = 0; d->blend = -1;
        for (int i = 0; i < nl; i++) {
            const Item *it = lst[i];
            if (it->decal || it->chr < 0) continue;
            if (it->blend < 0) put_item(gf, f, &se, rect, it, 0);
            else if (it->tex_semi) put_item(gf, f, &se, rect, it, 1);
        }
        d->count = gf->nv - d->first;
        if (!d->count) ps->nd--;
        /* semi-transparent, in walk order, one draw per run of the same mode */
        for (int i = 0; i < nl; i++) {
            const Item *it = lst[i];
            if (it->strip >= 0 || it->blend < 0 || it->decal) continue;
            d = &ps->d[ps->nd - 1];
            if (d->blend != it->blend || d->first + d->count != gf->nv) {
                d = &ps->d[ps->nd++];
                d->first = gf->nv; d->count = 0; d->blend = it->blend;
            }
            put_item(gf, f, &se, rect, it, it->tex_semi ? 2 : 0);
            d->count = gf->nv - d->first;
        }
        s_pp[pi] = ps;
        S.items += (u64)nl;
    }
    hlr_gpu_frame_unref(gf);        /* the passes hold it now */
    GROW(s_pt, s_ptcap, np, 64);
    s_npt = np;
    memcpy(s_pt, pts, (size_t)np * sizeof *pts);

    /* 6. what the walk suppresses: stage packets of drawn polygons, drawn strips */
    GROW(s_sup, s_supcap, s_nw, 8192);
    static bool *drawn; static int dcap;
    GROW(drawn, dcap, f->nn, 4096);
    memset(drawn, 0, (size_t)f->nn);
    for (int i = 0; i < s_ni; i++) if (s_it[i].nat >= 0) drawn[s_it[i].nat] = true;
    for (int i = 0; i < s_nw; i++) {
        s_sup[i] = (s_w[i].kind == W_STAGE && drawn[s_w[i].nat]) || s_w[i].kind == W_STRIP ||
                   (s_w[i].kind == W_CHAR && s_chrdone[s_w[i].nat]);
        if (s_sup[i]) S.suppressed++;
    }
    s_pos = 0; s_next = 0; s_on = true;
    return true;
}

/* per node of the walk (the entry node included); returns true to keep the node off the hi-res target */
bool hlr_native_node(u32 node)
{
    if (!s_on) return false;
    node &= 0x1ffffc;
    if (s_pos >= s_nw || s_w[s_pos].node != node) {     /* the list differs from the pre-walk: give up */
        S.desync++;
        finish();
        return false;
    }
    while (s_next < s_npt && s_pt[s_next] <= s_pos) emit(s_next++);
    return s_sup[s_pos++];
}

void hlr_native_end(void) { if (s_on) finish(); }
bool hlr_native_active(void) { return s_on; }
void hlr_native_reset(void)
{
    /* passes not yet emitted belong to a dropped timeline */
    for (int i = s_next; i < s_npt; i++) if (s_pp[i]) {
        HlrGpuPass *ps = s_pp[i];
        hlr_gpu_frame_unref(ps->f);
        free(ps->d); free(ps);
        s_pp[i] = NULL;
    }
    s_on = false;
}
