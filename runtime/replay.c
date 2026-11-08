/* Input recording / replay (milestone 2). Hooks called by savestate.c. */
#include "savestate.h"
void replay_vblank_begin(u64 frame) { (void)frame; }
void replay_vblank_end(u64 frame) { (void)frame; }
int replay_parse_arg(int argc, char **argv, int i) { (void)argc; (void)argv; (void)i; return 0; }
void replay_usage(void) {}
void replay_finish(void) {}
