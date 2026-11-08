/* Widescreen (16:9 fights) control. See AGENTS.md "Widescreen".
 * Nothing here changes emulated state: the widened stage renderer runs only as a sandboxed
 * shadow pass (runtime/overrides/widescreen.c) and the extra columns exist only in the hi-res
 * target's guard band. */
#pragma once
#include <stdbool.h>

/* ---- frontend (runtime/main.c) ---- */
/* Switch 16:9 fights on/off at runtime (menu item, hotkey F9). Takes effect at the next vblank,
 * between frames. The first switch-on allocates the hi-res guard band (a hi-res reconfigure,
 * like a window resize); switching off keeps it allocated and only stops the shadow pass and
 * the wide presentation. --widescreen sets the startup state. */
void frontend_set_widescreen(bool on);
bool frontend_widescreen(void);         /* requested state (pending switches included) */

/* ---- shadow pass (runtime/overrides/widescreen.c) ---- */
extern int g_widescreen;                /* shadow pass enabled (set by the frontend) */
void widescreen_enable(bool on);        /* frontend: set g_widescreen, drop pending captures */
void widescreen_report(void);

/* Host-side state that the recompiled code mutates besides guest RAM/scratchpad/CPU/GTE (for
 * example PGXP's per-register / per-RAM-word precision tags) must be snapshotted around the
 * shadow pass too, or the wide pass would leak into the real one. save() runs right after the
 * guest snapshot, before the wide pass; restore() runs after it, before the real pass. Both
 * run on the emulation thread, once per shadow pass. Up to 8 hooks. */
void widescreen_add_state_hook(void (*save)(void), void (*restore)(void));
