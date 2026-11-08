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

/* ---- provided by main.c ---- */
/* Queue action NAME[, ARG] (copied). Safe to call from menu callbacks. Names: see main.c. */
void frontend_menu_action(const char *name, const char *arg);
/* False for actions that are refused right now (state-changing actions during netplay). */
bool frontend_menu_action_allowed(const char *name);
/* Current hi-res renderer ("software", "gpu", "compare") and whether "gpu" can be chosen. */
const char *frontend_renderer(void);
bool frontend_gpu_available(void);
/* Whether the window is full screen (View > Enter/Exit Full Screen follows it). */
bool frontend_fullscreen(void);
#endif
