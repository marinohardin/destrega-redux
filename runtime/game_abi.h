/* The interface between the runtime and a game module (DESTREGA_GAME_MODULE, the launcher).
 *
 * In the static build the generated code (gen/) is linked into the executable and this file is
 * not used. In the module build gen/ is compiled into a shared library (built by the launcher on
 * the player's machine, or by CMake for development) that contains nothing but the recompiled
 * game code, its function table and runtime/game_module.c. The module imports every runtime
 * global and function the generated code touches through one GameImports struct and exports
 * the table and the few generated functions the runtime calls (overrides) through GameExports.
 * No symbol is resolved by the dynamic linker except the module's one entry point, so the same
 * scheme works for .dylib, .so and .dll alike.
 *
 * Versioning: GAME_ABI_VERSION changes whenever the lists or struct layouts below change;
 * game_abi_layout() hashes the sizes and offsets both sides depend on (it is computed by the
 * module's compiler and by the runtime's, so a module built with other headers or for another
 * target is refused); GAME_ABI_SDK_ID (build/game_sdk.h, tools/embed_files.py) is the hash of the
 * headers the module is compiled with (psx.h, recomp.h, pgxp.h, this file, game_module*.{c,h}): a
 * module compiled from other inline code (mem_r32, pgxp_lw, op_div ...) would emulate a different
 * machine. The runtime refuses a module unless all three match, with a plain message. */
#ifndef GAME_ABI_H
#define GAME_ABI_H

#include "recomp.h"

#define GAME_ABI_VERSION 1

/* Runtime globals the generated code reads or writes (recomp.h / psx.h / pgxp.h inlines). */
#define GAME_ABI_VARS(X)                                                                       \
    X(g_cpu) X(g_ram) X(g_scratch) X(g_mem_wlimit) X(g_callstack) X(g_depth)                    \
    X(g_widescreen_cull) X(g_ws_cull_hits) X(g_idle_skip) X(g_idle_skipped)                     \
    X(g_pgxp) X(g_pgxp_reg) X(g_pgxp_mem) X(g_pgxp_gte)

/* Runtime functions the generated code calls: X(return type, name, (params), (args)). */
#define GAME_ABI_FUNCS(X)                                                                      \
    X(u32, bus_read8, (u32 a), (a))                                                            \
    X(u32, bus_read16, (u32 a), (a))                                                           \
    X(u32, bus_read32, (u32 a), (a))                                                           \
    X(u32, gte_read_data, (int r), (r))                                                        \
    X(u32, gte_read_ctrl, (int r), (r))                                                        \
    X(u32, rt_mfc0, (int r), (r))                                                              \
    X(u32, pgxp_wslot_other, (u32 p), (p))
#define GAME_ABI_VFUNCS(X)                                                                     \
    X(bus_write8, (u32 a, u32 v), (a, v))                                                      \
    X(bus_write16, (u32 a, u32 v), (a, v))                                                     \
    X(bus_write32, (u32 a, u32 v), (a, v))                                                     \
    X(gte_command, (u32 i), (i))                                                               \
    X(gte_write_data, (int r, u32 v), (r, v))                                                  \
    X(gte_write_ctrl, (int r, u32 v), (r, v))                                                  \
    X(rt_call, (u32 a), (a))                                                                   \
    X(rt_jump, (u32 a), (a))                                                                   \
    X(rt_event, (void), ())                                                                    \
    X(rt_syscall, (u32 pc), (pc))                                                              \
    X(rt_break, (u32 pc, u32 code), (pc, code))                                                \
    X(rt_mtc0, (int r, u32 v), (r, v))                                                         \
    X(rt_rfe, (void), ())                                                                      \
    X(rt_invalid, (u32 pc, u32 w), (pc, w))                                                    \
    X(rt_unreached, (u32 pc), (pc))                                                            \
    /* hand-written overrides (analysis/overrides.txt): the generated code calls them */       \
    X(f_80036138, (void), ())                                                                  \
    X(f_800363c8, (void), ())                                                                  \
    X(f_80044210, (void), ())                                                                  \
    X(f_800465a4, (void), ())

/* Generated functions the runtime calls (the overrides' originals and their guest helpers;
 * the same list as tools/ci/stub_gen/stub.c). */
#define GAME_ABI_GEN(X)                                                                        \
    X(f_80036138_orig) X(f_800363c8_orig) X(f_80044210_orig) X(f_800465a4_orig)                 \
    X(f_80044918) X(f_80045414) X(f_80095188)

/* declared here too: a signature that differs from the runtime's own declaration is a compile
 * error on both sides (and declares the overrides for game_load.c) */
#define GA_DECL(r, n, p, a) r n p;
GAME_ABI_FUNCS(GA_DECL)
#undef GA_DECL
#define GA_VDECL(n, p, a) void n p;
GAME_ABI_VFUNCS(GA_VDECL)
#undef GA_VDECL

typedef struct GameImports {
    u32 abi, size;
    u64 layout;
#define GA_VAR(n) void *v_##n;
    GAME_ABI_VARS(GA_VAR)
#undef GA_VAR
#define GA_FN(r, n, p, a) r(*fn_##n) p;
    GAME_ABI_FUNCS(GA_FN)
#undef GA_FN
#define GA_VFN(n, p, a) void(*fn_##n) p;
    GAME_ABI_VFUNCS(GA_VFN)
#undef GA_VFN
} GameImports;

typedef struct GameExports {
    u32 abi, size;
    u64 layout;
    const char *sdk_id;            /* GAME_ABI_SDK_ID the module was compiled with */
    const RecompEntry *table;
    u32 count;
    const OverlayInfo *overlays;
    u32 overlay_count;
#define GA_GEN(n) RecompFn fn_##n;
    GAME_ABI_GEN(GA_GEN)
#undef GA_GEN
} GameExports;

/* The module's only exported symbol. Returns NULL if `imp` does not match what the module was
 * built for (abi, size or layout); the runtime then checks the returned exports the same way. */
#define GAME_MODULE_ENTRY "destrega_game_module_init"
typedef const GameExports *(*GameModuleInit)(const GameImports *imp);

static inline u64 game_abi_mix(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ull; }
/* Everything the two sides must agree on beyond the declarations themselves. */
static inline u64 game_abi_layout(void)
{
    u64 h = 0xcbf29ce484222325ull;
    const u64 v[] = {
        GAME_ABI_VERSION, sizeof(void *), sizeof(long), sizeof(Cpu), offsetof(Cpu, hi),
        offsetof(Cpu, pc), offsetof(Cpu, cop0), offsetof(Cpu, cycles), offsetof(Cpu, next_event),
        RAM_SIZE, 1024, CYCLES_PER_INSN, sizeof(RecompEntry), offsetof(RecompEntry, fn),
        offsetof(RecompEntry, overlay), offsetof(RecompEntry, hash), sizeof(OverlayInfo),
        offsetof(OverlayInfo, load_addr), PGXP_IO_SLOT, PGXP_POOL_SIZE, sizeof(PgxpVert),
        sizeof(GameImports), sizeof(GameExports), sizeof(bool), sizeof(int),
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) h = game_abi_mix(h, v[i]);
    return h;
}

#endif
