/* Support macros for generated code (gen/). */
#ifndef RECOMP_H
#define RECOMP_H

#include "psx.h"

#define R1 g_cpu.r[1]
#define R2 g_cpu.r[2]
#define R3 g_cpu.r[3]
#define R4 g_cpu.r[4]
#define R5 g_cpu.r[5]
#define R6 g_cpu.r[6]
#define R7 g_cpu.r[7]
#define R8 g_cpu.r[8]
#define R9 g_cpu.r[9]
#define R10 g_cpu.r[10]
#define R11 g_cpu.r[11]
#define R12 g_cpu.r[12]
#define R13 g_cpu.r[13]
#define R14 g_cpu.r[14]
#define R15 g_cpu.r[15]
#define R16 g_cpu.r[16]
#define R17 g_cpu.r[17]
#define R18 g_cpu.r[18]
#define R19 g_cpu.r[19]
#define R20 g_cpu.r[20]
#define R21 g_cpu.r[21]
#define R22 g_cpu.r[22]
#define R23 g_cpu.r[23]
#define R24 g_cpu.r[24]
#define R25 g_cpu.r[25]
#define R26 g_cpu.r[26]
#define R27 g_cpu.r[27]
#define R28 g_cpu.r[28]
#define R29 g_cpu.r[29]
#define R30 g_cpu.r[30]
#define R31 g_cpu.r[31]
#define HI g_cpu.hi
#define LO g_cpu.lo

/* Approximate CPU cost per instruction (cache misses, memory stalls). */
#ifndef CYCLES_PER_INSN
#define CYCLES_PER_INSN 2
#endif
#define CYC(n) (g_cpu.cycles += (u64)(n) * CYCLES_PER_INSN)
#define CYC_FLUSH() ((void)0)
/* Shadow call stack of guest function entries (diagnostics). */
extern u32 g_callstack[1024];
extern u32 g_depth;
#define FN_ENTER(a) (g_callstack[g_depth++ & 1023] = (a))
#define FN_RET() do { g_depth--; return; } while (0)
/* Widescreen culling patch points (analysis/widescreen_cull.txt): the original immediate
 * unless --widescreen widened the stage renderer's screen-space culling. g_ws_cull_hits
 * counts evaluations of a widened bound (the stage was drawn), used to pick 16:9 vs 4:3. */
extern int g_widescreen_cull;
extern u32 g_ws_cull_hits;
#define WS_IMM(orig, wide) (__builtin_expect(g_widescreen_cull, 0) ? (g_ws_cull_hits++, (wide)) : (orig))
#define EVT_CHECK() do { if (__builtin_expect(g_cpu.cycles >= g_cpu.next_event, 0)) rt_event(); } while (0)

static inline void op_div(u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) {
        g_cpu.hi = a;
        g_cpu.lo = n >= 0 ? 0xffffffffu : 1u;
    } else if (n == (s32)0x80000000 && d == -1) {
        g_cpu.hi = 0;
        g_cpu.lo = 0x80000000u;
    } else {
        g_cpu.lo = (u32)(n / d);
        g_cpu.hi = (u32)(n % d);
    }
}

static inline void op_divu(u32 a, u32 b) {
    if (b == 0) {
        g_cpu.hi = a;
        g_cpu.lo = 0xffffffffu;
    } else {
        g_cpu.lo = a / b;
        g_cpu.hi = a % b;
    }
}

static inline u32 op_lwl(u32 cur, u32 addr) {
    u32 w = mem_r32(addr & ~3u);
    switch (addr & 3) {
    case 0: return (cur & 0x00ffffffu) | (w << 24);
    case 1: return (cur & 0x0000ffffu) | (w << 16);
    case 2: return (cur & 0x000000ffu) | (w << 8);
    default: return w;
    }
}

static inline u32 op_lwr(u32 cur, u32 addr) {
    u32 w = mem_r32(addr & ~3u);
    switch (addr & 3) {
    case 0: return w;
    case 1: return (cur & 0xff000000u) | (w >> 8);
    case 2: return (cur & 0xffff0000u) | (w >> 16);
    default: return (cur & 0xffffff00u) | (w >> 24);
    }
}

static inline void op_swl(u32 v, u32 addr) {
    u32 a = addr & ~3u, w = mem_r32(a);
    switch (addr & 3) {
    case 0: w = (w & 0xffffff00u) | (v >> 24); break;
    case 1: w = (w & 0xffff0000u) | (v >> 16); break;
    case 2: w = (w & 0xff000000u) | (v >> 8); break;
    default: w = v; break;
    }
    mem_w32(a, w);
}

static inline void op_swr(u32 v, u32 addr) {
    u32 a = addr & ~3u, w = mem_r32(a);
    switch (addr & 3) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000ffu) | (v << 8); break;
    case 2: w = (w & 0x0000ffffu) | (v << 16); break;
    default: w = (w & 0x00ffffffu) | (v << 24); break;
    }
    mem_w32(a, w);
}

#endif
