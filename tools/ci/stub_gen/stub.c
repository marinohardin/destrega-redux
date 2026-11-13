/* CI stand-in for the generated code (DESTREGA_STUB_GEN): an empty function table and the
 * few recompiled functions the runtime calls directly. It only lets the runtime link and
 * start without the disc, so CI can check that the executable builds, loads its DLLs and
 * parses its options; it cannot run the game. */
#include "psx.h"
#include "gen_protos.h"
#include <stdio.h>
#include <stdlib.h>

static void no_game(const char *f)
{
#ifdef DESTREGA_MODULE_SIDE
    /* as a module (launcher self-test) it is only loaded, never run; and libc's data symbols
     * (stderr) cannot be reached under the module's hidden visibility on macOS */
    (void)f;
    __builtin_trap();
#else
    fprintf(stderr, "FATAL: %s: built with the CI stub instead of the recompiled game\n", f);
    exit(3);
#endif
}
void f_80036138_orig(void) { no_game(__func__); }
void f_800363c8_orig(void) { no_game(__func__); }
void f_80036b0c_orig(void) { no_game(__func__); }
void f_80044210_orig(void) { no_game(__func__); }
void f_800465a4_orig(void) { no_game(__func__); }
void f_8007ec5c_orig(void) { no_game(__func__); }
void f_80055820_orig(void) { no_game(__func__); }
void f_800559b0_orig(void) { no_game(__func__); }
void f_80055b70_orig(void) { no_game(__func__); }
void f_80055d48_orig(void) { no_game(__func__); }
void f_80055f54_orig(void) { no_game(__func__); }
void f_80056348_orig(void) { no_game(__func__); }
void f_8005650c_orig(void) { no_game(__func__); }
void f_80056704_orig(void) { no_game(__func__); }
void f_80056908_orig(void) { no_game(__func__); }
void f_80056b40_orig(void) { no_game(__func__); }
void f_80044918(void) { no_game(__func__); }
void f_80045414(void) { no_game(__func__); }
void f_80095188(void) { no_game(__func__); }

/* Marker that tools/package_release.sh looks for (grep -a) to tell a stub build from a real
 * one, so a stub can never be packaged under a playable name. */
__attribute__((used)) const char g_destrega_stub_marker[] = "DESTREGA-CI-STUB-BUILD";

#ifdef DESTREGA_MODULE_SIDE
/* compiled as a game module by the launcher's self-test (--game-selftest, CI): one entry, so the
 * runtime's "module holds no game code" check passes, and the overlay table a module exports */
static void stub_entry(void) { no_game(__func__); }
const RecompEntry g_recomp_table[1] = { { 0x80010000u, stub_entry, 0, 0x80010000u, 0x80010004u, 0 } };
const u32 g_recomp_count = 1;
const OverlayInfo g_overlays[1] = { { "", 0, 0 } };
const u32 g_overlay_count = 0;
#else
const RecompEntry g_recomp_table[1] = { { 0, NULL, 0, 0, 0, 0 } };
const u32 g_recomp_count = 0;
#endif
