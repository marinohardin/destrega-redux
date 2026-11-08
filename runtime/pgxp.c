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

u32 pgxp_new(u32 sxy, float dx, float dy, float w)
{
    u32 t = s_next;
    s_next = (s_next + 1) & (PGXP_POOL_SIZE - 1);
    if (!s_next) s_next = 1;   /* tag 0 = no association */
    PgxpVert *v = &g_pgxp_pool[t];
    v->sxy = sxy; v->dx = dx; v->dy = dy; v->w = w;
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
