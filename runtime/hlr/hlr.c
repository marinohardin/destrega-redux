/* Native high-level renderer: switches, frame store and the linked-list filter (docs/HLR_PLAN.md).
 *
 * Presentation only. The stage capture (hlr_stage.c) turns a checked real pass of stage_render
 * into an HlrFrame: a copy of every stage packet (address, kind, GP0 words), keyed by the scene
 * ordering table it was linked into. When the GPU linked-list DMA later walks that table, the
 * filter (hlr_ll_filter, called by bus.c per node) recognises the frame by its OT slot nodes,
 * and for every captured packet whose words in RAM still equal the capture it draws the capture
 * into the hi-res target itself (gpu_hires_replay) and tells the walk to send the original words
 * to the 1x GPU only. The end of the walk consumes the frame. A save-state load or a mode change
 * drops every frame (they belong to the old timeline).
 *
 * In phase 2a "on" and "exact" both draw the captured packets through the existing hi-res path
 * (so the picture must be byte-identical to off); "on" additionally requires a backend with an
 * external render pass (the GPU backend), which is inserted (empty) when the walk enters a
 * captured table: the place phase 2b draws the native 3D stage. */
#include "hlr.h"
#include "hlr_gpu.h"
#include "../hlr_hook.h"
#include "../hires.h"
#include "../hires_gpu.h"
#include "../pgxp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_mode = HLR_OFF;
static void motion_apply(void);
static unsigned s_parts = HLR_PARTS_IMPLEMENTED;
static HlrFrame s_fr[2];
static HlrFrame *s_active;          /* the frame whose table the walk is in */
static u64 s_seq;
static HlrStats S;

/* ---- switches ------------------------------------------------------------------------- */
int hlr_parse_mode(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "off")) return HLR_OFF;
    if (!strcmp(s, "on")) return HLR_ON;
    if (!strcmp(s, "exact")) return HLR_EXACT;
    if (!strcmp(s, "audit")) return HLR_AUDIT;
    return -1;
}

const char *hlr_mode_name(int m)
{
    switch (m) {
    case HLR_ON: return "on";
    case HLR_EXACT: return "exact";
    case HLR_AUDIT: return "audit";
    default: return "off";
    }
}

static const struct { const char *name; unsigned bit; } k_parts[] = {
    { "stage", HLR_PART_STAGE }, { "chars", HLR_PART_CHARS }, { "shadows", HLR_PART_SHADOWS },
    { "fx", HLR_PART_FX }, { "story", HLR_PART_STORY },
};

bool hlr_parse_parts(const char *s, unsigned *out)
{
    if (!s || !out) return false;
    if (!strcmp(s, "all")) { *out = HLR_PARTS_ALL; return true; }
    if (!strcmp(s, "none") || !*s) { *out = 0; return true; }
    unsigned r = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        unsigned b = 0;
        for (size_t i = 0; i < sizeof k_parts / sizeof k_parts[0]; i++)
            if (!strcmp(t, k_parts[i].name)) b = k_parts[i].bit;
        if (!b) return false;
        r |= b;
    }
    *out = r;
    return true;
}

const char *hlr_parts_name(unsigned parts)
{
    static char buf[64];
    buf[0] = 0;
    for (size_t i = 0; i < sizeof k_parts / sizeof k_parts[0]; i++)
        if (parts & k_parts[i].bit) {
            if (buf[0]) strcat(buf, ",");
            strcat(buf, k_parts[i].name);
        }
    return buf[0] ? buf : "none";
}

static void on_restore(void) { S.restores++; hlr_frames_drop(); }

bool hlr_native(void)
{
    return s_mode == HLR_ON && g_hires && g_hires->external && hlr_gpu_available();
}

bool hlr_stage_native(void)
{
    return (s_parts & HLR_PART_STAGE) && hlr_native();
}

/* characters are attached to the stage's frame, so they need the stage drawn natively too */
bool hlr_chars_native(void)
{
    return (s_parts & HLR_PART_CHARS) && (s_parts & HLR_PART_STAGE) && hlr_native();
}

/* shadows are decals on the native floor: the stage must be drawn natively too */
bool hlr_shadows_native(void)
{
    return (s_parts & HLR_PART_SHADOWS) && (s_parts & HLR_PART_STAGE) && hlr_native();
}

/* effects join the stage's frame too (drawn in its passes, depth tested against it) */
bool hlr_fx_native(void)
{
    return (s_parts & HLR_PART_FX) && (s_parts & HLR_PART_STAGE) && hlr_native();
}

void hlr_set_mode(int mode)
{
    if (mode < HLR_OFF || mode > HLR_AUDIT) mode = HLR_OFF;
    if (mode == s_mode) return;
    hlr_frames_drop();
    s_mode = mode;
    motion_apply();
    if (mode == HLR_OFF) { memset(&g_hlr, 0, sizeof g_hlr); return; }
    g_hlr.stage_pre = hlr_stage_pre;
    g_hlr.stage_post = hlr_stage_post;
    g_hlr.ll_filter = mode == HLR_AUDIT ? NULL : hlr_ll_filter;
    g_hlr.on_restore = on_restore;
    g_hlr.stage_native = mode == HLR_ON ? hlr_stage_native : NULL;
    g_hlr.char_pre = hlr_char_pre;
    g_hlr.char_post = hlr_char_post;
    g_hlr.chars_native = mode == HLR_ON ? hlr_chars_native : NULL;
    g_hlr.shadow_pre = hlr_shadow_pre;
    g_hlr.shadow_post = hlr_shadow_post;
    g_hlr.fx_pre = hlr_fx_pre;
    g_hlr.fx_post = hlr_fx_post;
}

int hlr_get_mode(void) { return s_mode; }

static bool s_motion;
static void motion_apply(void) { hires_gpu_set_motion(s_motion && s_mode == HLR_ON ? hlr_gpu_motion_provider() : NULL); }
void hlr_set_motion(bool on) { s_motion = on; motion_apply(); }
bool hlr_get_motion(void) { return s_motion; }
void hlr_set_parts(unsigned parts) { if (parts != s_parts) hlr_frames_drop(); s_parts = parts & HLR_PARTS_ALL; }
unsigned hlr_get_parts(void) { return s_parts; }

bool hlr_draws(void)
{
    if (!g_hires) return false;
    if (s_mode == HLR_EXACT) return true;
    if (s_mode == HLR_ON) return g_hires->external != NULL;
    return false;
}

/* ---- frame store ------------------------------------------------------------------------ */
static void frame_clear(HlrFrame *f)
{
    f->valid = false; f->np = 0; f->nw = 0; f->nn = 0; f->nc = 0;
}

HlrFrame *hlr_frame_begin(u32 ot)
{
    S.captured++;
    HlrFrame *f = NULL;
    for (int i = 0; i < 2; i++)
        if (s_fr[i].valid && s_fr[i].ot == ot) f = &s_fr[i];
    if (f) {
        S.replaced++;                   /* built again before it was walked */
        if (f == s_active) s_active = NULL;
    } else {
        for (int i = 0; i < 2 && !f; i++) if (!s_fr[i].valid && &s_fr[i] != s_active) f = &s_fr[i];
        if (!f) {                       /* both hold unwalked frames: replace the older */
            f = s_fr[0].seq <= s_fr[1].seq ? &s_fr[0] : &s_fr[1];
            if (f == s_active) f = f == &s_fr[0] ? &s_fr[1] : &s_fr[0];
            S.replaced++;
        }
    }
    frame_clear(f);
    f->ot = ot & 0x1ffffc;
    return f;
}

HlrPkt *hlr_frame_add(HlrFrame *f, u32 addr, int kind, const u32 *words, int n)
{
    if (f->np == f->pcap) {
        f->pcap = f->pcap ? f->pcap * 2 : 2048;
        f->p = realloc(f->p, (size_t)f->pcap * sizeof *f->p);
        if (!f->p) rt_fatal("hlr: out of memory");
    }
    if (f->nw + n > f->wcap) {
        while (f->nw + n > f->wcap) f->wcap = f->wcap ? f->wcap * 2 : 16384;
        f->w = realloc(f->w, (size_t)f->wcap * sizeof *f->w);
        if (!f->w) rt_fatal("hlr: out of memory");
    }
    HlrPkt *p = &f->p[f->np++];
    p->addr = addr & 0x1ffffc; p->n = (u16)n; p->kind = (u16)kind; p->off = (u32)f->nw; p->nat = -1; p->chr = -1;
    memcpy(f->w + f->nw, words, (size_t)n * sizeof *words);
    f->nw += n;
    return p;
}

HlrNat *hlr_frame_add_nat(HlrFrame *f)
{
    if (f->nn == f->ncap) {
        f->ncap = f->ncap ? f->ncap * 2 : 4096;
        f->nat = realloc(f->nat, (size_t)f->ncap * sizeof *f->nat);
        if (!f->nat) rt_fatal("hlr: out of memory");
    }
    HlrNat *n = &f->nat[f->nn++];
    memset(n, 0, sizeof *n);
    return n;
}

HlrChr *hlr_frame_add_chr(HlrFrame *f)
{
    if (f->nc == f->chcap) {
        f->chcap = f->chcap ? f->chcap * 2 : 1024;
        f->chr = realloc(f->chr, (size_t)f->chcap * sizeof *f->chr);
        if (!f->chr) rt_fatal("hlr: out of memory");
    }
    HlrChr *c = &f->chr[f->nc++];
    memset(c, 0, sizeof *c);
    return c;
}

HlrFrame *hlr_frame_for_ot(u32 ot)
{
    ot &= 0x1ffffc;
    for (int i = 0; i < 2; i++)
        if (s_fr[i].valid && s_fr[i].ot == ot && &s_fr[i] != s_active) return &s_fr[i];
    return NULL;
}

static inline u32 hslot(u32 addr, int cap) { return (addr * 2654435761u) & (u32)(cap - 1); }

void hlr_frame_reindex(HlrFrame *f)
{
    int need = 16;
    while (need < f->np * 2) need <<= 1;
    if (need > f->hcap) {
        f->hcap = need;
        f->hash = realloc(f->hash, (size_t)f->hcap * sizeof *f->hash);
        if (!f->hash) rt_fatal("hlr: out of memory");
    }
    for (int i = 0; i < f->hcap; i++) f->hash[i] = -1;
    for (int i = 0; i < f->np; i++) {
        u32 h = hslot(f->p[i].addr, f->hcap);
        while (f->hash[h] >= 0) h = (h + 1) & (u32)(f->hcap - 1);
        f->hash[h] = i;
    }
}

void hlr_frame_commit(HlrFrame *f)
{
    hlr_frame_reindex(f);
    f->seq = ++s_seq;
    f->valid = true;
    S.committed++;
    S.packets += (u64)f->np;
}

void hlr_frame_abort(HlrFrame *f) { frame_clear(f); S.rejected++; }

const HlrPkt *hlr_frame_find(const HlrFrame *f, u32 addr)
{
    if (!f->valid || !f->hcap) return NULL;
    addr &= 0x1ffffc;
    u32 h = hslot(addr, f->hcap);
    while (f->hash[h] >= 0) {
        if (f->p[f->hash[h]].addr == addr) return &f->p[f->hash[h]];
        h = (h + 1) & (u32)(f->hcap - 1);
    }
    return NULL;
}

void hlr_frames_drop(void)
{
    for (int i = 0; i < 2; i++) if (s_fr[i].valid) { S.dropped++; frame_clear(&s_fr[i]); }
    s_active = NULL;
    hlr_native_reset();
    hlr_char_reset();
}

const HlrFrame *hlr_frame_get(int i) { return i >= 0 && i < 2 ? &s_fr[i] : NULL; }

/* ---- the linked-list filter --------------------------------------------------------------- */
static void external_noop(void *ctx, void *cmdbuf, void *colour_tex, int w, int h)
{
    (void)ctx; (void)cmdbuf; (void)colour_tex; (void)w; (void)h;
}

/* ---- walk attribution (DESTREGA_HLR_WALK_DIAG=LO-HI[:BUCKET], libetc vblank count) ---------------
 * Diagnostic only (phase 7, story scenes): every packet node the GPU linked-list walk visits is
 * classed as a captured stage packet, a captured character face or "other"; others are counted by
 * GP0 code and by RAM page (4 KiB) so their writer can be looked up. One line per BUCKET vblanks
 * (default 500). Reads nothing but RAM and the frame store. */
static struct {
    int on;                             /* -1 = not read yet */
    u32 lo, hi, bucket, cur;
    u64 walks, stage, chr, other, outside;
    u32 code[256];
    struct { u32 page, n; u8 code; } pg[64];
    int npg;
} WD = { .on = -1 };

static void walk_diag_flush(void)
{
    if (!WD.walks && !WD.other && !WD.stage) return;
    char codes[512] = "", pages[1024] = "";
    for (int c = 0; c < 256; c++) if (WD.code[c] && strlen(codes) < sizeof codes - 16)
        snprintf(codes + strlen(codes), sizeof codes - strlen(codes), " %02x:%u", c, WD.code[c]);
    for (int i = 0; i < WD.npg; i++) if (strlen(pages) < sizeof pages - 24)
        snprintf(pages + strlen(pages), sizeof pages - strlen(pages), " %05x(%02x):%u",
                 0x80000 | (WD.pg[i].page >> 12), WD.pg[i].code, WD.pg[i].n);
    rt_log("hlr-walk-diag: vblank %u..%u: %llu walks; packets in a captured table: %llu stage, %llu character,"
           " %llu other; %llu outside it; other codes%s; other pages%s", WD.cur, WD.cur + WD.bucket - 1,
           (unsigned long long)WD.walks, (unsigned long long)WD.stage, (unsigned long long)WD.chr,
           (unsigned long long)WD.other, (unsigned long long)WD.outside, codes, pages);
    WD.walks = WD.stage = WD.chr = WD.other = WD.outside = 0;
    memset(WD.code, 0, sizeof WD.code);
    WD.npg = 0;
}

static void walk_diag(u32 node)
{
    if (WD.on < 0) {
        const char *e = getenv("DESTREGA_HLR_WALK_DIAG");
        WD.on = e != NULL;
        WD.hi = ~0u; WD.bucket = 500;
        if (e && strchr(e, '-')) { WD.lo = (u32)strtoul(e, NULL, 10); WD.hi = (u32)strtoul(strchr(e, '-') + 1, NULL, 10); }
        if (e && strchr(e, ':')) WD.bucket = (u32)strtoul(strchr(e, ':') + 1, NULL, 10);
        if (!WD.bucket) WD.bucket = 500;
    }
    if (!WD.on) return;
    const u32 vbc = *(u32 *)(g_ram + 0xbf2b4);
    if (vbc < WD.lo || vbc > WD.hi) return;
    const u32 b = vbc / WD.bucket * WD.bucket;
    if (b != WD.cur) { walk_diag_flush(); WD.cur = b; }
    if (node == 0xffffffffu) { WD.walks++; return; }
    node &= 0x1ffffc;
    const u32 hdr = *(u32 *)(g_ram + node);
    if (!(hdr >> 24)) return;           /* an empty node (ordering-table slot or a bare link) */
    for (int i = 0; i < 2; i++)
        if (s_fr[i].valid && node >= s_fr[i].ot && node < s_fr[i].ot + 4 * HLR_OT_SLOTS) return;
    if (!s_active) { WD.outside++; return; }
    const HlrPkt *p = hlr_frame_find(s_active, node);
    if (p && p->chr >= 0) { WD.chr++; return; }
    if (p) { WD.stage++; return; }
    WD.other++;
    const u8 code = (u8)(*(u32 *)(g_ram + node + 4) >> 24);
    WD.code[code]++;
    const u32 page = node & ~0xfffu;
    int i = 0;
    while (i < WD.npg && !(WD.pg[i].page == page && WD.pg[i].code == code)) i++;
    if (i < WD.npg) WD.pg[i].n++;
    else if (WD.npg < 64) { WD.pg[WD.npg].page = page; WD.pg[WD.npg].code = code; WD.pg[WD.npg].n = 1; WD.npg++; }
}

bool hlr_ll_filter(u32 node)
{
    walk_diag(node);
    if (node == 0xffffffffu) {              /* end of the walk: the frame has been drawn */
        hlr_native_end();
        if (s_active) { S.walked++; frame_clear(s_active); s_active = NULL; }
        return false;
    }
    node &= 0x1ffffc;
    if (hlr_native_active()) return hlr_native_node(node);
    for (int i = 0; i < 2; i++) {
        HlrFrame *f = &s_fr[i];
        if (f->valid && node >= f->ot && node < f->ot + 4 * HLR_OT_SLOTS) {
            if (s_active != f) {
                s_active = f;
                /* on: the native 3D stage (hlr_native.c) schedules its passes into the rest of
                 * the walk; else (exact, or no native renderer) the 2a replay below */
                if (f->nn && hlr_native() && hlr_native_begin(f, node)) { S.externals++; return hlr_native_node(node); }
                if (g_hires && g_hires->external) { g_hires->external(external_noop, NULL); S.externals++; }
            }
            return false;                   /* an ordering-table slot: nothing to draw */
        }
    }
    if (!s_active || !g_hires) return false;
    const HlrPkt *p = hlr_frame_find(s_active, node);
    if (!p) return false;
    const u32 *ram = (const u32 *)(g_ram + node);
    if ((ram[0] >> 24) != p->n) { S.stale_len++; return false; }
    const u32 *w = s_active->w + p->off;
    if (memcmp(ram + 1, w, (size_t)p->n * 4)) { S.stale_pkts++; return false; }
    /* test hook: DESTREGA_HLR_TEST_DROP=N suppresses every Nth packet without drawing it, so the
     * byte-identical checks can show they would notice a missing packet (tools/test_hlr.py) */
    static int drop_every = -1;
    if (drop_every < 0) { const char *e = getenv("DESTREGA_HLR_TEST_DROP"); drop_every = e ? atoi(e) : 0; }
    if (drop_every > 0 && S.suppressed % (u64)drop_every == (u64)drop_every - 1) { S.suppressed++; return true; }
    u32 tags[16];
    for (int i = 0; i < p->n && i < 16; i++) tags[i] = g_pgxp_mem[((node + 4 + 4 * (u32)i) & 0x1ffffc) >> 2];
    gpu_hires_replay(w, p->n, tags);
    S.suppressed++;
    return true;
}

void hlr_get_stats(HlrStats *st) { *st = S; }

void hlr_report(void)
{
    if (WD.on > 0) walk_diag_flush();
    if (s_mode == HLR_OFF && !S.captured) return;
    hlr_stage_report();
    hlr_char_report();
    hlr_shadow_report();
    hlr_fx_report();
    if (s_mode == HLR_AUDIT) return;
    u64 left = 0;
    for (int i = 0; i < 2; i++) if (s_fr[i].valid) left++;
    rt_log("hlr: mode %s, parts %s%s; %llu frames captured, %llu committed, %llu walked, %llu replaced,"
           " %llu dropped (%llu save-state loads), %llu left; %llu packets, %llu drawn by the HLR,"
           " %llu stale (%llu length); %llu external passes",
           hlr_mode_name(s_mode), hlr_parts_name(s_parts), hlr_draws() ? "" : " (not drawing)",
           (unsigned long long)S.captured, (unsigned long long)S.committed, (unsigned long long)S.walked,
           (unsigned long long)S.replaced, (unsigned long long)S.dropped, (unsigned long long)S.restores,
           (unsigned long long)left, (unsigned long long)S.packets, (unsigned long long)S.suppressed,
           (unsigned long long)(S.stale_pkts + S.stale_len), (unsigned long long)S.stale_len,
           (unsigned long long)S.externals);
    HlrNativeStats N;
    hlr_native_get_stats(&N);
    if (N.frames) {
        HlrGpuStats G;
        hlr_gpu_get_stats(&G);
        if (N.chr_items || N.chr_back)
            rt_log("hlr-native-chars: %.0f character faces per frame drawn in 3D, %.0f per frame facing away (float test)",
                   (double)N.chr_items / (double)N.frames, (double)N.chr_back / (double)N.frames);
        if (N.shadow_items)
            rt_log("hlr-native-shadows: %.0f shadow triangles per frame drawn in 3D (depth-biased decals)",
                   (double)N.shadow_items / (double)N.frames);
        if (N.fx_items)
            rt_log("hlr-native-fx: %.1f effect polygons per frame drawn in 3D (depth tested, PS1 order among effects), %.2f pass points per frame added for them",
                   (double)N.fx_items / (double)N.frames, (double)N.fx_points / (double)N.frames);
        rt_log("hlr-native-draw: %llu frames, %.2f passes per frame, %.0f polygons per frame drawn in 3D"
               " (near %s, look %s, MSAA %dx); %llu culled by the camera, %llu dropped as too near, %llu stale,"
               " %llu without texture, %llu of skipped objects without packet words; %llu packets suppressed; %llu walks given up; GPU %llu passes"
               " (%llu MSAA, %llu of them keeping the previous pass's samples), %llu draws, %llu triangles, %llu failed",
               (unsigned long long)N.frames, (double)N.passes / (double)N.frames, (double)N.items / (double)N.frames,
               hlr_get_near() == HLR_NEAR_PS1 ? "ps1" : "clip", hlr_get_look() == HLR_LOOK_PS1 ? "ps1" : "enhanced",
               hlr_gpu_get_msaa(), (unsigned long long)N.culled, (unsigned long long)N.near_dropped,
               (unsigned long long)N.stale, (unsigned long long)N.no_texture, (unsigned long long)N.hidden_bad,
               (unsigned long long)N.suppressed,
               (unsigned long long)N.desync, (unsigned long long)G.passes, (unsigned long long)G.msaa_passes,
               (unsigned long long)G.msaa_kept,
               (unsigned long long)G.draws, (unsigned long long)G.tris, (unsigned long long)G.failed);
    }
    HgpuMotionStats M;
    hires_gpu_motion_stats(&M);
    if (M.vblanks) {
        HlrMotionStats I;
        hlr_gpu_motion_stats(&I);
        rt_log("hlr-motion: %llu vblanks, %llu with a new picture, %llu in-between pictures (%llu commands redrawn);"
               " not replayed: %llu without a pair, %llu not shown, %llu without native passes, %llu too many commands, %llu failed",
               (unsigned long long)M.vblanks, (unsigned long long)M.recorded, (unsigned long long)M.replays,
               (unsigned long long)M.ops_replayed, (unsigned long long)M.no_pair, (unsigned long long)M.not_shown,
               (unsigned long long)M.no_token, (unsigned long long)M.overflow, (unsigned long long)M.failed);
        rt_log("hlr-motion-frames: %llu in-between frames (%llu without a next frame, %llu other pass area, %llu camera cuts);"
               " vertices %llu, keyed %llu, moved %llu (largest step %.0f), snapped %llu, ambiguous %llu",
               (unsigned long long)I.frames, (unsigned long long)I.unpaired, (unsigned long long)I.refused,
               (unsigned long long)I.cuts, (unsigned long long)I.verts, (unsigned long long)I.keyed,
               (unsigned long long)I.matched, I.max_step, (unsigned long long)I.snapped, (unsigned long long)I.ambiguous);
    }
}
