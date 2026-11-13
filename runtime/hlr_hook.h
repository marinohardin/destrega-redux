/* Hooks of the native high-level renderer (docs/HLR_PLAN.md 2.7) into the covered runtime.
 *
 * The HLR itself (runtime/hlr/) is presentation-only and outside the netplay build identity;
 * the covered code (bus.c, savestate.c, overrides/widescreen.c) only calls through this table.
 * Every entry is NULL while the HLR is off, and none of them may change emulated state: they
 * read guest RAM / scratchpad, never write it, never call guest code, never touch the guest GTE,
 * GPU state, cycles or the scheduler. The table is defined in bus.c so that module unit tests link
 * without the HLR. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct HlrHooks {
    /* stage_render (80044210), around its real pass (overrides/widescreen.c); mode = its a0 */
    void (*stage_pre)(uint32_t mode);
    void (*stage_post)(void);
    /* true: the stage is drawn natively in 3D this pass, so stage_render skips its widescreen
     * shadow pass (the native camera covers the guard band) */
    bool (*stage_native)(void);
    /* char_mesh_render (80036b0c), around the original (overrides/char_mesh.c); mb = its a0 (the
     * model block), ot = its a1 (the scene ordering table) */
    void (*char_pre)(uint32_t mb, uint32_t ot);
    void (*char_post)(void);
    /* true: characters are drawn natively in 3D, so char_mesh.c adds no PGXP extra faces */
    bool (*chars_native)(void);
    /* GPU linked-list DMA (bus.c), per node (RAM offset of its header) before its words are
     * written, and once with 0xffffffff at the end of the walk. Returns true when the node's
     * words must reach the 1x GPU only (the HLR drew its own version into the hi-res target). */
    bool (*ll_filter)(uint32_t node);
    /* ss_restore (savestate.c) after a successful load: captures of the old timeline are dropped */
    void (*on_restore)(void);
} HlrHooks;

extern HlrHooks g_hlr;
