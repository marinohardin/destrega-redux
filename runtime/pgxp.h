/* PGXP-style vertex precision tracking (presentation only).
 *
 * The GTE returns integer screen coordinates; the guest stores them as packed
 * SXY words (y << 16 | x) that travel GTE register -> CPU register -> RAM ->
 * GP0 command stream. Rounding to integers is what makes PS1 geometry wobble.
 * This module keeps, next to each of those places, a *tag*: an index into a
 * pool of precise vertices {integer SXY word, sub-pixel offsets, depth}. A tag
 * is only trusted when the integer word it was made for still equals the word
 * that is being used (pgxp_lookup), so stale tags (a register reused by an ALU
 * op, a RAM word rewritten with sh/sb, a ring slot recycled) are harmless:
 * they simply fail validation and the integer vertex is used.
 *
 * Nothing here is ever read by the emulated machine: the GTE results, RAM and
 * VRAM the guest sees are bit-identical with tracking on or off. Only the hi-res
 * target (runtime/hires.c) consumes precise vertices.
 *
 * Tracked paths (anything else drops the association, i.e. falls back):
 *   RTPS/RTPT -> SXY FIFO (gte.c), MFC2/MTC2/LWC2/SWC2 of SXY0..SXYP, LW/SW,
 *   register moves (addu/or rd, rs, $zero; addiu rt, rs, 0), GP0 writes by DMA
 *   (block and linked-list), by an SW to the GP0 port and by BIOS GPU_cw.
 *   The hooks are emitted by recomp/codegen.py and mirrored in interp.c.
 */
#ifndef PGXP_H
#define PGXP_H

#include "psx.h"

typedef struct {
    u32 sxy;        /* the integer SXY word this vertex was produced as */
    float dx, dy;   /* precise screen position minus the integer SX / SY */
    float w;        /* view depth (the unclamped SZ), > 0 */
} PgxpVert;

#define PGXP_POOL_BITS 18
#define PGXP_POOL_SIZE (1u << PGXP_POOL_BITS)
#define PGXP_RAM_WORDS (RAM_SIZE / 4)
#define PGXP_IO_SLOT (PGXP_RAM_WORDS + 256)   /* any non-RAM/scratchpad word (GP0 port writes) */

extern bool g_pgxp;                          /* tracking enabled (--pgxp on) */
extern u32 g_pgxp_reg[32];                   /* tag per CPU register */
extern u32 g_pgxp_mem[PGXP_IO_SLOT + 1];     /* tag per RAM word, scratchpad word, I/O sink */
extern u32 g_pgxp_gte[3];                    /* tag per SXY0..SXY2 FIFO entry */
extern PgxpVert g_pgxp_pool[PGXP_POOL_SIZE];

static inline u32 pgxp_slot(u32 a)
{
    u32 p = a & 0x1fffffff;
    if (p < 0x800000) return (p & 0x1ffffc) >> 2;
    if ((p & 0xfffffc00) == 0x1f800000) return PGXP_RAM_WORDS + ((p & 0x3fc) >> 2);
    return PGXP_IO_SLOT;
}

/* ---- hooks used by generated code and the interpreter ---------------------- */
static inline void pgxp_lw(int rt, u32 a)
{
    if (g_pgxp) g_pgxp_reg[rt] = g_pgxp_mem[pgxp_slot(a)];
}
static inline void pgxp_sw(u32 a, int rt)
{
    if (g_pgxp) g_pgxp_mem[pgxp_slot(a)] = rt ? g_pgxp_reg[rt] : 0;
}
static inline void pgxp_mov(int rd, int rs)
{
    if (g_pgxp) g_pgxp_reg[rd] = rs ? g_pgxp_reg[rs] : 0;
}
static inline u32 pgxp_gte_tag(int reg)      /* tag of a GTE data register as read */
{
    if (reg >= 12 && reg <= 14) return g_pgxp_gte[reg - 12];
    if (reg == 15) return g_pgxp_gte[2];     /* SXYP reads as SXY2 */
    return 0;
}
static inline void pgxp_mfc2(int rt, int rd)
{
    if (g_pgxp) g_pgxp_reg[rt] = pgxp_gte_tag(rd);
}
static inline void pgxp_swc2(u32 a, int rd)
{
    if (g_pgxp) g_pgxp_mem[pgxp_slot(a)] = pgxp_gte_tag(rd);
}
/* After gte_write_data(reg, ...) (which shifts the tag FIFO on an SXYP write and
 * clears the written entry): attach the source's tag. */
static inline void pgxp_gte_written(int reg, u32 tag)
{
    if (!g_pgxp) return;
    if (reg >= 12 && reg <= 14) g_pgxp_gte[reg - 12] = tag;
    else if (reg == 15) g_pgxp_gte[2] = tag;
}
static inline void pgxp_mtc2(int rd, int rt) { pgxp_gte_written(rd, rt ? g_pgxp_reg[rt] : 0); }
static inline void pgxp_lwc2(int rd, u32 a) { pgxp_gte_written(rd, g_pgxp_mem[pgxp_slot(a)]); }

/* ---- producer / consumer --------------------------------------------------- */
/* New precise vertex for the integer word `sxy`; returns its tag (never 0). */
u32 pgxp_new(u32 sxy, float dx, float dy, float w);
/* Precise vertex for `tag` if it is still valid for the integer word `sxy`. */
static inline const PgxpVert *pgxp_lookup(u32 tag, u32 sxy)
{
    if (!tag) return 0;
    const PgxpVert *v = &g_pgxp_pool[tag & (PGXP_POOL_SIZE - 1)];
    return v->sxy == sxy ? v : 0;
}
void pgxp_reset(void);                       /* drop every association */

/* Diagnostics: polygon vertices sent to the hi-res target, and those with a valid precise
 * vertex (counted in gpu.c while hi-res and tracking are on). */
extern u64 g_pgxp_stat_vtx, g_pgxp_stat_hit, g_pgxp_stat_new;
/* Misses whose integer word equals a recent GTE result (a hash set of the last ~64K produced
 * words): an estimate of GTE vertices whose association was lost on the way (the rest are
 * mostly 2D primitives computed by the CPU), and misses that carried a stale tag. */
extern u64 g_pgxp_stat_miss_gte, g_pgxp_stat_miss_stale;
extern u32 g_pgxp_recent[1u << 16];
static inline u32 pgxp_recent_slot(u32 sxy) { return (sxy * 2654435761u) >> 16; }

#endif
