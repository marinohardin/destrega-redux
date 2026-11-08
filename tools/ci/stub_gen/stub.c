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
    fprintf(stderr, "FATAL: %s: built with the CI stub instead of the recompiled game\n", f);
    exit(3);
}
void f_80044210_orig(void) { no_game(__func__); }
void f_800465a4_orig(void) { no_game(__func__); }
void f_80044918(void) { no_game(__func__); }
void f_80045414(void) { no_game(__func__); }
void f_80095188(void) { no_game(__func__); }

/* Marker that tools/package_release.sh looks for (grep -a) to tell a stub build from a real
 * one, so a stub can never be packaged under a playable name. */
__attribute__((used)) const char g_destrega_stub_marker[] = "DESTREGA-CI-STUB-BUILD";

const RecompEntry g_recomp_table[1] = { { 0, NULL, 0, 0, 0, 0 } };
const u32 g_recomp_count = 0;
