/* In-window menu bar drawn by the frontend over the game picture (Linux's menus; macOS and Windows
 * with --overlay-menu on). Presentation only, like the native menus: every item queues the same
 * named action (frontend_menu_action), which main.c runs between frames.
 *
 * What the player sees: the bar (File, View, Netplay) slides in when the mouse is at the top of the
 * window; Alt (pressed and released alone), F10, a right-click or a controller's Guide button open
 * it from the keyboard / controller. While a menu or form is open the game is paused (main.c keeps
 * presenting the last picture and gives every key, mouse and controller event to the overlay), as
 * the Mac pauses while a menu is tracked. Forms: Host Game, Join Game, Controls; the netplay
 * waiting panel (non-modal) with Cancel. Text: SDL's built-in 8x8 debug font, scaled by whole
 * pixels; rectangles and lines from SDL_Renderer. No other code or data.
 *
 * Items come from one table (menu_overlay.c, k_items): a path ("View/Renderer/Software"), the
 * action and an optional shortcut label. Adding an item = adding a row. */
#ifndef MENU_OVERLAY_H
#define MENU_OVERLAY_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* the frontend's event handler (main.c): synthetic test events go through it, exactly as
     * SDL's own would (overlay first, then the game); NULL: the overlay only */
    void (*route)(SDL_Event *e);
    /* write the current picture plus the overlay to a PNG (test hook "shot=PATH") */
    void (*shot)(const char *path);
    bool headless;
    int canvas_w, canvas_h;   /* headless layout size (the --window size) */
} OverlayHooks;

/* win NULL = headless: only the test hooks and screenshots (overlay_menu_draw on a software
 * renderer) work. */
void overlay_menu_init(SDL_Window *win, const OverlayHooks *hooks);
bool overlay_menu_enabled(void);   /* a window has the overlay (drawn, takes events) */
/* An event from the frontend: true if the overlay used it (the game must not see it). While a
 * menu or form is open (overlay_menu_modal) every keyboard, mouse and controller event is used. */
bool overlay_menu_event(const SDL_Event *e);
bool overlay_menu_modal(void);     /* a menu or form is open: the game is paused */
void overlay_menu_close(void);
/* Draw whatever is visible (bar, menus, form, netplay panel, hint) over a w x h pixel picture. */
void overlay_menu_draw(SDL_Renderer *r, int w, int h);
/* hold=MS test hook (windowed): true once the time is up and the overlay was closed for it */
bool overlay_menu_hold_expired(void);
/* View > Controls... / the "controls" action */
void overlay_menu_controls(void);
/* Netplay waiting panel ("Waiting for an opponent..." + Cancel); NULL hides it. */
void overlay_menu_progress(const char *text);
/* Test hook --menu-action F:menu-test=CMD[;CMD...] (headless on every platform, windowed with the
 * overlay); commands in menu_overlay.c. */
bool overlay_menu_test(const char *cmds);
#endif
