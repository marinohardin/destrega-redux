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
/* Is a memory card inserted in slot 0/1? (menu titles: Insert vs Eject) */
bool frontend_card_inserted(int slot);

#endif
