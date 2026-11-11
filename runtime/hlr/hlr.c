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
#include "../hlr_hook.h"
#include "../hires.h"
#include "../pgxp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_mode = HLR_OFF;
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

void hlr_set_mode(int mode)
{
    if (mode < HLR_OFF || mode > HLR_AUDIT) mode = HLR_OFF;
    if (mode == s_mode) return;
    hlr_frames_drop();
    s_mode = mode;
    if (mode == HLR_OFF) { memset(&g_hlr, 0, sizeof g_hlr); return; }
    g_hlr.stage_pre = hlr_stage_pre;
    g_hlr.stage_post = hlr_stage_post;
    g_hlr.ll_filter = mode == HLR_AUDIT ? NULL : hlr_ll_filter;
    g_hlr.on_restore = on_restore;
}

int hlr_get_mode(void) { return s_mode; }
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
    f->valid = false; f->np = 0; f->nw = 0;
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

void hlr_frame_add(HlrFrame *f, u32 addr, int kind, const u32 *words, int n)
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
    p->addr = addr & 0x1ffffc; p->n = (u16)n; p->kind = (u16)kind; p->off = (u32)f->nw;
    memcpy(f->w + f->nw, words, (size_t)n * sizeof *words);
    f->nw += n;
}

static inline u32 hslot(u32 addr, int cap) { return (addr * 2654435761u) & (u32)(cap - 1); }

void hlr_frame_commit(HlrFrame *f)
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
}

const HlrFrame *hlr_frame_get(int i) { return i >= 0 && i < 2 ? &s_fr[i] : NULL; }

/* ---- the linked-list filter --------------------------------------------------------------- */
static void external_noop(void *ctx, void *cmdbuf, void *colour_tex, int w, int h)
{
    (void)ctx; (void)cmdbuf; (void)colour_tex; (void)w; (void)h;
}

bool hlr_ll_filter(u32 node)
{
    if (node == 0xffffffffu) {              /* end of the walk: the frame has been drawn */
        if (s_active) { S.walked++; frame_clear(s_active); s_active = NULL; }
        return false;
    }
    node &= 0x1ffffc;
    for (int i = 0; i < 2; i++) {
        HlrFrame *f = &s_fr[i];
        if (f->valid && node >= f->ot && node < f->ot + 4 * HLR_OT_SLOTS) {
            if (s_active != f) {
                s_active = f;
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
    u32 tags[16];
    for (int i = 0; i < p->n && i < 16; i++) tags[i] = g_pgxp_mem[((node + 4 + 4 * (u32)i) & 0x1ffffc) >> 2];
    gpu_hires_replay(w, p->n, tags);
    S.suppressed++;
    return true;
}

void hlr_get_stats(HlrStats *st) { *st = S; }

void hlr_report(void)
{
    if (s_mode == HLR_OFF && !S.captured) return;
    hlr_stage_report();
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
}
