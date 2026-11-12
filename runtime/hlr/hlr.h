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
#include "hlr_cam.h"

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
/* on draws the stage natively (phase 2b): mode on, a backend with external passes, and the native
 * renderer available on its device (hlr_gpu.c: MSL, SPIR-V or DXIL shaders). Otherwise on replays the
 * captured packets like exact. */
bool hlr_native(void);
/* the widescreen shadow pass of stage_render is not needed (hlr_hook.h stage_native) */
bool hlr_stage_native(void);
void hlr_report(void);                          /* exit summary: "hlr: ...", "hlr-audit: ..." */

/* ---- frame store (hlr.c) ------------------------------------------------------------- */
typedef struct {
    u32 addr;           /* RAM offset of the packet's header (tag) */
    u16 n;              /* GP0 words after the tag */
    u16 kind;
    u32 off;            /* first word in HlrFrame.w */
    s32 nat;            /* native polygon it belongs to (HlrFrame.nat; a subdivision quarter: its
                         * parent), -1 = none */
} HlrPkt;

/* A stage polygon for native 3D drawing (phase 2b), read from the stage model by the capture: the
 * builders' vertices (model coordinates are world coordinates: every object is drawn with the
 * world-screen matrix alone), normals (lit) or colours (prelit, flat), and the packet whose UV /
 * CLUT / texture page words it uses. Every polygon of every object the flag rules let through is
 * listed, also those the PS1 culled (4:3 screen bounds, too near, an object outside the 4:3 view):
 * the native renderer has its own frustum, at any aspect ratio. */
enum { HLR_SH_FLAT, HLR_SH_LIT, HLR_SH_PRELIT };
void hlr_set_guard(int gx);     /* widescreen guard band columns (0 in 4:3): objects the stage renderer
                                 * culls at 4:3 are drawn natively only if they pass its test widened by gx */
enum { HLR_NAT_LINKED = 1, HLR_NAT_SPLIT = 2, HLR_NAT_NEAR = 4, HLR_NAT_HIDDEN = 8 };
typedef struct {
    s16 v[4][3];        /* vertices */
    s16 n[4][3];        /* lit: normals (4.12) */
    u32 col[4];         /* prelit: vertex colours as the colour stream holds them (code byte in
                         * col[2] for triangles, as the builder's RGBC); flat: the packet's colour
                         * word; lit: 0x34808080 / 0x3c808080 */
    u32 pkt;            /* RAM offset of the packet (tag) */
    u8 nv, shade, flags, kind;      /* kind: 0 F3, 1 F4, 2 GT3, 3 GT4 */
    s16 slot;           /* ordering-table slot for its depth (the PS1's when linked) */
    u16 zmax;           /* largest SZ of its vertices (clamped as the GTE does) */
} HlrNat;

typedef struct HlrFrame {
    bool valid;
    u32 ot;             /* RAM offset of the scene ordering table it was built into */
    u64 seq;            /* capture number */
    HlrPkt *p; int np, pcap;
    u32 *w; int nw, wcap;
    int *hash; int hcap;
    HlrNat *nat; int nn, ncap;      /* native polygons (mode on) */
    HlrCam cam;         /* the float camera of the pass (hlr_cam.h) */
} HlrFrame;

#define HLR_OT_SLOTS 2048
HlrFrame *hlr_frame_begin(u32 ot);              /* empty frame for this table (replaces an unwalked one) */
HlrPkt *hlr_frame_add(HlrFrame *f, u32 addr, int kind, const u32 *words, int n);
HlrNat *hlr_frame_add_nat(HlrFrame *f);      /* a zeroed native polygon */
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

/* ---- native drawing (hlr_native.c) --------------------------------------------------- */
enum { HLR_NEAR_CLIP, HLR_NEAR_PS1 };           /* --hlr-near: clip at the near plane / drop like the PS1 */
enum { HLR_LOOK_ENHANCED, HLR_LOOK_PS1 };       /* --hlr-look: per-pixel lighting and fog, filtered
                                                 * textures / per-vertex, point-sampled (PS1-like) */
void hlr_set_near(int m);
int hlr_get_near(void);
void hlr_set_look(int m);
int hlr_get_look(void);
bool hlr_native_begin(HlrFrame *f, u32 entry);  /* the walk entered f's table at node entry */
bool hlr_native_node(u32 node);                 /* every node from the entry on: true = suppress */
void hlr_native_end(void);                      /* end of the walk: the passes still due */
bool hlr_native_active(void);
void hlr_native_reset(void);                    /* drop what is pending (save-state load, mode change) */
typedef struct {
    u64 frames, passes, items, culled, near_dropped, stale, no_texture, suppressed, desync, lookups;
    u64 hidden_bad;     /* polygons of objects the PS1 skipped whose packet words were never written */
} HlrNativeStats;
void hlr_native_get_stats(HlrNativeStats *st);

/* ---- stage capture (hlr_stage.c) ----------------------------------------------------- */
void hlr_stage_pre(u32 mode);                   /* before the real pass of stage_render; mode = its a0 */
void hlr_stage_post(void);                      /* after it: audit, and the capture when drawing */
void hlr_stage_report(void);                    /* "hlr-audit: ..." */
typedef struct { u64 frames, linked, mismatch, unexpl, missing, heads, badwalk, alt_tris, alt_quads, alt_subs; } HlrAuditStats;
void hlr_stage_get_stats(HlrAuditStats *st);
