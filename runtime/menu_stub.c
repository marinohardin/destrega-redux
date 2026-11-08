/* Non-Apple platforms: no native menus. (SDL's own dialogs are used by main.c where needed.) */
#ifndef __APPLE__
#include "menu.h"
#include <stdio.h>
#include <unistd.h>

void menu_init(void) {}
void menu_reveal(const char *path) { (void)path; }
bool menu_executable_path(char *out, size_t n) {
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) return false;
    out[k] = 0;
    return true;
}
#endif
