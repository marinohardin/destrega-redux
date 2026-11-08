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
u64 g_pgxp_stat_bad_d, g_pgxp_stat_bad_w, g_pgxp_stat_old, g_pgxp_stat_tris, g_pgxp_stat_split;
u32 g_pgxp_batch = 1;
u64 g_pgxp_stat_welded, g_pgxp_stat_weld_int, g_pgxp_stat_weld_full;

#include <stdio.h>
#include <math.h>

#define WELD_BITS 16
#define WELD_PROBE 16
static struct { u32 sxy, batch; bool have; PgxpVert v; } s_weld[1u << WELD_BITS];

/* Two precise results for one SXY word are the same point only at a similar depth: different
 * meshes can land on the same integer pixel (seen: a stage vertex at w=9073 and a character
 * vertex at w=3065 on word 100,-94), and welding those would hand one the other's W. */
static bool same_depth(float a, float b) { return a <= b * 1.25f && b <= a * 1.25f; }

const PgxpVert *pgxp_weld(u32 sxy, const PgxpVert *q)
{
    u32 h = pgxp_recent_slot(sxy);
    for (int k = 0; k < WELD_PROBE; k++, h = (h + 1) & ((1u << WELD_BITS) - 1)) {
        if (s_weld[h].batch != g_pgxp_batch) {   /* first use of this word in the batch */
            s_weld[h].batch = g_pgxp_batch;
            s_weld[h].sxy = sxy;
            s_weld[h].have = q != 0;
            if (q) s_weld[h].v = *q;
            return q ? &s_weld[h].v : 0;
        }
        if (s_weld[h].sxy != sxy) continue;
        if (q && s_weld[h].have && !same_depth(q->w, s_weld[h].v.w)) continue;   /* another mesh */
        if (!s_weld[h].have) { g_pgxp_stat_weld_int += q != 0; return 0; }
        if (!q || q->dx != s_weld[h].v.dx || q->dy != s_weld[h].v.dy) g_pgxp_stat_welded++;
        return &s_weld[h].v;
    }
    /* table neighbourhood full (> WELD_PROBE colliding words in one batch): integer is the only
     * value guaranteed to match other uses of this word */
    g_pgxp_stat_weld_full++;
    return 0;
}

void pgxp_check_tri(const PgxpVert *const q[3], const u32 sxy[3])
{
    {   /* an SXY word sent at two different positions within one batch = a crack */
        static struct { u32 sxy, batch; float dx, dy, w; bool prec; } tab[1 << 16];
        for (int i = 0; i < 3; i++) {
            u32 h = pgxp_recent_slot(sxy[i]) ^ 0x5555;   /* not the weld table's layout */
            for (int k = 0; k < 32; k++, h = (h + 1) & 0xFFFF) {
                if (tab[h].batch != g_pgxp_batch) {
                    tab[h].batch = g_pgxp_batch; tab[h].sxy = sxy[i]; tab[h].prec = q[i] != 0;
                    tab[h].dx = q[i] ? q[i]->dx : 0; tab[h].dy = q[i] ? q[i]->dy : 0;
                    tab[h].w = q[i] ? q[i]->w : 0;
                    break;
                }
                if (tab[h].sxy != sxy[i]) continue;
                if (q[i] && tab[h].prec && !same_depth(q[i]->w, tab[h].w)) continue;   /* another mesh */
                if ((q[i] != 0) != tab[h].prec || (q[i] && (q[i]->dx != tab[h].dx || q[i]->dy != tab[h].dy))) {
                    g_pgxp_stat_split++;
                    if (g_pgxp_diag > 0) {
                        g_pgxp_diag--;
                        fprintf(stderr, "pgxp-diag f%u: word %d,%d sent at two positions in one batch\n",
                                g_pgxp_frame, (s16)(sxy[i] & 0xFFFF), (s16)(sxy[i] >> 16));
                    }
                }
                break;
            }
        }
    }
    float wmin = 1e30f, wmax = 0, dmax = 0;
    u32 age = 0, site0 = 0;
    int n = 0;
    bool mixed = false;   /* precise vertices from different GTE call sites */
    for (int i = 0; i < 3; i++) {
        if (!q[i]) continue;
        if (n++ == 0) site0 = q[i]->site;
        else if (q[i]->site != site0) mixed = true;
        /* on-screen only: far from the projection centre the UNR divide legitimately rounds
         * up to ~1.5 px differently (seen at x = -597 / 745) */
        s16 x = (s16)(sxy[i] & 0xFFFF), y = (s16)(sxy[i] >> 16);
        bool onscreen = x >= 0 && x < 512 && y >= 0 && y < 256;
        float d = onscreen ? fmaxf(fabsf(q[i]->dx), fabsf(q[i]->dy)) : 0;
        if (d > dmax) dmax = d;
        if (q[i]->w < wmin) wmin = q[i]->w;
        if (q[i]->w > wmax) wmax = q[i]->w;
        u32 a = g_pgxp_frame - q[i]->frame;
        if (a > age) age = a;
    }
    if (!n) return;
    g_pgxp_stat_tris++;
    bool bd = dmax > 1.5f, bw = mixed && wmax > 2.0f * wmin, bo = age > 2;
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
