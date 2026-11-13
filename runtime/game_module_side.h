/* Forced first include (-include) of every file of a game module (game_abi.h): turns each
 * runtime global the generated code uses into a pointer that runtime/game_module.c fills from
 * GameImports at load. `extern Cpu g_cpu;` in psx.h then declares `Cpu (*gm_g_cpu)`, and
 * `g_ram + x` decays the pointed-to array as before, so gen/ compiles unchanged. Every symbol is
 * hidden: the module exports only GAME_MODULE_ENTRY and resolves nothing at load. */
#ifndef GAME_MODULE_SIDE_H
#define GAME_MODULE_SIDE_H
#define DESTREGA_MODULE_SIDE 1
#if !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif
#define g_cpu (*gm_g_cpu)
#define g_ram (*gm_g_ram)
#define g_scratch (*gm_g_scratch)
#define g_mem_wlimit (*gm_g_mem_wlimit)
#define g_callstack (*gm_g_callstack)
#define g_depth (*gm_g_depth)
#define g_widescreen_cull (*gm_g_widescreen_cull)
#define g_ws_cull_hits (*gm_g_ws_cull_hits)
#define g_idle_skip (*gm_g_idle_skip)
#define g_idle_skipped (*gm_g_idle_skipped)
#define g_pgxp (*gm_g_pgxp)
#define g_pgxp_reg (*gm_g_pgxp_reg)
#define g_pgxp_mem (*gm_g_pgxp_mem)
#define g_pgxp_gte (*gm_g_pgxp_gte)
#endif
