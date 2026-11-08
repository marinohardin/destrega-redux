/* Smoothness as the player sees it (windowed runs): presented-frame intervals, frames held on
 * screen for more than one vblank and what held them (a netplay stall, a rollback's re-simulation,
 * a time-sync slowdown, or the host simply being too slow), audio underruns, and the netplay events
 * behind them. A summary is printed at exit ("smooth:" line + "smooth-json:" for scripts,
 * tools/netplay_smooth.py). Presentation only: nothing here touches emulated state.
 *
 * Counting starts at frame --smooth-from F (default 1); the frames of a netplay session started
 * from the menu are those of the new boot. Headless runs present nothing: only the netplay part is
 * counted there. */
#ifndef SMOOTH_H
#define SMOOTH_H
#include <stdbool.h>
#include <stdint.h>

/* main.c, windowed, once per presented frame: the host time the present returned, the host time
 * spent on this frame before sleeping (emulation + netplay + present), and the frame number. */
void smooth_present(uint64_t now_ns, uint64_t work_ns, uint64_t frame);
/* main.c, windowed: audio bytes still queued when this frame's audio is added; `repeat` when the
 * frame had to re-queue samples it already played (queue ran low with deterministic audio). */
void smooth_audio(uint32_t queued_bytes, uint32_t frame_bytes, bool repeat);

/* netplay.c */
void smooth_stall(uint64_t ns);                 /* waited for the peer's input */
void smooth_sync(uint64_t ns);                  /* time-sync slowdown applied this frame */
void smooth_rollback(uint64_t depth, uint64_t resim_ns, uint64_t shown_wrong);
uint64_t smooth_from(void);

int smooth_parse_arg(int argc, char **argv, int i);
void smooth_report(void);

/* Recent local load: average share of the 16.7 ms frame budget this host used over the last
 * ~2 s of presented frames (windowed), 0 if unknown. Netplay sends it to the peer. */
double smooth_load(void);

/* Scale governor (presentation only, cannot affect sync): while a netplay session runs
 * (netplay.c calls smooth_governor(true/false)), a host whose average frame time stays above 90%
 * of the budget lowers its hi-res render scale one step (4x6 -> 4x4 -> 3x4 -> ... -> 1x1) every
 * 2.5 s until it keeps up, so a slow peer stops dragging the other one; the caps are lifted when
 * the session ends. smooth_scale_cap() returns true once after each change, with the new caps
 * (0 = none) for main.c's auto scale. --no-scale-governor turns it off; a forced --scale is never
 * changed. */
void smooth_governor(bool on);

/* netplay.c: time-sync slowdown (ns) for the windowed frontend to add to this frame's pacing */
uint64_t netplay_pace_extra_ns(void);
bool smooth_scale_cap(int *sx, int *sy);
#endif
