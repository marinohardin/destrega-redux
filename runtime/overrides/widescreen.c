/* Widescreen shadow pass for the stage renderer (presentation only).
 *
 * The stage renderer (0x80044210, `stage_render`) culls every stage polygon whose screen-space
 * bounding box lies beyond x = +-255 (and every stage object beyond +-257) before it is put into
 * the ordering table. Widening those bounds inside the real game would change guest RAM (packet
 * buffers, ordering table, per-object animation counters) and, through the extra instructions and
 * GPU DMA words, emulated timing - which measurably changes the fight (a different frame drops,
 * inputs land on different game ticks).
 *
 * So the widened renderer never runs "for real". With --widescreen (and a hi-res guard band):
 *
 *   1. snapshot scratchpad, CPU and GTE, and open the RAM write journal (psx.h g_mem_wlimit,
 *      bus.c: the first store to each 256-byte block saves its old contents);
 *   2. run the recompiled renderer with the widened bounds (WS_IMM in the generated code,
 *      analysis/widescreen_cull.txt), interrupts held off and hardware access blocked
 *      (bus sandbox), then record every packet in the stage ordering table's 2048 slots;
 *   3. undo the journal, restore the snapshot and run the renderer with the original bounds: this is the run the
 *      game sees, bit for bit the same as without --widescreen;
 *   4. record the ordering table again; per slot, the packets of pass 2 that pass 3 does not
 *      have (compared by length and payload, not by address or link) are the "extras";
 *   5. when the GPU DMA walks that ordering table, each slot's extras are sent to the hi-res
 *      target only (gpu_shadow_packet), just before the slot's own packets.
 *
 * Emulated state therefore never differs from a run without --widescreen; the full-state hash
 * test (tools/test_widescreen.sh) checks that. Costs: one extra renderer run plus copying the
 * blocks it wrote (about 38 KB of RAM and the matching PGXP tags in a fight) per rendered frame;
 * the full 2 MB RAM + 2 MB tag copies used before took half of the 0.4 ms pass.
 * DESTREGA_WS_JOURNAL_CHECK=1 also keeps full copies and checks every undo against them.
 * The BIOS is blocked like hardware (bios.c): it keeps host-side state and writes RAM directly.
 * During netplay re-simulation the pass does not run at all: netplay.c detaches g_hires for all
 * but the last 3 re-simulated vblanks, so the first check below takes the plain renderer.
 *
 * Host-side state the generated code keeps besides guest state (e.g. PGXP precision tags) is
 * saved/restored around the wide pass through widescreen_add_state_hook() (widescreen.h).
 * g_widescreen can change between frames (frontend_set_widescreen): the next stage_render then
 * simply does or does not run the shadow pass.
 */
#include "override.h"
#include "gen_protos.h"
#include "hires.h"
#include "widescreen.h"
#include "pgxp.h"
#include <stdlib.h>
#include <string.h>
#include "platform.h"

int g_widescreen;                       /* shadow pass on (frontend_set_widescreen, main.c) */
extern int g_widescreen_cull;           /* rt.c: WS_IMM() yields the widened bound */
extern int g_bus_sandbox;               /* bus.c */
extern u32 g_bus_sandbox_hits;
extern void (*g_gpu_ll_hook)(u32 node); /* bus.c: GPU linked-list DMA node hook */

/* ---- guest layout ------------------------------------------------------------------- */

#define SCR_BUFFER_INDEX 0x1f800000u    /* u32: current double-buffer index (0/1) */
#define OT_POINTERS      0x800c0724u    /* per buffer (stride 0x14): u32 ordering table pointer */
#define OT_STRIDE        0x14u
#define OT_SLOTS         2048           /* depth index 0..0x7ff (see NOTES.md, "Rendering") */

static u32 ot_base(void)                /* RAM offset of the current stage ordering table */
{
    u32 idx = mem_r32(SCR_BUFFER_INDEX);
    return mem_r32(OT_POINTERS + idx * OT_STRIDE) & 0x1ffffc;
}

static inline u32 ram32(u32 off) { return *(u32 *)(g_ram + (off & 0x1ffffc)); }

/* ---- packet lists ------------------------------------------------------------------- */

typedef struct { int slot, off, n; u32 hash; } Pkt;   /* off/n: words in the owning buffer */
typedef struct {
    Pkt *p; int np, cap;
    u32 *w; int nw, wcap;
    PgxpVert *v; int vcap;              /* PGXP: precise vertex per word (w = 0: none); wide pass */
} PktList;

static void list_clear(PktList *l) { l->np = 0; l->nw = 0; }

static void grow_v(PktList *l)
{
    if (l->vcap < l->wcap) { l->vcap = l->wcap; l->v = realloc(l->v, sizeof(PgxpVert) * (size_t)l->vcap); }
}

/* with_pv: also copy the precise vertex each word's PGXP tag points to (the pool entries are
 * undone with the rest of the wide pass, so they are copied, not referenced) */
static void list_add(PktList *l, int slot, u32 node, bool with_pv)
{
    int n = (int)(ram32(node) >> 24);
    if (l->np == l->cap) { l->cap = l->cap ? l->cap * 2 : 4096; l->p = realloc(l->p, sizeof(Pkt) * (size_t)l->cap); }
    if (l->nw + n > l->wcap) {
        while (l->nw + n > l->wcap) l->wcap = l->wcap ? l->wcap * 2 : 65536;
        l->w = realloc(l->w, sizeof(u32) * (size_t)l->wcap);
    }
    u32 h = 2166136261u ^ (u32)n;
    for (int i = 0; i < n; i++) {
        u32 v = ram32(node + 4 + 4 * (u32)i);
        l->w[l->nw + i] = v;
        h = (h ^ v) * 16777619u;
    }
    if (with_pv) {
        grow_v(l);
        for (int i = 0; i < n; i++) {
            u32 a = node + 4 + 4 * (u32)i;
            const PgxpVert *q = pgxp_lookup(g_pgxp_mem[pgxp_slot(a)], l->w[l->nw + i]);
            if (q) l->v[l->nw + i] = *q;
            else l->v[l->nw + i].w = 0;
        }
    }
    l->p[l->np++] = (Pkt){ slot, l->nw, n, h };
    l->nw += n;
}

/* Every packet chained from each of the OT_SLOTS slots, in chain (= draw) order. A chain ends
 * where it links back into the table (the next slot's entry) or at the terminator. */
static bool collect(PktList *l, u32 base, bool with_pv)
{
    list_clear(l);
    const u32 lo = base, hi = base + 4 * OT_SLOTS;
    if (hi > RAM_SIZE) return false;
    int budget = 1 << 20;
    for (int k = OT_SLOTS - 1; k >= 0; k--) {
        u32 node = ram32(lo + 4 * (u32)k) & 0xffffff;
        while (node != 0xffffff && !(node >= lo && node < hi)) {
            if (node >= RAM_SIZE || --budget < 0) return false;
            list_add(l, k, node, with_pv);
            node = ram32(node) & 0xffffff;
        }
    }
    return true;
}

/* ---- captures (one per ordering table: the game double-buffers) ---------------------- */

typedef struct {
    bool valid;
    u32 base;
    int first[OT_SLOTS];                /* index into ex.p of the slot's first extra, or -1 */
    int count[OT_SLOTS];
    PktList ex;
} Capture;

static Capture s_cap[2];
static PktList s_a, s_b;
static u8 s_scr_bak[sizeof g_scratch];
static bool s_ll_hooked;
static u64 s_journal_blocks;            /* RAM blocks the wide passes wrote (and were restored) */
static bool s_used[2];
static u64 s_passes, s_extras, s_rejected;
static u64 s_cost_ns;                   /* thread CPU time of the shadow pass (all but the real run) */

static u64 cpu_ns(void) { return plat_thread_cpu_ns(); }

static int cmp_slot(const void *x, const void *y)
{
    const Pkt *a = x, *b = y;
    return a->slot != b->slot ? (a->slot > b->slot ? -1 : 1) : (a->off < b->off ? -1 : a->off > b->off);
}

static bool same_pkt(const PktList *la, const Pkt *a, const PktList *lb, const Pkt *b)
{
    return a->n == b->n && a->hash == b->hash &&
           !memcmp(la->w + a->off, lb->w + b->off, sizeof(u32) * (size_t)a->n);
}

/* extras = per slot, the packets of a (wide pass) without an equal packet in b (real pass) */
static void build_capture(Capture *c, u32 base)
{
    c->base = base;
    list_clear(&c->ex);
    for (int k = 0; k < OT_SLOTS; k++) { c->first[k] = -1; c->count[k] = 0; }
    /* both lists are in slot order (OT_SLOTS-1 .. 0) by construction */
    int ib = 0;
    static bool *used; static int used_cap;
    if (used_cap < s_b.np) { used_cap = s_b.np * 2; used = realloc(used, (size_t)used_cap); }
    memset(used, 0, (size_t)s_b.np);
    for (int ia = 0; ia < s_a.np;) {
        int k = s_a.p[ia].slot;
        while (ib < s_b.np && s_b.p[ib].slot > k) ib++;
        int b0 = ib, b1 = ib;
        while (b1 < s_b.np && s_b.p[b1].slot == k) b1++;
        for (; ia < s_a.np && s_a.p[ia].slot == k; ia++) {
            const Pkt *pa = &s_a.p[ia];
            bool found = false;
            for (int j = b0; j < b1 && !found; j++)
                if (!used[j] && same_pkt(&s_a, pa, &s_b, &s_b.p[j])) { used[j] = true; found = true; }
            if (found) continue;
            PktList *e = &c->ex;
            if (e->np == e->cap) { e->cap = e->cap ? e->cap * 2 : 1024; e->p = realloc(e->p, sizeof(Pkt) * (size_t)e->cap); }
            if (e->nw + pa->n > e->wcap) {
                while (e->nw + pa->n > e->wcap) e->wcap = e->wcap ? e->wcap * 2 : 16384;
                e->w = realloc(e->w, sizeof(u32) * (size_t)e->wcap);
            }
            memcpy(e->w + e->nw, s_a.w + pa->off, sizeof(u32) * (size_t)pa->n);
            if (g_pgxp) { grow_v(e); memcpy(e->v + e->nw, s_a.v + pa->off, sizeof(PgxpVert) * (size_t)pa->n); }
            if (c->first[k] < 0) c->first[k] = e->np;
            c->count[k]++;
            e->p[e->np++] = (Pkt){ k, e->nw, pa->n, pa->hash };
            e->nw += pa->n;
        }
        ib = b1;
    }
    c->valid = c->ex.np > 0;
    s_extras += (u64)c->ex.np;
}

/* GPU linked-list DMA: draw a slot's extras (hi-res target only) when the walk reaches it.
 * A capture is used for one walk of its table, then dropped. */
static void ll_hook(u32 node)
{
    if (node == 0xffffffffu) {
        for (int i = 0; i < 2; i++) if (s_used[i]) { s_cap[i].valid = false; s_used[i] = false; }
        return;
    }
    for (int i = 0; i < 2; i++) {
        Capture *c = &s_cap[i];
        if (!c->valid || node < c->base || node >= c->base + 4 * OT_SLOTS) continue;
        int k = (int)((node - c->base) / 4);
        s_used[i] = true;
        for (int j = 0; j < c->count[k]; j++) {
            const Pkt *p = &c->ex.p[c->first[k] + j];
            const PgxpVert *pv[256];
            bool any = false;
            for (int j = 0; j < p->n && j < 256; j++) {
                const PgxpVert *q = g_pgxp && c->ex.v ? &c->ex.v[p->off + j] : 0;
                pv[j] = q && q->w > 0 ? q : 0;
                any |= pv[j] != 0;
            }
            gpu_shadow_packet(c->ex.w + p->off, p->n, any && p->n <= 256 ? pv : 0);
        }
    }
}

/* ---- control -------------------------------------------------------------------------- */

static struct { void (*save)(void); void (*restore)(void); } s_hooks[8];
static int s_nhooks;

void widescreen_add_state_hook(void (*save)(void), void (*restore)(void))
{
    if (s_nhooks < 8) { s_hooks[s_nhooks].save = save; s_hooks[s_nhooks].restore = restore; s_nhooks++; }
    else rt_log("widescreen: too many state hooks");
}

void widescreen_enable(bool on)
{
    g_widescreen = on;
    for (int i = 0; i < 2; i++) { s_cap[i].valid = false; s_used[i] = false; }
}

/* ---- the override ------------------------------------------------------------------- */

static void stage_render(void)
{
    if (!g_widescreen || !g_hires || !g_hires_guard) { f_80044210_orig(); return; }
    if (!s_ll_hooked) { g_gpu_ll_hook = ll_hook; s_ll_hooked = true; }
    const u32 base = ot_base();
    u64 t0 = cpu_ns();
    /* DESTREGA_WS_JOURNAL_CHECK=1 (development check): also keep full copies of RAM and the
     * PGXP tag table and compare them after the undo */
    static int check = -1;
    static u8 *full;
    static u32 *full_tags;
    if (check < 0) {
        const char *e = getenv("DESTREGA_WS_JOURNAL_CHECK");
        check = e && *e == '1' && (full = malloc(RAM_SIZE)) && (full_tags = malloc(sizeof g_pgxp_mem));
        if (check) rt_log("widescreen: checking the write journal against full copies");
    }
    if (check) { memcpy(full, g_ram, RAM_SIZE); memcpy(full_tags, g_pgxp_mem, sizeof g_pgxp_mem); }
    /* 1. snapshot: CPU, GTE, scratchpad, host hooks; RAM through the write journal */
    const Cpu cpu = g_cpu;
    const u32 depth = g_depth;
    GteState gte;
    gte_save(&gte);
    memcpy(s_scr_bak, g_scratch, sizeof g_scratch);
    for (int i = 0; i < s_nhooks; i++) if (s_hooks[i].save) s_hooks[i].save();
    if (!mem_journal_begin()) {
        for (int i = 0; i < s_nhooks; i++) if (s_hooks[i].restore) s_hooks[i].restore();
        f_80044210_orig();
        return;
    }
    /* 2. widened pass, sealed off from interrupts and hardware */
    const u32 hits = g_bus_sandbox_hits;
    g_cpu.next_event = ~0ull;
    g_bus_sandbox = 1;
    g_widescreen_cull = 1;
    f_80044210_orig();
    g_widescreen_cull = 0;
    g_bus_sandbox = 0;
    bool ok = g_bus_sandbox_hits == hits && collect(&s_a, base, g_pgxp);
    /* 3. restore, then the real pass */
    s_journal_blocks += mem_journal_undo();
    memcpy(g_scratch, s_scr_bak, sizeof g_scratch);
    gte_restore(&gte);
    for (int i = 0; i < s_nhooks; i++) if (s_hooks[i].restore) s_hooks[i].restore();
    g_cpu = cpu;
    g_depth = depth;
    if (check) {
        for (u32 i = 0; i < RAM_SIZE; i++)
            if (full[i] != g_ram[i]) rt_fatal("widescreen: write journal missed a RAM write at %08x", 0x80000000u + i);
        for (u32 i = 0; i < sizeof g_pgxp_mem / 4; i++)
            if (full_tags[i] != g_pgxp_mem[i]) rt_fatal("widescreen: PGXP tag journal missed slot %u", i);
    }

    u64 t1 = cpu_ns();
    f_80044210_orig();
    u64 t2 = cpu_ns();
    /* 4. extras for this table */
    ok = ok && collect(&s_b, base, false);
    Capture *c = &s_cap[s_cap[0].base == base || !s_cap[0].valid ? 0 : 1];
    if (s_cap[1].valid && s_cap[1].base == base) c = &s_cap[1];
    s_passes++;
    if (!ok) {
        s_cost_ns += (t1 - t0) + (cpu_ns() - t2);
        if (s_rejected++ < 3) rt_log("widescreen: shadow pass rejected (hardware access or bad table)");
        c->valid = false;
        return;
    }
    build_capture(c, base);
    s_cost_ns += (t1 - t0) + (cpu_ns() - t2);
}

/* Not routed through override_call (--verify-overrides): without --widescreen this is the
 * original, and with it the guest-visible part is the original run (the shadow pass is undone);
 * tools/test_widescreen.sh proves that with the full state hash. The verifier itself cannot check
 * stage_render: it runs the function twice from one RAM/CPU snapshot but cannot rewind the
 * scheduler, so an interrupt or CD event that lands inside the renderer (story-mode scenes, about
 * every 720 frames) shows up as a false memory mismatch. */
void f_80044210(void) { stage_render(); }

void widescreen_report(void)
{
    if (s_passes || g_widescreen) {
        rt_log("widescreen: %llu shadow passes, %llu extra stage packets, %llu rejected",
               (unsigned long long)s_passes, (unsigned long long)s_extras, (unsigned long long)s_rejected);
        if (s_passes)
            rt_log("widescreen-cost: shadow pass %.3f ms CPU per pass (snapshot, wide run, restore, diff;"
                   " the guard-band drawing is in the hi-res totals); %.1f KB of RAM journalled per pass",
                   s_cost_ns / 1e6 / (double)s_passes,
                   (double)s_journal_blocks * (1 << MEM_JOURNAL_SHIFT) / 1024.0 / (double)s_passes);
    }
}
