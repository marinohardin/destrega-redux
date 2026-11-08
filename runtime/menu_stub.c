/* Non-Apple platforms: no native menus. (SDL's own dialogs are used by main.c where needed.) */
#ifndef __APPLE__
#include "menu.h"
#include "platform.h"

void menu_init(void) {}
void menu_reveal(const char *path) { (void)path; }
bool menu_executable_path(char *out, size_t n) { return plat_executable_path(out, n); }
#endif
