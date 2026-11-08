/* Platforms without native menus (Linux, other): no-ops. macOS has menu_mac.m, Windows
 * menu_win.c. (SDL's own dialogs are used by main.c where needed.) */
#if !defined(__APPLE__) && !defined(_WIN32)
#include "menu.h"
#include "platform.h"
#include <stdio.h>

void menu_init(void) {}
void menu_netplay_progress(const char *text) { (void)text; }
void menu_reveal(const char *path) { (void)path; }
bool menu_executable_path(char *out, size_t n) { return plat_executable_path(out, n); }
bool menu_test(const char *cmd) { fprintf(stderr, "menu: test %s: no native menus on this platform\n", cmd); return false; }
#endif
