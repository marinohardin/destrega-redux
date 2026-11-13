/* Character mesh: the faces the integer backface test drops (presentation only, PGXP).
 *
 * The character renderer (char_mesh_render 80036b0c) projects every vertex of a fighter's mesh
 * once (RTPT per bone part, into the vertex buffer 800c9d38: {SXY, SZ} per vertex) and then hands
 * the face lists to char_mesh_tris_to_ot (80036138, GT3) and char_mesh_quads_to_ot (800363c8,
 * GT4). Those keep a face only if GTE NCLIP on its *integer* screen positions says it faces the
 * camera (tri: NCLIP(A,C,B) > 0; quad: NCLIP(D,B,C) < 0 or NCLIP(A,B,C) > 0), then fill the
 * face's packet (colours from the lit-colour buffer 800cb7c8, positions from the vertex buffer;
 * the UV / CLUT / texture-page words are never written there: they are set up with the mesh) and
 * link it into the ordering table at a depth slot.
 *
 * On the PS1 that is exact enough: a face whose integer NCLIP is 0 has no area at 1x. With PGXP
 * the hi-res target draws the faces at their precise positions, where many of those dropped faces
 * do have area and face the camera. They are missing: the mesh has holes through which faces
 * behind show (the far arm, the inside of the hair), and which faces are integer-degenerate
 * changes every frame, so the holes flicker. A distant fighter has most faces only a few pixels
 * big; there about half the faces that point at the camera are dropped (NOTES.md, "Character
 * mesh and PGXP"). The reverse also happens: a face whose integer NCLIP passes can be back-facing
 * in precise positions; drawn as is it is an inverted sliver over its neighbours.
 *
 * So with PGXP and a hi-res target, before the guest function runs, the same face list is read
 * (read-only) and every face the game is about to drop that faces the camera in precise positions
 * is rebuilt as a packet exactly as the game would have written it (same words, same depth slot)
 * and kept as an "extra" for that ordering-table slot; when the GPU DMA walks the table the extras
 * are drawn into the hi-res target only (gpu_shadow_packet_in_area), just before the slot's own
 * packets. Every vertex of the mesh is marked as belonging to a backface-culled mesh
 * (PgxpVert.cull), and gpu.c skips, in the hi-res target only, a triangle of such vertices that is
 * back-facing in precise positions. The guest function then runs unchanged: guest RAM, registers
 * and timing are exactly those of a run without this file (state hashes, replays, netplay).
 */
#include "override.h"
#include "game_syms.h"
#include "hires.h"
#include "pgxp.h"
#include "hlr_hook.h"
#include <stdlib.h>
#include <string.h>

extern void (*g_gpu_ll_hook2)(u32 node);   /* bus.c: second GPU linked-list DMA node hook */

#define SCR_BUFFER_INDEX 0x1f800000u   /* u32: current double-buffer index (0/1) */
#define SCR_OT_NEAR      0x1f80000cu   /* ordering-table depth mapping (fighter_ot_depth_range) */
#define SCR_OT_MID       0x1f800010u
#define SCR_OT_FAR       0x1f800008u
#define OT_SLOTS         2048

/* guest read without side effects: RAM or scratchpad (the fight stack lives there), else 0 */
static inline u32 ram32(u32 a)
{
    u32 p = a & 0x1fffffff;
    if (p < 0x800000) return *(u32 *)(g_ram + (p & 0x1ffffc));
    if ((p & 0xfffffc00) == 0x1f800000) return *(u32 *)(g_scratch + (p & 0x3fc));
    return 0;
}
static inline u32 scr32(u32 a) { return *(u32 *)(g_scratch + (a & 0x3fc)); }

/* depth -> ordering-table slot: the 3-segment map of 80036138 / 800363c8 (u32 arithmetic) */
static u32 ot_slot(u32 z)
{
    u32 near = scr32(SCR_OT_NEAR), mid = scr32(SCR_OT_MID), far = scr32(SCR_OT_FAR);
    if (z < near) return (z * 0x155u) / (near + 1);
    if (z < mid) return ((z - near) * 0x400u) / (mid - (near - 1)) + 0x155;
    if (z < far) return ((z - mid) * 0x2abu) / (far - (mid - 1)) + 0x555;
    return 0x7ff;
}

/* ---- extras per ordering table -------------------------------------------------------- */

typedef struct { u32 w[13]; int n, next; u32 frame; PgxpVert v[4]; } Extra;
typedef struct {
    u32 base;                           /* RAM offset of the ordering table (0 = unused) */
    int head[OT_SLOTS];
    Extra *e; int ne, cap;
    bool walked;
} Capture;
static Capture s_cap[2];
static u64 s_faces, s_kept, s_extras, s_drawn, s_stale;

static Capture *capture_for(u32 base)
{
    for (int i = 0; i < 2; i++) if (s_cap[i].base == base && s_cap[i].ne) return &s_cap[i];
    for (int i = 0; i < 2; i++) if (!s_cap[i].ne) { s_cap[i].base = base; return &s_cap[i]; }
    /* both in use by other tables (a table built but never drawn): drop the older one */
    Capture *c = s_cap[0].e[0].frame <= s_cap[1].e[0].frame ? &s_cap[0] : &s_cap[1];
    s_stale += (u64)c->ne;
    c->ne = 0; c->base = base;
    return c;
}

static void ll_hook(u32 node)
{
    if (node == 0xffffffffu) {          /* end of a walk: the walked tables' extras are used up */
        for (int i = 0; i < 2; i++) if (s_cap[i].walked) { s_cap[i].ne = 0; s_cap[i].walked = false; }
        return;
    }
    for (int i = 0; i < 2; i++) {
        Capture *c = &s_cap[i];
        if (!c->ne || node < c->base || node >= c->base + 4 * OT_SLOTS) continue;
        c->walked = true;
        for (int k = c->head[(node - c->base) / 4]; k >= 0; k = c->e[k].next) {
            Extra *x = &c->e[k];
            if (g_pgxp_frame - x->frame > 3) { s_stale++; continue; }
            const PgxpVert *pv[13] = { 0 };
            for (int j = 0; j < x->n / 3; j++) pv[1 + 3 * j] = x->v[j].w > 0 ? &x->v[j] : 0;
            gpu_shadow_packet_in_area(x->w, x->n, pv);
            s_drawn++;
        }
    }
}

static void add_extra(u32 ot, u32 slot, const u32 *w, int n, const PgxpVert *const *q)
{
    if (!g_gpu_ll_hook2) g_gpu_ll_hook2 = ll_hook;
    Capture *c = capture_for(ot & 0x1ffffc);
    if (!c->ne) for (int i = 0; i < OT_SLOTS; i++) c->head[i] = -1;
    if (c->ne == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->e = realloc(c->e, sizeof(Extra) * (size_t)c->cap);
        if (!c->e) abort();
    }
    Extra *x = &c->e[c->ne];
    memcpy(x->w, w, sizeof(u32) * (size_t)n);
    x->n = n;
    x->frame = g_pgxp_frame;
    for (int j = 0; j < n / 3; j++) {
        if (q[j]) x->v[j] = *q[j];
        else x->v[j].w = 0;
    }
    x->next = c->head[slot & 0x7ff];
    c->head[slot & 0x7ff] = c->ne++;
    s_extras++;
}

/* ---- vertices ---------------------------------------------------------------------------- */

typedef struct { u32 sxy, sz; const PgxpVert *q; double x, y; } V;

static void vtx(V *v, u32 vb, s16 idx)
{
    u32 a = vb + (u32)(s32)idx * 8;
    v->sxy = ram32(a);
    v->sz = ram32(a + 4);
    u32 tag = g_pgxp_mem[pgxp_slot(a)];
    v->q = pgxp_lookup(tag, v->sxy);
    v->x = (s16)(v->sxy & 0xffff);
    v->y = (s16)(v->sxy >> 16);
    if (v->q) {
        v->x += v->q->dx; v->y += v->q->dy;
        g_pgxp_pool[tag & (PGXP_POOL_SIZE - 1)].cull = 1;   /* vertex of a backface-culled mesh */
    }
}

/* NCLIP on the integer words (what the GTE computes) and on the precise positions */
static s64 nclip_int(const V *a, const V *b, const V *c)
{
    s64 x0 = (s16)(a->sxy & 0xffff), y0 = (s16)(a->sxy >> 16), x1 = (s16)(b->sxy & 0xffff), y1 = (s16)(b->sxy >> 16);
    s64 x2 = (s16)(c->sxy & 0xffff), y2 = (s16)(c->sxy >> 16);
    return (s32)(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
}
static double nclip_prec(const V *a, const V *b, const V *c)
{
    return a->x * b->y + b->x * c->y + c->x * a->y - a->x * c->y - b->x * a->y - c->x * b->y;
}

/* a0 = the renderer's face-list block: [0] faces, [1] ordering table, [2] count, [4] vertex
 * buffer, [5] lit-colour buffer (80036b0c builds it on its stack) */
static void tris_extras(u32 a0)
{
    u32 rec = ram32(a0), ot = ram32(a0 + 4), vb = ram32(a0 + 16), cb = ram32(a0 + 20);
    s32 cnt = (s32)ram32(a0 + 8);
    u32 half = scr32(SCR_BUFFER_INDEX) ? 0x28 : 0;
    for (s32 i = 0; i < cnt; i++, rec += 0x5c) {
        u32 w50 = ram32(rec + 0x50), w54 = ram32(rec + 0x54), w58 = ram32(rec + 0x58);
        V a, b, c;                       /* SXY0 = A, SXY1 = C, SXY2 = B */
        vtx(&a, vb, (s16)w50); vtx(&c, vb, (s16)(w50 >> 16)); vtx(&b, vb, (s16)w54);
        s_faces++;
        if (nclip_int(&a, &c, &b) > 0) { s_kept++; continue; }  /* the game draws it */
        if (!(a.q || b.q || c.q) || !(nclip_prec(&a, &c, &b) > 0)) continue;
        u32 pkt = rec + half;
        u32 w[9] = {
            (ram32(cb + (u32)(s32)(s16)(w54 >> 16) * 4) & 0xffffff) | 0x34000000u, a.sxy, ram32(pkt + 12),
            ram32(cb + (u32)(s32)(s16)w58 * 4), c.sxy, ram32(pkt + 24),
            ram32(cb + (u32)(s32)(s16)(w58 >> 16) * 4), b.sxy, ram32(pkt + 36),
        };
        const PgxpVert *q[3] = { a.q, c.q, b.q };
        add_extra(ot, ot_slot((a.sz + c.sz + b.sz) / 3), w, 9, q);
    }
}

static void quads_extras(u32 a0)
{
    u32 rec = ram32(a0), ot = ram32(a0 + 4), vb = ram32(a0 + 16), cb = ram32(a0 + 20);
    s32 cnt = (s32)ram32(a0 + 8);
    u32 half = scr32(SCR_BUFFER_INDEX) ? 0x34 : 0;
    for (s32 i = 0; i < cnt; i++, rec += 0x78) {
        u32 w68 = ram32(rec + 0x68), w6c = ram32(rec + 0x6c), w70 = ram32(rec + 0x70), w74 = ram32(rec + 0x74);
        V a, b, c, d;
        vtx(&a, vb, (s16)w68); vtx(&b, vb, (s16)(w68 >> 16)); vtx(&c, vb, (s16)w6c); vtx(&d, vb, (s16)(w6c >> 16));
        s_faces++;
        if (nclip_int(&d, &b, &c) < 0 || nclip_int(&a, &b, &c) > 0) { s_kept++; continue; }   /* drawn */
        if (!(a.q || b.q || c.q || d.q)) continue;
        if (!(nclip_prec(&d, &b, &c) < 0 || nclip_prec(&a, &b, &c) > 0)) continue;
        s32 z = (s32)b.sz;               /* the game's minimum, signed compares */
        if ((s32)c.sz < z) z = (s32)c.sz;
        if ((s32)d.sz < z) z = (s32)d.sz;
        if ((s32)a.sz < z) z = (s32)a.sz;
        u32 pkt = rec + half;
        u32 w[12] = {
            ram32(cb + (u32)(s32)(s16)w70 * 4), a.sxy, ram32(pkt + 12),
            ram32(cb + (u32)(s32)(s16)(w70 >> 16) * 4), b.sxy, ram32(pkt + 24),
            ram32(cb + (u32)(s32)(s16)w74 * 4), c.sxy, ram32(pkt + 36),
            ram32(cb + (u32)(s32)(s16)(w74 >> 16) * 4), d.sxy, ram32(pkt + 48),
        };
        const PgxpVert *q[4] = { a.q, b.q, c.q, d.q };
        add_extra(ot, ot_slot((u32)z), w, 12, q);
    }
}

/* Not routed through override_call: the guest-visible part is the original function, run
 * unchanged after a read-only pass over its inputs. */
/* the native high-level renderer draws the characters itself (runtime/hlr/hlr_char.c): no extras */
static inline bool extras_wanted(void)
{
    return g_pgxp && g_hires && !(g_hlr.chars_native && g_hlr.chars_native());
}

void f_80036138(void)
{
    if (extras_wanted()) tris_extras(g_cpu.r[4]);
    f_80036138_orig();
}

void f_800363c8(void)
{
    if (extras_wanted()) quads_extras(g_cpu.r[4]);
    f_800363c8_orig();
}

/* char_mesh_render (80036b0c): a0 = model block, a1 = scene ordering table, a2 = 11. The native
 * high-level renderer (hlr_hook.h) reads the model before and the buffers / packets the original
 * wrote after; it only reads guest state (hlr_char.c). Not routed through override_call either. */
void f_80036b0c(void)
{
    if (!g_hlr.char_pre) { f_80036b0c_orig(); return; }
    g_hlr.char_pre(g_cpu.r[4], g_cpu.r[5]);
    f_80036b0c_orig();
    if (g_hlr.char_post) g_hlr.char_post();
}

void char_mesh_report(void)
{
    if (s_faces)
        rt_log("pgxp-mesh: %llu character faces, %llu kept by the game's integer backface test, %llu dropped by it "
               "but facing the camera at their precise positions (%.1f%% of those facing it; drawn in the hi-res "
               "target: %llu), %llu stale", (unsigned long long)s_faces, (unsigned long long)s_kept,
               (unsigned long long)s_extras, s_kept + s_extras ? 100.0 * (double)s_extras / (double)(s_kept + s_extras) : 0.0,
               (unsigned long long)s_drawn, (unsigned long long)s_stale);
}

/* fighter_shadow_render (8007ec5c): the original unchanged; the HLR's shadow capture reads what it
 * wrote (runtime/hlr/hlr_shadow.c). a0 = fighter, a1 = scene ordering table, a2 = packet records. */
void f_8007ec5c(void)
{
    if (!g_hlr.shadow_pre) { f_8007ec5c_orig(); return; }
    g_hlr.shadow_pre(g_cpu.r[4], g_cpu.r[5], g_cpu.r[6]);
    f_8007ec5c_orig();
    if (g_hlr.shadow_post) g_hlr.shadow_post();
}

