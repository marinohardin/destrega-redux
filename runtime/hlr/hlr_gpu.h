/* Native 3D stage drawing on the SDL_GPU device of the hi-res backend (docs/HLR_PLAN.md 2b).
 * Presentation only: runs inside HiresBackend.external passes, between the hi-res backend's own
 * render passes, and writes the hi-res target only. */
#pragma once
#include "psx.h"

/* one vertex (44 bytes; the layout of HVI in hlr_gpu.c's MSL and of shaders/hlr.glsl) */
typedef struct {
    float pos[3];           /* model (= world) coordinates; HV_SCREEN: NDC x, y of the pass */
    float nrm[3];           /* lit: normal / 4096 */
    u8 col[4];              /* prelit / flat colour (0..255), lit: unused */
    float uv[2];            /* texels */
    u32 uvr;                /* u min | u max << 8 | v min << 16 | v max << 24 (UV clamp range) */
    u32 info;               /* HV_* */
} HlrVtx;
enum {
    HV_LAYER = 0xFF,        /* texture cache layer */
    HV_TEX = 1u << 8, HV_RAW = 1u << 9,
    HV_SHADE_SH = 10,       /* 2 bits: HLR_SH_FLAT / LIT / PRELIT */
    HV_SCREEN = 1u << 12,   /* pos is already in NDC, no depth (background strips) */
    HV_SEL_SH = 13,         /* 2 bits: 0 every texel, 1 texels without STP, 2 STP texels only */
    HV_DECAL = 1u << 15,    /* depth biased towards the camera by misc[3] world units (shadows on the floor) */
};

/* uniforms (vertex and fragment, the same block; std140-compatible: float4s only) */
typedef struct {
    float rt[3][4], tr[4];
    float proj[4];          /* ax, bx, ay, by: x_clip = ax * vx + bx * vz, y_clip = ay * vy + by * vz */
    float misc[4];          /* z_clip (the near plane), per-pixel shading (1) or per vertex (0),
                             * 1 = 15-bit target (PS1 red goes to .b, as the hi-res shaders write it),
                             * the decal depth bias (world units: HV_DECAL vertices are drawn as if that
                             * much nearer) */
    float fog[4];           /* H * 65536, DQA, DQB, fog on */
    float fc[4];            /* far colour 0..1 */
    float llm[3][4], lcm[3][4], bk[4];
} HlrUniforms;

enum { HLR_DRAW_OPAQUE_NOZ = 4 };
typedef struct { u32 first, count; int blend; } HlrDraw;   /* blend -1 = opaque (depth write), 0..3 semi,
                                                             * HLR_DRAW_OPAQUE_NOZ opaque without depth write */

#ifndef HLR_KPOS_DEFINED      /* also in hlr.h */
#define HLR_KPOS_DEFINED
typedef struct { u64 key; float p[3]; } HlrKpos;
#endif

/* a frame's geometry, shared by its passes (reference counted: freed after the last pass ran) */
typedef struct HlrGpuFrame {
    int refs;
    HlrVtx *v; u32 nv, vcap;
    u64 *k;                 /* per vertex: what it is across updates (HlrChr.key; 0 = static world /
                             * screen: moves with the camera only), for motion (phase 6) */
    HlrKpos *kp; u32 nkp;   /* keyed positions of vertices this frame does not draw (HlrFrame.kp) */
    HlrUniforms u;
    int px0, py0, pw, ph;   /* the passes' rectangle in target pixels */
    int samples;            /* MSAA sample count in effect */
    bool nearest_tex;       /* point-sampled level 0 (--hlr-look ps1) instead of trilinear + anisotropic */
    bool cleared;
    u64 serial;
} HlrGpuFrame;

typedef struct {
    HlrGpuFrame *f;
    HlrDraw *d; int nd;
} HlrGpuPass;

bool hlr_gpu_available(void);           /* the device of the hi-res backend can draw (shaders for its API) */
const char *hlr_gpu_unavailable_reason(void);
void hlr_gpu_set_msaa(int n);           /* 1 (off), 2, 4, 8; lowered to what the target format supports */
int hlr_gpu_get_msaa(void);
int hlr_gpu_msaa_for(int target_format);   /* the sample count used for that target format */
HlrGpuFrame *hlr_gpu_frame_new(void);
void hlr_gpu_frame_unref(HlrGpuFrame *f);
HlrVtx *hlr_gpu_frame_vtx(HlrGpuFrame *f, u32 n);   /* n more vertices at the end */
/* HiresBackend.external callback; ctx = a malloc'd HlrGpuPass (freed here, with its draw list) */
void hlr_gpu_external(void *ctx, void *cmdbuf, void *colour_tex, int w, int h);
typedef struct { u64 passes, draws, tris, msaa_passes, msaa_kept, uploads, upload_bytes, failed; } HlrGpuStats;
void hlr_gpu_get_stats(HlrGpuStats *st);

/* motion at 60 Hz (phase 6): the provider for hires_gpu_set_motion. Its in-between pass draws the
 * shown update's frame with every keyed vertex moved towards the same vertex of the next update's
 * frame, and the camera, fog and light uniforms interpolated the same way. */
struct HgpuMotion;
const struct HgpuMotion *hlr_gpu_motion_provider(void);
typedef struct { u64 frames, verts, keyed, matched, snapped, ambiguous, cuts, refused, unpaired; double max_step; } HlrMotionStats;
void hlr_gpu_motion_stats(HlrMotionStats *st);
