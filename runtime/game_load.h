/* Game module loading (module builds; game_abi.h, game_load.c). */
#ifndef GAME_LOAD_H
#define GAME_LOAD_H
#include <stdbool.h>
/* Loads the module at `path` (UTF-8) and installs its function table. NULL on success, else a
 * message for the player. Loading twice is a no-op. */
const char *game_module_load(const char *path);
bool game_module_loaded(void);
#endif
