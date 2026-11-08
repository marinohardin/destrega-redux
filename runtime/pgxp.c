/* PGXP-style vertex precision tracking: shadow tags and the precise vertex pool.
 * See pgxp.h. Host-only state: never part of the emulated machine. */
#include <string.h>
#include "pgxp.h"

bool g_pgxp;
u32 g_pgxp_reg[32];
u32 g_pgxp_mem[PGXP_IO_SLOT + 1];
u32 g_pgxp_gte[3];
PgxpVert g_pgxp_pool[PGXP_POOL_SIZE];
u64 g_pgxp_stat_vtx, g_pgxp_stat_hit, g_pgxp_stat_new;
u64 g_pgxp_stat_miss_gte, g_pgxp_stat_miss_stale;
u32 g_pgxp_recent[1u << 16];

static u32 s_next = 1;
u32 g_pgxp_frame;
const u32 *g_pgxp_callstack;
const u32 *g_pgxp_depth;
int g_pgxp_diag;
u64 g_pgxp_stat_bad_d, g_pgxp_stat_bad_w, g_pgxp_stat_old, g_pgxp_stat_tris;

#include <stdio.h>
#include <math.h>
void pgxp_check_tri(const PgxpVert *const q[3], const u32 sxy[3])
{
    float wmin = 1e30f, wmax = 0, dmax = 0;
    u32 age = 0;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (!q[i]) continue;
        n++;
        float d = fmaxf(fabsf(q[i]->dx), fabsf(q[i]->dy));
        if (d > dmax) dmax = d;
        if (q[i]->w < wmin) wmin = q[i]->w;
        if (q[i]->w > wmax) wmax = q[i]->w;
        u32 a = g_pgxp_frame - q[i]->frame;
        if (a > age) age = a;
    }
    if (!n) return;
    g_pgxp_stat_tris++;
    bool bd = dmax > 1.5f, bw = n > 1 && wmax > 2.0f * wmin, bo = age > 2;
    g_pgxp_stat_bad_d += bd; g_pgxp_stat_bad_w += bw; g_pgxp_stat_old += bo;
    if ((bd || bw || bo) && g_pgxp_diag > 0) {
        g_pgxp_diag--;
        fprintf(stderr, "pgxp-diag f%u:%s%s%s", g_pgxp_frame, bd ? " far" : "", bw ? " w-ratio" : "", bo ? " old" : "");
        for (int i = 0; i < 3; i++) {
            if (q[i]) fprintf(stderr, " [%d,%d d=%.2f,%.2f w=%.0f site=%08x age=%u]",
                              (s16)(sxy[i] & 0xFFFF), (s16)(sxy[i] >> 16), q[i]->dx, q[i]->dy, q[i]->w,
                              q[i]->site, g_pgxp_frame - q[i]->frame);
            else fprintf(stderr, " [%d,%d int]", (s16)(sxy[i] & 0xFFFF), (s16)(sxy[i] >> 16));
        }
        fprintf(stderr, "\n");
    }
}

u32 pgxp_new(u32 sxy, float dx, float dy, float w)
{
    u32 t = s_next;
    s_next = (s_next + 1) & (PGXP_POOL_SIZE - 1);
    if (!s_next) s_next = 1;   /* tag 0 = no association */
    PgxpVert *v = &g_pgxp_pool[t];
    v->sxy = sxy; v->dx = dx; v->dy = dy; v->w = w;
    v->frame = g_pgxp_frame;
    v->site = g_pgxp_callstack ? g_pgxp_callstack[(*g_pgxp_depth - 1) & 1023] : 0;
    g_pgxp_stat_new++;
    g_pgxp_recent[pgxp_recent_slot(sxy)] = sxy;
    return t;
}

void pgxp_reset(void)
{
    memset(g_pgxp_reg, 0, sizeof g_pgxp_reg);
    memset(g_pgxp_mem, 0, sizeof g_pgxp_mem);
    memset(g_pgxp_gte, 0, sizeof g_pgxp_gte);
    memset(g_pgxp_pool, 0, sizeof g_pgxp_pool);
    s_next = 1;
}
