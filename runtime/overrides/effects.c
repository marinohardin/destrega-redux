/* The effect builders 80055820 .. 80056b40 (analysis/NOTES.md "Effects"): the originals unchanged;
 * the HLR's effect capture (runtime/hlr/hlr_fx.c, hlr_hook.h fx_pre / fx_post) reads what they
 * wrote. Nothing here changes emulated state: with the HLR off the hooks are NULL. */
#include "override.h"
#include "hlr_hook.h"
#include "game_syms.h"

#define FX_WRAP(addr) \
void f_##addr(void) \
{ \
    if (!g_hlr.fx_pre) { f_##addr##_orig(); return; } \
    g_hlr.fx_pre(0x##addr##u, g_cpu.r[4], g_cpu.r[5]); \
    f_##addr##_orig(); \
    if (g_hlr.fx_post) g_hlr.fx_post(); \
}

FX_WRAP(80055820)
FX_WRAP(800559b0)
FX_WRAP(80055b70)
FX_WRAP(80055d48)
FX_WRAP(80055f54)
FX_WRAP(80056348)
FX_WRAP(8005650c)
FX_WRAP(80056704)
FX_WRAP(80056908)
FX_WRAP(80056b40)
