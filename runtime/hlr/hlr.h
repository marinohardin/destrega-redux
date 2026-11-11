/* Native high-level renderer (docs/HLR_PLAN.md). Presentation only, outside the netplay build
 * identity: it reads guest RAM through the hooks of runtime/hlr_hook.h and never writes guest
 * state.
 *
 * Phase 2a: the stage capture (hlr_stage.c) predicts every stage packet with the builders' own
 * rules on a host GTE, checks the prediction against what the game wrote (audit), and keeps the
 * checked packets as a frame (HlrFrame, a copy: never a pointer into guest RAM). In exact mode
 * the GPU linked-list walk draws each captured packet into the hi-res target itself (painter's
 * order, PS1 projection, the existing hi-res pipelines) and the original packet reaches the 1x GPU
 * only, so the picture must stay byte-identical to off; an external render pass of the GPU
 * backend (HiresBackend.external, a no-op in 2a) marks where the native 3D pass of phase 2b goes. */
#pragma once
#include "psx.h"

enum { HLR_OFF, HLR_ON, HLR_EXACT, HLR_AUDIT };
enum { HLR_PART_STAGE = 1, HLR_PART_CHARS = 2, HLR_PART_SHADOWS = 4, HLR_PART_FX = 8, HLR_PART_STORY = 16,
       HLR_PARTS_ALL = 31, HLR_PARTS_IMPLEMENTED = HLR_PART_STAGE };

/* ---- switches (hlr.c) ---------------------------------------------------------------- */
int hlr_parse_mode(const char *s);              /* -1 if invalid */
const char *hlr_mode_name(int mode);
bool hlr_parse_parts(const char *s, unsigned *out);   /* "stage,chars,..." | "all" | "none" */
const char *hlr_parts_name(unsigned parts);     /* static buffer */
void hlr_set_mode(int mode);                    /* installs / removes the hooks (between frames) */
int hlr_get_mode(void);
void hlr_set_parts(unsigned parts);
unsigned hlr_get_parts(void);
/* the mode in effect for the current hi-res backend: on needs HiresBackend.external (GPU), and
 * in 2a it draws like exact */
bool hlr_draws(void);
void hlr_report(void);                          /* exit summary: "hlr: ...", "hlr-audit: ..." */

/* ---- frame store (hlr.c) ------------------------------------------------------------- */
typedef struct {
    u32 addr;           /* RAM offset of the packet's header (tag) */
    u16 n;              /* GP0 words after the tag */
    u16 kind;
    u32 off;            /* first word in HlrFrame.w */
} HlrPkt;

typedef struct HlrFrame {
    bool valid;
    u32 ot;             /* RAM offset of the scene ordering table it was built into */
    u64 seq;            /* capture number */
    HlrPkt *p; int np, pcap;
    u32 *w; int nw, wcap;
    int *hash; int hcap;
} HlrFrame;

#define HLR_OT_SLOTS 2048
HlrFrame *hlr_frame_begin(u32 ot);              /* empty frame for this table (replaces an unwalked one) */
void hlr_frame_add(HlrFrame *f, u32 addr, int kind, const u32 *words, int n);
void hlr_frame_commit(HlrFrame *f);
void hlr_frame_abort(HlrFrame *f);
const HlrPkt *hlr_frame_find(const HlrFrame *f, u32 addr);
void hlr_frames_drop(void);                     /* every capture (save-state load, mode change) */
bool hlr_ll_filter(u32 node);                   /* hlr_hook.h ll_filter */
const HlrFrame *hlr_frame_get(int i);           /* tests: the two slots */

typedef struct {
    u64 captured, committed, rejected, replaced, walked, dropped, restores;
    u64 packets, suppressed, stale_pkts, stale_len, externals, not_drawn;
} HlrStats;
void hlr_get_stats(HlrStats *st);

/* ---- stage capture (hlr_stage.c) ----------------------------------------------------- */
void hlr_stage_pre(u32 mode);                   /* before the real pass of stage_render; mode = its a0 */
void hlr_stage_post(void);                      /* after it: audit, and the capture when drawing */
void hlr_stage_report(void);                    /* "hlr-audit: ..." */
typedef struct { u64 frames, linked, mismatch, unexpl, missing, heads, badwalk, alt_tris, alt_quads, alt_subs; } HlrAuditStats;
void hlr_stage_get_stats(HlrAuditStats *st);
