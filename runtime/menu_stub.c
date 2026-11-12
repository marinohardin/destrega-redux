/* Platforms without native menus (Linux, other): the menus are the in-window menu bar
 * (menu_overlay.c), which main.c turns on for every windowed run here. macOS has menu_mac.m,
 * Windows menu_win.c; there the overlay is only used with --overlay-menu on. (File pickers and
 * alerts are SDL's own dialogs in main.c on every platform.) */
#if !defined(__APPLE__) && !defined(_WIN32)
#include "menu.h"
#include "menu_overlay.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void rt_log(const char *fmt, ...);

void menu_init(void) {}
void menu_netplay_progress(const char *text, bool copy) { (void)copy; overlay_menu_progress(text); }

/* open the folder holding `path` (or `path` itself if it is a folder) in the file manager */
void menu_reveal(const char *path) {
    char dir[4096], url[3 * 4096 + 16];
    snprintf(dir, sizeof dir, "%s", path);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = 0;
    }
    char *o = url;
    o += sprintf(o, "file://");
    for (const unsigned char *s = (const unsigned char *)dir; *s && o < url + sizeof url - 4; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("/-_.~", *s)) *o++ = (char)*s;
        else o += sprintf(o, "%%%02X", *s);
    }
    *o = 0;
    if (!SDL_OpenURL(url)) rt_log("menu: ERROR: could not open %s: %s", dir, SDL_GetError());
    else rt_log("menu: opened %s", dir);
}

bool menu_executable_path(char *out, size_t n) { return plat_executable_path(out, n); }
void controls_open(void) { overlay_menu_controls(); }
bool menu_test(const char *cmd) { return overlay_menu_test(cmd); }
bool menu_wait_until(unsigned long long deadline_ns) { (void)deadline_ns; return false; }
#endif
