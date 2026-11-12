/* First-launch "build from your own disc" launcher (DESTREGA_GAME_MODULE builds).
 *
 * The executable holds no game code. Before the game boots, launcher_prepare() finds the game
 * module (game_abi.h) or builds it: the C recompiler (recompiler.c) turns SLUS_007.32 and the
 * overlays of the player's data pack into C, a C compiler (the bundled Zig, `zig cc`, or a
 * system clang/gcc) compiles it into <data dir>/game/destrega_game.<dylib|dll|so>, and the
 * module is loaded. Later launches load it directly while its key matches; the key hashes the
 * recompiler, its analysis inputs, the module SDK (runtime headers), the compiler and its
 * flags, the target and the disc files the code comes from. */
#ifndef LAUNCHER_H
#define LAUNCHER_H
#include <stdbool.h>

/* main.c's option loop: returns how many arguments at argv[i] were consumed (0: not ours). */
int launcher_parse_arg(int argc, char **argv, int i);
void launcher_usage(void);

/* After disc_verify(). data_dir ends with a path separator. sdl_window: the game window
 * (windowed runs) for the progress UI and messages, NULL headless. Returns true with the game
 * module loaded; false: *exit_code is set (0 = the player cancelled, else an error was shown /
 * printed), or the run was a --recompile request. */
bool launcher_prepare(const char *data_dir, bool headless, void *sdl_window, int *exit_code);

/* progress UI (launcher_ui_{mac,win,sdl}): a small window "Preparing the game for this
 * computer..." with a determinate bar and Cancel */
typedef struct LauncherUI LauncherUI;
LauncherUI *lui_open(void *sdl_window);
void lui_update(LauncherUI *ui, double frac, const char *step);
bool lui_cancelled(LauncherUI *ui);   /* also pumps events */
void lui_close(LauncherUI *ui);
#endif
