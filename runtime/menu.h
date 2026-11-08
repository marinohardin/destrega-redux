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

/* Netplay progress panel ("Waiting for an opponent..." with Cancel); NULL hides it. No-op
 * without menus (headless, other platforms). */
void menu_netplay_progress(const char *text);
/* Test hook (--menu-action F:menu-test=CMD, windowed): "dump" logs every menu item with its
 * greyed / checked state; "click=Menu/Submenu/Item" chooses that item as a click would.
 * Windows only (menu_win.c); elsewhere it logs and returns false. */
bool menu_test(const char *cmd);

/* ---- provided by netplay.c / net.c (Netplay menu) ---- */
const char *netplay_status_text(void);
bool netplay_busy(void);          /* a session or a host / join attempt is running */
bool netplay_active(void);        /* a session is running */
int net_local_addresses(char *out, size_t n);

/* ---- provided by main.c ---- */
/* Queue action NAME[, ARG] (copied). Safe to call from menu callbacks. Names: see main.c. */
void frontend_menu_action(const char *name, const char *arg);
/* False for actions that are refused right now (state-changing actions during netplay). */
bool frontend_menu_action_allowed(const char *name);
/* Current hi-res renderer ("software", "gpu", "compare") and whether "gpu" can be chosen. */
const char *frontend_renderer(void);
bool frontend_gpu_available(void);
/* Check mark of a View-menu render-quality item ("tex-filter=bilinear", "smooth-colours",
 * "ssaa=2"): true and *checked set if `action` is one of them. */
bool frontend_view_checked(const char *action, bool *checked);
/* Whether the window is full screen (View > Enter/Exit Full Screen follows it). */
bool frontend_fullscreen(void);
#endif
