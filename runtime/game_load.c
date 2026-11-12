/* Loading a game module (DESTREGA_GAME_MODULE builds; game_abi.h). The module holds the
 * recompiled game code built on this machine by the launcher (runtime/launcher/) or by CMake
 * (destrega_game, development). Only compiled into module builds. */
#ifdef DESTREGA_GAME_MODULE
/* system headers first: recomp.h's register macros (R8 ...) would rename winnt.h's fields */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "game_load.h"
#include "game_abi.h"
#include "game_sdk.h"

const RecompEntry *g_recomp_table;
u32 g_recomp_count;
const OverlayInfo *g_overlays;
u32 g_overlay_count;
GameExports g_game;

static char s_err[1024];

static void *open_lib(const char *path)
{
#ifdef _WIN32
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 4096)) return NULL;
    return (void *)LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void *find_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
static void close_lib(void *lib)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}
static const char *lib_error(void)
{
#ifdef _WIN32
    static char buf[64];
    snprintf(buf, sizeof buf, "Windows error %lu", (unsigned long)GetLastError());
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "unknown error";
#endif
}

bool game_module_loaded(void) { return g_recomp_table != NULL; }

const char *game_module_load(const char *path)
{
    if (g_recomp_table) return NULL;
    void *lib = open_lib(path);
    if (!lib) {
        snprintf(s_err, sizeof s_err, "could not load the game module %s (%s)", path, lib_error());
        return s_err;
    }
    GameModuleInit init;
    void *sym = find_sym(lib, GAME_MODULE_ENTRY);
    memcpy(&init, &sym, sizeof init);
    if (!init) {
        close_lib(lib);
        snprintf(s_err, sizeof s_err, "%s is not a Destrega game module", path);
        return s_err;
    }
    GameImports imp;
    memset(&imp, 0, sizeof imp);
    imp.abi = GAME_ABI_VERSION;
    imp.size = sizeof imp;
    imp.layout = game_abi_layout();
#define GL_VAR(n) imp.v_##n = (void *)&n;
    GAME_ABI_VARS(GL_VAR)
#undef GL_VAR
#define GL_FN(r, n, p, a) imp.fn_##n = n;
    GAME_ABI_FUNCS(GL_FN)
#undef GL_FN
#define GL_VFN(n, p, a) imp.fn_##n = n;
    GAME_ABI_VFUNCS(GL_VFN)
#undef GL_VFN
    const GameExports *e = init(&imp);
    if (!e || e->abi != GAME_ABI_VERSION || e->size != sizeof(GameExports) || e->layout != game_abi_layout()) {
        close_lib(lib);
        snprintf(s_err, sizeof s_err,
                 "the game module %s was built for another version of Destrega (interface %u, this program "
                 "needs %u); it has to be built again",
                 path, e ? e->abi : 0u, GAME_ABI_VERSION);
        return s_err;
    }
    if (!e->sdk_id || strcmp(e->sdk_id, GAME_ABI_SDK_ID)) {
        close_lib(lib);
        snprintf(s_err, sizeof s_err,
                 "the game module %s was built from other runtime headers (%.12s, this program has %.12s); it has "
                 "to be built again",
                 path, e->sdk_id ? e->sdk_id : "none", GAME_ABI_SDK_ID);
        return s_err;
    }
    if (!e->count || !e->table) {
        close_lib(lib);
        snprintf(s_err, sizeof s_err, "the game module %s holds no game code", path);
        return s_err;
    }
    g_game = *e;
    g_recomp_table = e->table;
    g_recomp_count = e->count;
    g_overlays = e->overlays;
    g_overlay_count = e->overlay_count;
    return NULL;   /* the library stays loaded until exit */
}
#endif
