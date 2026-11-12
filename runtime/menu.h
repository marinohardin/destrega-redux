/* Native application menus (File menu). The platform half lives in menu_mac.m (Apple) or
 * menu_stub.c (everything else: no-ops). Menu handlers only *queue* a named action
 * (frontend_menu_action); main.c runs it on the main thread between frames, the same way the
 * headless `--menu-action` test hook does, so menus never touch the emulated machine. */
#ifndef MENU_H
#define MENU_H
#include <stdbool.h>
#include <stddef.h>

/* Build the menus. Windowed mode only, after SDL_Init(VIDEO). */
void menu_init(void);
/* Open `path` in the file manager: a folder is opened, a file is selected in its folder. */
void menu_reveal(const char *path);
/* Absolute path of the running executable (for the clean re-exec after loading a game image). */
bool menu_executable_path(char *out, size_t n);

/* Netplay progress panel ("Waiting for an opponent..." with Cancel); NULL hides it. `copy` adds a
 * "Copy Address" button (queues netplay-copy-address). No-op without menus (headless, other
 * platforms). The text comes from netplay_ui.c. */
void menu_netplay_progress(const char *text, bool copy);
/* Test hook (--menu-action F:menu-test=CMD, windowed): "dump" logs every menu item with its
 * greyed / checked state; "click=Menu/Submenu/Item" chooses that item as a click would.
 * Windows only (menu_win.c); elsewhere it logs and returns false. */
bool menu_test(const char *cmd);
/* Wait until SDL_GetTicksNS() >= deadline_ns while keeping the native event loop running
 * (macOS: a macOS full-screen Space draws the menu bar inside this process, so a plain sleep for
 * most of every frame made revealing and using the menus lag). Events that arrive are handed to
 * SDL_PumpEvents at once; SDL queues its own events for the next frame's poll as before.
 * Returns false where there is nothing to run (no menus, other platforms): sleep instead. */
bool menu_wait_until(unsigned long long deadline_ns);

/* ---- provided by netplay.c / netplay_ui.c / net.c (Netplay menu) ---- */
const char *netplay_status_text(void);
const char *netplay_ui_copy_title(void);   /* "Copy My Address" / "Copy My Home-Network Address" */
bool netplay_busy(void);          /* a session or a host / join attempt is running */
bool netplay_active(void);        /* a session is running */
int net_local_addresses(char *out, size_t n);

/* ---- provided by main.c ---- */
/* Queue action NAME[, ARG] (copied). Safe to call from menu callbacks. Names: see main.c. */
void frontend_menu_action(const char *name, const char *arg);
/* False for actions that are refused right now (state-changing actions during netplay). */
bool frontend_menu_action_allowed(const char *name);
/* Current hi-res renderer: "software", "compare" or the GPU backend's graphics API ("metal",
 * "vulkan", "d3d12"), and whether `name` (one of those, or "gpu") can be chosen here. */
const char *frontend_renderer(void);
bool frontend_renderer_available(const char *name);
/* This platform's graphics APIs in order of preference (View > Renderer items): action names
 * ("metal", "vulkan", "d3d12") and titles ("Metal", "Vulkan", "Direct3D 12"). Returns the count. */
int frontend_graphics_apis(const char **names, const char **titles, int max);
/* Check mark of a View-menu render-quality item ("tex-filter=bilinear", "smooth-colours",
 * "ssaa=2"): true and *checked set if `action` is one of them. */
bool frontend_view_checked(const char *action, bool *checked);
/* Whether the window is full screen (View > Enter/Exit Full Screen follows it). */
bool frontend_fullscreen(void);
#endif
