/* The generated functions the runtime calls by name (game_abi.h GAME_ABI_GEN). Static build:
 * gen_protos.h. Module build: the pointers the loaded game module exported (game_load.c). */
#ifndef GAME_SYMS_H
#define GAME_SYMS_H
#ifdef DESTREGA_GAME_MODULE
#include "game_abi.h"
extern GameExports g_game;
#define f_80036138_orig (g_game.fn_f_80036138_orig)
#define f_800363c8_orig (g_game.fn_f_800363c8_orig)
#define f_80036b0c_orig (g_game.fn_f_80036b0c_orig)
#define f_80044210_orig (g_game.fn_f_80044210_orig)
#define f_800465a4_orig (g_game.fn_f_800465a4_orig)
#define f_8007ec5c_orig (g_game.fn_f_8007ec5c_orig)
#define f_80044918 (g_game.fn_f_80044918)
#define f_80045414 (g_game.fn_f_80045414)
#define f_80095188 (g_game.fn_f_80095188)
#else
#include "gen_protos.h"
#endif
#endif
