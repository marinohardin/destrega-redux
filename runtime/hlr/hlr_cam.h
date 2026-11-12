/* Float camera of the native stage renderer (docs/HLR_PLAN.md 2b): the GTE's world-screen
 * transform, perspective projection, depth cue (fog) and lighting, reconstructed from the GTE
 * registers the stage pass ran with, without the integer steps.
 *
 *   view   = RT * v / 4096 + TR                      (RT 4.12; row 1 carries the game's 0.625 Y scale)
 *   screen = OF / 65536 + H * view.xy / view.z       (RTPS: SX2 = OFX + H * IR1 / SZ3, UNR division)
 *   fog    = clamp((min(H * 65536 / z, 0x1ffff) * DQA + DQB) / 2^24, 0, 1)    (IR0 / 4096)
 *   colour = c + (FC / 16 - c) * fog                 (DPCS / NCDS interpolate_color, 0..255 units)
 *   lit    = 0x80 * clamp(BK + LCM * clamp(LLM * n / 4096, 0) / 4096, 0) / 4096   (NCDS, lm = 1)
 *
 * The GPU draws the same projection in clip space (hlr_gpu.c, shaders/hlr.glsl): x_clip = ax * vx
 * + bx * vz, y_clip = ay * vy + by * vz, z_clip = near, w = vz, i.e. reverse Z with an infinite far
 * plane; any aspect ratio (the frustum is the whole render pass, guard band included). Header-only
 * so the unit test (runtime/tests/test_hlr.c) checks the very code the renderer uses. */
#pragma once
#include "psx.h"

typedef struct {
    float rt[3][3];         /* RT / 4096 */
    float tr[3];
    float h;                /* projection plane distance H */
    float ofx, ofy;         /* OFX / 65536, OFY / 65536 */
    float dqa, dqb;         /* DQA (s16), DQB (s32) */
    float fc[3];            /* far colour, 0..255 units (FC / 16) */
    float llm[3][3];        /* LLM / 4096 */
    float lcm[3][3];        /* LCM / 4096 */
    float bk[3];            /* BK / 4096 (IR units: 1.0 = 4096) */
} HlrCam;

static inline void hlr_cam_mat(float m[3][3], const u32 *c)
{
    /* the GTE's 3x3 layout: c0 = 11 | 12 << 16, c1 = 13 | 21 << 16, c2 = 22 | 23 << 16, c3 = 31 | 32 << 16, c4 = 33 */
    const s16 e[9] = { (s16)c[0], (s16)(c[0] >> 16), (s16)c[1], (s16)(c[1] >> 16), (s16)c[2], (s16)(c[2] >> 16),
                       (s16)c[3], (s16)(c[3] >> 16), (s16)c[4] };
    for (int i = 0; i < 9; i++) m[i / 3][i % 3] = (float)e[i] / 4096.0f;
}

/* from a GteState's control registers (RT 0-4, TR 5-7, LLM 8-12, BK 13-15, LCM 16-20, FC 21-23,
 * OFX 24, OFY 25, H 26, DQA 27, DQB 28) */
static inline void hlr_cam_from_gte(HlrCam *k, const GteState *g)
{
    hlr_cam_mat(k->rt, &g->c[0]);
    hlr_cam_mat(k->llm, &g->c[8]);
    hlr_cam_mat(k->lcm, &g->c[16]);
    for (int i = 0; i < 3; i++) {
        k->tr[i] = (float)(s32)g->c[5 + i];
        k->bk[i] = (float)(s32)g->c[13 + i] / 4096.0f;
        k->fc[i] = (float)(s32)g->c[21 + i] / 16.0f;
    }
    k->ofx = (float)(s32)g->c[24] / 65536.0f;
    k->ofy = (float)(s32)g->c[25] / 65536.0f;
    k->h = (float)(u16)g->c[26];
    k->dqa = (float)(s16)g->c[27];
    k->dqb = (float)(s32)g->c[28];
}

static inline void hlr_cam_view(const HlrCam *k, const float v[3], float o[3])
{
    for (int i = 0; i < 3; i++) o[i] = k->rt[i][0] * v[0] + k->rt[i][1] * v[1] + k->rt[i][2] * v[2] + k->tr[i];
}

/* screen position relative to the drawing offset (the GTE's SX/SY); false behind the eye */
static inline bool hlr_cam_project(const HlrCam *k, const float view[3], float *sx, float *sy)
{
    if (view[2] <= 0.0f) return false;
    *sx = k->ofx + k->h * view[0] / view[2];
    *sy = k->ofy + k->h * view[1] / view[2];
    return true;
}

static inline float hlr_cam_fog(const HlrCam *k, float z)
{
    float hd = z > 0.0f ? k->h * 65536.0f / z : 131071.0f;
    if (hd > 131071.0f) hd = 131071.0f;
    float f = (hd * k->dqa + k->dqb) / 16777216.0f;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}
