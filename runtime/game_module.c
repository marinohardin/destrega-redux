/* Glue of a game module (game_abi.h): compiled into the module together with gen/, with
 * game_module_side.h forced in front. Defines the global pointers and the forwarding functions
 * the generated code links against, and the module's single exported entry point. */
#include "game_abi.h"
#include "gen_protos.h"
#include "game_sdk.h"   /* GAME_ABI_SDK_ID (tools/embed_files.py) */

#ifndef DESTREGA_MODULE_SIDE
#error "compile game modules with -include game_module_side.h"
#endif

/* the pointers behind the game_module_side.h macros */
#define GM_DEF(n) __typeof__(gm_##n) gm_##n;
GAME_ABI_VARS(GM_DEF)
#undef GM_DEF

static GameImports s_imp;

#define GM_FN(r, n, p, a) r n p { return s_imp.fn_##n a; }
GAME_ABI_FUNCS(GM_FN)
#undef GM_FN
#define GM_VFN(n, p, a) void n p { s_imp.fn_##n a; }
GAME_ABI_VFUNCS(GM_VFN)
#undef GM_VFN

extern const RecompEntry g_recomp_table[];
extern const u32 g_recomp_count;
extern const OverlayInfo g_overlays[];
extern const u32 g_overlay_count;

static GameExports s_exp;

#if defined(_WIN32)
#define GM_EXPORT __declspec(dllexport)
#else
#define GM_EXPORT __attribute__((visibility("default")))
#endif

GM_EXPORT const GameExports *destrega_game_module_init(const GameImports *imp)
{
    if (!imp || imp->abi != GAME_ABI_VERSION || imp->size != sizeof(GameImports) || imp->layout != game_abi_layout())
        return NULL;
    s_imp = *imp;
#define GM_SET(n) gm_##n = imp->v_##n;
    GAME_ABI_VARS(GM_SET)
#undef GM_SET
    s_exp.abi = GAME_ABI_VERSION;
    s_exp.size = sizeof(GameExports);
    s_exp.layout = game_abi_layout();
    s_exp.sdk_id = GAME_ABI_SDK_ID;
    s_exp.table = g_recomp_table;
    s_exp.count = g_recomp_count;
    s_exp.overlays = g_overlays;
    s_exp.overlay_count = g_overlay_count;
#define GM_GEN(n) s_exp.fn_##n = n;
    GAME_ABI_GEN(GM_GEN)
#undef GM_GEN
    return &s_exp;
}
