# Destrega native high-level renderer (HLR): implementation plan

Status: design only, nothing implemented. Builds on `docs/HIGH_LEVEL_RENDERER.md` (phase 1 done,
phase 2 "go") and corrects it where the findings of PRs #34 / #35 and a closer reading of the
runtime change it. Written against commit bb2d4bf. Facts about the game carry the NOTES.md
confidence levels (`verified` / `likely` / `guess`); design claims about the runtime are from
reading the code named in each section.

Terms: "HLR" = this renderer; "original path" = 1x GPU (`gpu.c`) + hi-res backend
(`hires.c` / `hires_gpu.c`) + PGXP + widescreen shadow pass, i.e. today's picture.

---

## Decisions (user, 2026-11-11)

- **Enhanced by default** (section 8 question 1): once a phase passes its acceptance, the
  enhancements it brings (per-pixel lighting, mipmapped / anisotropic textures, `clip` near plane,
  MSAA) are on by default with the GPU renderer; the PS1-faithful look stays available as an option.
- **Motion: predict ahead** (question 2): `--hlr-motion extrap` is the default, offline and in
  netplay. No added latency.
- **All work on the `feature/hlr` branch**, merged to main only with approval.
- **Texture modding / texture packs, later** (question 5; 2026-11-11): not implemented now, but
  the design must keep it easy. Every texture is named by a fingerprint of its content (texels +
  CLUT), stable and independent of its VRAM position; the texture-cache key and the material path
  get one single place where a replacement texture could be substituted (section 4.4,
  "Replacement hook"). Dumped textures are never committed.
- Open: questions 3, 4, 6-8 of section 8.

## Phase 0 results (2026-11-11, `feature/hlr`)

- **A. Stage packet audit: GO.** `--hlr audit` (`runtime/hlr/hlr_audit.c`, read-only, around the
  real `stage_render` pass) predicts every stage packet with `gte_exec_on` on a host GTE and
  compares words, links and OT slot heads: **0 mismatched, 0 unexplained, 0 missing** on all 12 VS
  stages, practice and story (1.9 M packets in all; per stage in NOTES "Stage builders in full").
  Remaining for phase 2a: the 0x2000 subdivision builders (800426c4 / 800433c4) are not modelled;
  they are recognised as alt ranges (gate 10 objects, castle1 2417 packets, remains 494, story 49).
  Per-frame `--state-hash` with the probe on equals off (vs_hills, vs_castle1 widescreen 2x3, the
  Milena repro), all with `--no-interp --verify-overrides`.
- **C. Bone layout: verified.** `workm = super.workm x coord` exactly (integer `>> 12`), over
  every part of both fighters in 67 dumps (`tools/hlr_probe.py coords`). Corrections below.
- **B. Head jitter: `float` fails, plain prediction makes it worse; only smoothing helps.**
  Milena repro, 48 updates, 203 head vertices (`tools/hlr_probe.py jitter`), mean per-vertex
  step / successive-step cosine: game 0.224 px / -0.33; float chain 0.186 / -0.56 (-17 %, still
  back and forth); EMA 0.3 0.083 / 0.29 (lag ~1 update = 33 ms); centred 5-tap 0.082 (2 updates
  look-ahead); linear prediction 1 update ahead 0.600 (2.7x worse); EMA 0.3 + predict 1 update
  0.136 (-39 %, ~0 lag). The jitter is in the animation's local matrices, not in the chain's
  rounding. **Consequence for the "predict ahead" decision**: extrapolating raw `workm` amplifies
  the jitter; `--hlr-motion extrap` must extrapolate a smoothed bone signal (EMA + prediction, or
  a one-euro filter) to stay steady without latency. `--hlr-bones float` is dropped; `smooth`
  becomes the only option against the shimmer (user decision whether it may be on by default).

## 1. Executive summary

- **Keep the capture-not-replacement architecture of the existing plan.** Every guest render
  function keeps running unchanged; thin read-only hooks at the override points that already
  exist (`f_80044210` in `overrides/widescreen.c`, `f_80036138` / `f_800363c8` in
  `overrides/char_mesh.c`) plus a few new ones (`fighter_render` 80037144, `fighter_shadow_render`
  8007ec5c, the effect builder family 80055820..80057100) copy meshes, matrices, camera and lights
  into a host-side `HlrFrame`. Guest RAM, GTE, GPU, scheduler and cycle counts are untouched by
  construction: the hooks only read, never call guest code, and the original function runs
  afterwards exactly as today.
- **Composite, do not replace.** The HLR draws only the 3D scene (scene OT C: stage, characters,
  shadows, effects, plus the two background quads of OT D) into the *existing* hi-res GPU target
  with a depth buffer, at the point of the ordering-table walk where OT C begins. HUD (OT A),
  menus (OT B), character select, FMV, loading screens and anything the capture does not
  recognise keep drawing through the original path. A missing capture shows today's picture, not
  a hole; each subsystem can be switched independently.
- **Suppression is exact and per packet.** Packets the capture replaced are, at linked-list DMA
  time, executed on the 1x GPU as today (UV / colour tracking, timing) but not forwarded to the
  hi-res target. Packet addresses are known per item (record + idx * stride).
- **Determinism is proven the same way as hi-res / PGXP / widescreen**: per-frame `--state-hash`
  with HLR on equals off; replays; `test_savestate.sh`; netplay with *one* peer on HLR. New: an
  **audit mode** that re-derives, on the host, the integer GTE results of every captured item and
  checks them against the packets the game actually wrote, so a capture is proven complete and
  correct *before* its 3D version is turned on.
- **What is new since the existing doc**: (1) PR #35 showed a per-character depth test does not
  fix the face shimmer; the shimmer is sub-pixel jitter of the whole head from the game's
  fixed-point bone chain. A native renderer only improves on it if it re-derives the bone chain in
  float from the local (`coord`) matrices or smooths the captured world matrices; that is a phase-0
  experiment, not an assumption. (2) 30 -> 60 Hz motion needs the composition split into an
  "under" image, the HLR layer and a premultiplied "over" layer, so the HLR can be re-rendered at
  the vblank where the game draws nothing. (3) The audit mode and a host-side GTE evaluator
  replace the doc's "orthographic PS1-exact compare mode" with something cheaper and stricter.
- **Order of work**: phase 0 = two kill-experiments (stage packet audit, head-jitter source), one
  week; phase 2 = backend + stage, 3-4 weeks; characters 3 weeks; shadows days; effects 2-3
  weeks; 60 Hz 2 weeks; story scenes 1 week; polish open-ended. Each phase ships behind a switch
  with the original path intact.

---

## 2. Architecture

### 2.1 Where the data comes from (capture points)

All addresses from `analysis/symbols.txt` / NOTES.md "Rendering pipeline". Hooks are C wrappers
of functions listed in `analysis/overrides.txt` (recomp emits `f_<addr>_orig`; the wrapper runs a
read-only pass and then calls `_orig`, as `char_mesh.c` does, bypassing `override_call`).

| Hook (guest function) | When it runs | What the HLR reads (read-only) | Confidence |
|---|---|---|---|
| `stage_render` 80044210 (already wrapped in `overrides/widescreen.c`) | first half of the update, after `camera_update`, once per update (twice in widescreen: capture only the **real** pass, i.e. when `g_bus_sandbox == 0`) | camera = GTE RT/TR (== GsWSMATRIX 800ed4c8) and H, OFX/OFY, DQA/DQB, FC, BK, LLM/LCM from the GTE control registers at entry (`gte_save` -> `GteState.c[]`); stage root `*(800c03b8)`: object table {data off, packet off, normal-index off, flags}, command words, world SVECTORs, normal table `*(800c03b4)`, packet buffers gp+0x5e8 / gp+0x5e4 (pre-initialised UV / CLUT / tpage words), per-object animation counters (flag 0x80); scratch 1f800000 (buffer idx), 1f800008/0c/10 (OT depth map) | camera, fog, lights, root layout: **verified** (phase 1); near plane / OT rule / UV animation / object flag semantics: **likely** |
| `fighter_render` 80037144 (new wrapper) or `char_mesh_render` 80036b0c | per fighter, first half | model block at fighter +0xe8 (part count +8, parts +0xc of 0x70 bytes, face groups +0x10), each part's word 0 -> GsCOORDINATE2-like struct: `workm` 3x3 s16 at +0x24, t at +0x38/+0x3c/+0x40; vertices part+0x64 (count +0x6c), normals part+0x68 (count +0x6e); attachment at +0x1a0; `char_lights` 800c9cf0 (3 x 12 bytes), `dyn_light` 800c9d20, `char_base_rgb` 800bfe28 | layout **likely**; face-list layout **verified** (char_mesh.c) |
| `char_mesh_tris_to_ot` 80036138 / `char_mesh_quads_to_ot` 800363c8 (already wrapped) | per face group | face records (0x5c / 0x78 bytes; vertex and colour indices at +0x50.. / +0x68..), the packet buffer (UV / CLUT / tpage words at pkt +12 / +24 / +36 / +48), `char_vertex_buffer` 800c9d38 and `char_colour_buffer` 800cb7c8 (for the audit only) | **verified** |
| `fighter_shadow_render` 8007ec5c (new wrapper, after `_orig`) | per fighter | `shadow_world_verts` 800e70f0 (45 x SVECTOR on the ground), `shadow_pair_table` 800b6158, the 64 triangle packets (stride 0x58, copy at + idx*0x28, grey levels) | **likely**; 128 semi GT3 per frame verified |
| effect builders 80055820, 800559b0, 80055b70, 80055d48, 80055f54, 80056348, 8005650c, 80056704, 80056908, 80056b40 (new wrappers, `a0..a3` + the caller's object) | second half of the update | the object's world point(s), the billboard offsets, colours, UV / CLUT / tpage words, and the preceding e1 draw-mode word (semi mode, tpage) in the same OT slot | packet codes per builder **verified**; argument layouts **likely / not read** |
| `background_prims_add` 8003e32c (optional wrapper) | first half | the two quads 800cc510 / 800cc540 (fixed; could be hard-coded) | **verified** |
| `frame_submit` 80026628 (new wrapper) | end of the update | seals `HlrFrame[idx]`: records the update counter and the scene OT base `*(800c0724 + idx*0x14)` | **likely** |

Everything a hook keeps must be **copied**, never referenced: after a save-state restore or a
netplay rollback the RAM a pointer would point at is a different timeline. Static mesh data
(stage vertices, character vertices / normals / faces, pre-initialised packet UV words) is
copied once into a host cache keyed by (RAM address, byte length, FNV-1a of the bytes) and
re-validated by hash when the key is seen again (cheap: stage ~100 KB once per stage,
character meshes once per load). Per-update data (bone matrices, camera, lights, shadow
vertices, effect vertices, animation counters) is copied per frame: a few KB.

### 2.2 Frame store and pairing

`HlrFrame[2]`, indexed by the game's buffer index (scratch 1f800000), each with
`{update counter, scene OT base, camera, lights, items[], packet address set}`. The frame built
under idx i is drawn at the next `vblank_callback` (80027268) draw of root OT i (verified,
phase 1). `frame_submit` seals it; the DMA walk consumes it exactly once (as `widescreen.c`'s
`s_used[]` / `char_mesh.c`'s `walked` do) and drops it at the end of the walk.

### 2.3 Suppression (in the GPU linked-list walk)

`bus.c dma_run` (ch 2, sync 2) calls `g_gpu_ll_hook` / `g_gpu_ll_hook2` per node. Replace the
two fixed pointers by a small registry `gpu_ll_add_hook(fn)` (bus.c, 4 slots) and add a
**filter** `bool (*g_gpu_ll_hires_filter)(u32 node)`: when it returns false, bus.c sets a gpu.c
flag (`gpu_set_node_no_hires(true)`) before writing the node's words and clears it after, and
`gp0_polygon` / `gp0_rect` pass `no_hires = true` to the triangle path (that parameter already
exists in `draw_tri`, gpu.c line ~590: "if (g_hires && no_hires) { tracking off }"). The 1x GPU
still executes the packet (the 1x skip rule means it only tracks; timing is unchanged), the
hi-res target never sees it. The filter answers from the current `HlrFrame`'s packet address set
(a hash set of `record + idx * stride` addresses; ~1000 entries per frame).

A packet is suppressed only if the frame being walked has a capture for it. No capture (HLR off,
re-simulation, a subsystem off, a restore without side-car) = nothing suppressed = today's picture.

### 2.4 Rendering into the hi-res target (composition)

Order of the root OT (verified): D (background), B (menus, empty in fights), C (scene), A (HUD).
The HLR pass runs when the walk reaches the first node inside the scene OT range
`[base, base + 8192)` of the frame being drawn (the hook already sees node addresses; that is the
D/B -> C transition). It renders the captured frame **into the hi-res backend's colour target**
with its own depth texture, then the walk continues: non-captured OT C packets (unknown effects,
e1 words) and OT A draw over it through the normal hi-res path, depth-less, as they do today.

Mechanism: a new optional `HiresBackend` entry
`void (*external)(void (*fn)(void *ctx, void *cmdbuf, void *colour_tex, int w, int h), void *ctx)`.
`hires_gpu.c` implements it by closing the current group, ending its render pass (the same
break it does for VRAM uploads / copies, see its header "Render passes break only for..."),
and calling `fn` with the command buffer and `s_target`; the HLR begins its own render pass
(colour = `s_target` LOAD/STORE, depth = HLR-owned D32_FLOAT texture of the same size,
CLEAR), draws, ends it; hires_gpu then resumes. The software backend leaves `external` NULL:
**the HLR needs the GPU renderer** (View > Renderer: Metal / Vulkan / Direct3D 12); with
Software it is greyed out and logged. Headless runs use `--renderer gpu` (already supported with
the backend's own device).

The target is 15-bit (B5G5R5A1) or RGBA8 (full-precision colour); the HLR pipelines are built
for both formats like hires_gpu's `s_pipe[0/1]`. In widescreen the target already has the
guard band; the HLR renders the full wide frustum (no shadow pass needed when the stage capture
is on, see 2.6).

**Why composite, not replace the whole PS1 picture**: the HUD, menus, title, character select
and FMV are 2D work the original path already renders well (xbr 2D rule, bit-exact tests); a
full replacement would need a second 2D renderer, lose the automatic fallback for anything not
captured, and could not ship in phases. Compositing costs one render-pass break per frame
(hires_gpu already breaks passes for uploads) and keeps every existing test and setting
meaningful. The only thing replacement would give is freedom in output resolution and format,
which the hi-res target already provides (any scale, RGBA8, guard band).

### 2.5 Coexistence with hi-res, PGXP and widescreen

- Hi-res: unchanged for everything not suppressed. The HLR layer inherits the target scale,
  colour format, SSAA and the guard band; the present path (`main.c present_frame`) is untouched.
- PGXP: still runs (tags, welding, `char_mesh.c` extras) for packets that are *not* suppressed.
  For suppressed packets `char_mesh.c` must not add extras: the HLR character capture replaces
  the `char_mesh.c` extras path for that fighter (one `if (hlr_chars_active()) return;` at the top
  of `tris_extras` / `quads_extras`). PGXP tracking itself (gte.c, pgxp.c) is not touched; the HLR
  never reads PGXP values (it has the real 3D data).
- Widescreen: while the HLR stage capture is on, `stage_render`'s shadow pass is skipped (the
  HLR draws the whole frustum from the object list, culled on the host at the wide aspect) and
  `g_widescreen_cull` stays 0. The presentation aspect logic (`s_ws_stage`, `ws_present_extra`,
  `g_ws_cull_hits`) keys on `WS_IMM` evaluations, which the real pass still performs, so it
  keeps working. The two OT D background quads (fixed 4:3 coordinates, verified) are drawn by
  the HLR as a full-width gradient when the stage capture is on (today they leave the guard band
  black / cyan-less; the doc lists this as a separate fix).
- Fast precision / texture cache: the HLR samples textures through the fast path's texture
  cache (`tc_lookup`, page + CLUT + depth + window -> decoded premultiplied RGBA8 layer with
  mips + STP coverage layer). The cache is independent of `s_fast`; expose `hires_gpu_tc_layer()`
  so the HLR can use it with the exact precision too. No new invalidation logic.

### 2.6 Switches (runtime, menu, settings)

- `--hlr off|on|exact|audit` (default off until phase 2 passes its acceptance; then `on` in a
  window when the GPU renderer is active, off headless so every existing test keeps its hashes).
  `exact` = draw the captured items with the PS1 projection and painter's order through the
  existing hires pipelines (section 3.1); `audit` = `off` plus the capture audit (section 3.2).
- `--hlr-parts stage,chars,shadows,fx,story` (default all once each phase ships);
  `--hlr-motion off|interp|extrap` (section 4.8); `--hlr-msaa 1|2|4|8`; `--hlr-bones game|float|smooth`
  (section 4.2); `--hlr-near ps1|clip` (section 4.3); `--hlr-tex-dump DIR`, `--hlr-tex-dir DIR`.
- Menu (`menu_mac.m`, `menu_win.c`, after View > Anti-aliasing): **View > 3D Scene** with
  Original / Native (radio), then check items Stage, Characters, Shadows, Effects, Smooth Motion
  (60 Hz), and an Anti-aliasing (MSAA) submenu; greyed out unless a GPU API is the renderer
  (`frontend_renderer_available`), allowed during netplay (presentation only, like `renderer`).
  Menu actions `hlr[=off|on]`, `hlr-part=stage:on`, `hlr-motion=...`, `hlr-msaa=N` (also
  `--menu-action`, headless logs). Settings keys `[display] hlr`, `hlr_parts`, `hlr_motion`,
  `hlr_msaa`, `hlr_bones`; flags lock keys as the other display options do.
- Every switch takes effect between frames (`menu_script_and_poll`), at a vblank: the next
  capture / walk sees the new value; a frame in flight keeps its capture.

### 2.7 Files and build identity

New, **presentation-only, excluded from the netplay identity** (`tools/source_ident.py` lists
`runtime/` top level and `runtime/overrides/`; a new directory is outside both, and the EXCLUDE
comment gets a line saying so explicitly):

```
runtime/hlr/hlr.h          public API (switches, hooks table, report), HlrFrame types
runtime/hlr/hlr.c          frame store, packet set, DMA-walk hook + filter, side-car, switches, exit report
runtime/hlr/hlr_gte.c      host-side GTE: float camera / lighting / depth cue, and the exact integer port (audit, exact mode)
runtime/hlr/hlr_stage.c    stage capture (root walk, builders' rules, UV animation)
runtime/hlr/hlr_char.c     character capture (parts, bones, faces), bone re-derivation / smoothing
runtime/hlr/hlr_shadow.c   shadow decal capture
runtime/hlr/hlr_fx.c       effect builder captures
runtime/hlr/hlr_gpu.c      SDL_GPU pipelines (depth, skinning, billboards, decals, compose), MSL embedded
runtime/shaders/hlr.glsl   Vulkan GLSL -> hlr_spirv.h (gen_spirv.py SOURCES['hlr']) and hlr_dxil.h (gen_dxil.py)
runtime/tests/test_hlr.c   CTest, no disc (section 7)
tools/test_hlr.sh, tools/hlr_diff.py, tools/hlr_probe.py (phase 0)
```

Covered (identity-changing, kept minimal and stable):
```
runtime/hlr_hook.h               a struct of function pointers g_hlr (stage, char, shadow, fx, submit, ll_filter), NULL = off
runtime/overrides/widescreen.c   f_80044210: call g_hlr.stage_pre/post around the real pass; skip the shadow pass when it says so
runtime/overrides/char_mesh.c    skip extras for fighters the HLR captures
runtime/overrides/hlr_hooks.c    the new wrappers: f_80037144, f_8007ec5c, f_80055820 .. f_80056b40, f_80026628 (each: g_hlr.x ? g_hlr.x(args) : 0; f_<addr>_orig())
analysis/overrides.txt           the new addresses
runtime/bus.c                    gpu_ll_add_hook(), the hi-res filter
runtime/gpu.c                    gpu_set_node_no_hires()
runtime/gte.c                    gte_exec_on(GteState *, u32 cmd) (section 3.2)
runtime/savestate.c|h            ss_add_sidecar() (section 3.4)
runtime/hires.h|c, hires_gpu.c   HiresBackend.external, hires_gpu_tc_layer(), the over-layer (phase 6)
runtime/main.c, menu_*, settings  switches (menus/settings are excluded anyway)
```
Two players with different HLR settings or HLR versions can play: the identity covers only the
hook shims, which change once per phase like any runtime change (both players update then, as
for every covered file).

---

## 3. Determinism

### 3.1 Rules for the hooks

1. **Read-only, synchronous, no guest calls.** A hook runs inside the guest's C call chain (it
   *is* the wrapper of a guest function). It reads `g_ram` / `g_scratch` / `GteState` and copies.
   It never writes guest memory, GTE or CPU registers, never calls `guest_call`, `rt_call` or a
   generated function, never touches the bus (`bus_read*` has side effects on I/O: use the direct
   `g_ram` reads as `char_mesh.c`'s `ram32`). It does not keep guest-derived values across a
   guest call, so `g_ss_unsafe` is not needed; the vblank cannot land inside a hook (hooks do not
   run `EVT_CHECK`).
2. **No emulated time.** Hooks add host time only. `CYC()` / `next_event` are untouched.
3. **If a capture ever needed guest code to run** (none planned: the shadow verts, bone matrices,
   lit colours are all already in RAM when the hook runs), it would use the widescreen pattern:
   `gte_save`, scratchpad copy, `mem_journal_begin`, `g_cpu.next_event = ~0`, `g_bus_sandbox = 1`,
   `g_ss_unsafe++`, run, undo, restore, `widescreen_add_state_hook`-registered host state restored.
   Reject the capture on any `g_bus_sandbox_hits`. Document the reason in NOTES.md if it happens.
4. **GPU side**: suppression only changes what is forwarded to `g_hires`; `gpu.c`'s 1x execution,
   tracking and GPUSTAT are unchanged. The HLR pass runs inside hires_gpu's command buffer and
   never reads back into guest state.
5. **PGXP**: never written by the HLR (the `cull` flag stays with char_mesh.c for the uncaptured
   fighters).

### 3.2 The audit mode (how each capture proves it is complete and right)

Before any 3D output, each capture phase must pass `--hlr audit` on the scenario and replay
library. The audit re-derives, on the host, exactly what the game wrote:

- `gte.c` gains `gte_exec_on(GteState *s, u32 cmd)`: swaps the module globals (`d`, `c`, `g_flag`)
  with `*s`, runs `gte_command(cmd)` with PGXP production disabled (a static flag checked in
  `pgxp_project`), swaps back. Pure with respect to emulated state (the guest GTE registers are
  restored byte for byte; `gte_state_io` sees no difference). Cost ~2 x 260-byte copies per
  command; a fight frame needs ~3000 commands = negligible. Tested in `test_hlr.c`: the guest
  GTE state hash before == after.
- The audit builds, from the captured item, the packet words the game's builder must have
  written: the same RTPT/RTPS/NCDT/NCDS/DPCT/NCLIP sequence on a `GteState` initialised from the
  camera / light registers of the capture, the same OT-slot map (1f800008/0c/10), the same
  culling. It then compares, per item, with the packet at `record + idx * stride` after
  `_orig` returned (XY words, colour words, OT link = slot) and with the set of packets actually
  chained into the scene OT (`collect()` of widescreen.c, reused). Mismatches are logged with
  the item and the first differing word; the exit line `hlr-audit: N items, M packets, K
  mismatched, U unexplained packets` must be `0 / 0` for the subsystem to be allowed on.
- `--hlr exact` is the pixel-level form of the same proof: the captured items are turned into
  `HiresPrim` + `HiresVtx` (integer positions from the audit's GTE run, UV / colours from the
  packet words) and sent through `g_hires->tri` in the order the OT walk would have drawn the
  suppressed packets. With the original packets suppressed, the hi-res target must be
  **byte-identical** to a `--hlr off` run (`--dump` PNGs compared by `tools/hlr_diff.py`, and
  `--state-hash` equal). This is the doc's "orthographic PS1-exact compare mode", obtained for
  free from the audit.

### 3.3 Save states, rollback, re-simulation

- Snapshots are taken at the vblank between the two halves of an update (verified, phase 1), so
  the frame drawn after a restore has its first half (stage, characters, shadows) captured before
  the snapshot. Two acceptable behaviours, in order of delivery:
  1. **Phase 2 minimum**: `ss_restore` (savestate.c, next to `pgxp_reset()` / `hires_reseed()`)
     calls `hlr_on_restore()`, which drops both `HlrFrame`s. The next draw finds no capture,
     suppresses nothing and shows the packet picture for one draw (2 vblanks). Visible as a
     one-frame change of look after a rewind; netplay rollbacks of depth <= 2 vblanks would show
     it per rollback.
  2. **Side-car** (phase 2, second PR): `ss_add_sidecar(name, save_fn, load_fn)` in savestate.c:
     after `ss_visit_all` in SS_SAVE / SS_LOAD only (never in SS_HASH, never in `ss_hash_all`),
     the registered presentation modules append / read a blob. `HlrFrame[2]` is 10-50 KB (static
     meshes are in the host cache, keyed by address+hash, and re-validated against the restored
     RAM on first use), so the netplay ring of 24 snapshots grows by ~1 MB. The side-car also
     stores the previous-frame data motion interpolation needs (section 4.8) or marks it invalid.
- Re-simulation (`ss_resimulating()`): netplay.c `hires_for_resim` sets `g_hires = NULL` for all
  but the last 3 vblanks; the hooks capture only when `g_hires && g_hlr.on`, so they do nothing
  during the presentation-free part and re-capture for the final vblanks. An update straddles 2
  vblanks and draws happen every 2, so 3 vblanks is enough in the common case; where it is not,
  the no-capture fallback shows the packet picture for that one draw. (Raising the 3 to 4 in
  `hires_for_resim` is a one-line, covered change if it is ever visible.)
- `--verify-overrides`: the wrappers bypass `override_call` (not rewrites, like widescreen.c /
  char_mesh.c). The verifier runs `_orig` twice from one snapshot; with the hooks reading only,
  both runs see the same input. The HLR must be **off** during the verifier's second run anyway:
  `override_call` sets `g_ss_unsafe`; the hooks check `g_ss_unsafe == 0` and skip (defensive).
- Widescreen's journal: hooks run on the real pass only (`g_bus_sandbox == 0` check in the
  `stage_render` wrapper), so they never see journalled state.

### 3.4 Tests that prove it (existing + new)

| Check | Tool | Pass condition |
|---|---|---|
| State hash HLR on == off | `tools/test_hires_determinism.sh` new cases `hlr4x6`, `hlrws` (16:9), `hlrmsaa` | identical `--state-hash` files; `pgxp-check` still 0 |
| Replays | `tools/replays.py run` with `DESTREGA_ARGS="--renderer gpu --hlr on"` on 3 replays (`vs_tieme_rohzen_villa`, `practice_milena`, `match_gradd_celia_hills`) | no divergence, state hash lines equal |
| Save states | `tools/test_savestate.sh` with `--renderer gpu --hlr on`; plus a side-car check: `--savestate-rewind 37:5 --dump` frames at the restore points byte-equal to a plain `--hlr on` run's | per-frame hashes equal; dumps equal (phase 2b) |
| Netplay | `tools/test_netplay.py` new case `lan_hlr`: host `--renderer gpu --hlr on`, joiner software | same pass rules as `lan` (hashes equal to the offline run) |
| Completeness | `--hlr audit` on `scenarios.py run --all` and the replay library | `hlr-audit:` 0 mismatched, 0 unexplained |
| Pixel exactness of the capture | `--hlr exact` vs `--hlr off` dumps | 0 differing pixels (`tools/hlr_diff.py`) |
| No guest observation of suppressed packets | existing `gpu-1x:` tile detector (unchanged) | 0 |
| Story scenes | `scenarios.py run story` with HLR on | state hash equal; audit 0 |

---

## 4. Rendering design

### 4.1 Camera and projection (verified facts, float reconstruction)

At every stage builder entry GTE RT/TR == GsWSMATRIX 800ed4c8, OFX = OFY = 0, H in 0x342..0x3bb,
ZSF3 = 0x155, the matrix row 1 carries the 0.625 Y scale (`camera_aspect_base` 800ed4e8 =
diag(0x1000, 0x0a00, 0x1000)). The PS1 projection is sx = H * x / z, sy = H * y' / z with
y' = 0.625 y, screen centre (256, 120), 512 x 240 shown at 4:3. Because 240 / 0.625 = 384 =
3/4 * 512, the 0.625 is exactly the display's pixel aspect: in square-pixel NDC the frustum is
`tan(half_fov_x) = 256 / H`, `tan(half_fov_y) = 192 / H`, aspect 4:3.

HLR camera: `view = WS_rows_0_and_2_as_is, row_1 / 0.625, translation likewise`; projection
`f_x = H / 256`, `f_y = H / 192` (4:3) or `f_x = H / (192 * A)` for aspect A (widescreen
`A = 16/9` or the window's, same clamp as `ws_present_extra`). Reverse-Z, D32_FLOAT, near
= 16 view units, far = 65536 (SZ range). Depth cue and OT slots use the view z of the vertex
(= the GTE SZ before rounding). The stage's drop rule (SZ <= 0x18f -> whole polygon dropped)
is an option: `--hlr-near ps1` reproduces it (fidelity: walls close to the camera vanish as on
the PS1), `clip` lets the near plane cut them (shows geometry the PS1 never showed; default
`ps1` until someone looks at every stage with `clip`).

Camera modes 2 / 3 (80087a00 / 80087fe4, never seen) and the EVENTOVL cameras need no special
code: the capture reads the GTE registers, not the camera variables.

### 4.2 Characters: skinning, bones, and the shimmer

- Mesh: per part, vertices in bone space (SVECTOR, part+0x64), normals (part+0x68), one world
  matrix per part (`workm`), faces index vertices across parts (joints closed by topology:
  verified). HLR: one static vertex buffer per model block (position, normal, part index,
  from the host cache), one index buffer per face group (tri / quad -> 2 tris, winding from the
  game's NCLIP order: tris (A, C, B), quads (A, B, C) / (D, B, C)), per-update bone array (<= 64
  matrices) in a uniform buffer; vertex shader `p_world = bone[part] * p`.
- Backface culling in float (NCLIP sign == winding sign), not the integer test; no
  integer-degenerate faces exist here, so the `char_mesh.c` extras machinery is not needed for
  captured fighters.
- Lighting: NCCT/NCCS semantics per part: `colour = base_rgb * (BK + LCM * clamp(LLM * n, 0))`,
  with LLM / LCM per part as `bone_light_matrices` (800338d8) builds them from `char_lights`
  (3 lights, light 0 = `dyn_light` with its per-bone direction and timer-scaled colour). The HLR
  reads LLM / LCM per part from the GTE control registers at each part's draw (they are written
  before the part's NCCT, verified by the ctc2 log) or recomputes them from `char_lights` /
  `dyn_light` (likely layout); start with the registers (verified), move to recomputation when
  per-pixel lighting is wanted (section 4.6).
- **The shimmer** (NOTES "Floor tile seams and face shimmer", verified): the head moves 0.24 px
  per update in a random direction; a depth test does not help (PR #35). The HLR renders from
  the same `workm`, so by default it shows the same jitter. Three options behind `--hlr-bones`:
  - `game` (default until proven otherwise): use `workm` as the game computed it.
  - **Phase 0 corrections (verified)**: the coord structs are 0x16c apart, `param` (+0x44) points
    inline at struct + 0x50 and its rotate angles do not reproduce the local matrix (stale or
    unused: likely), +0x4c is not a reliable `sub`; `workm` is the exact integer product. The
    `float` option below measured only -17 % and is dropped (Phase 0 results, B).
  - `float`: re-derive each part's world matrix from the chain of local matrices. **Guess**: the
    0x50-byte struct is a real libgs `GsCOORDINATE2` {flg +0, coord MATRIX +4 (m at +4, t at
    +0x18), workm MATRIX +0x24, param +0x44, super +0x48, sub +0x4c}: the camera's 800cc670 /
    800cc620 pair matches this layout exactly (matrix at +4, flags zeroed by `camera_apply`,
    super / sub links), and libgs `GsGetLw` composes `workm = super.workm * coord` with 12-bit
    truncation per level, which is a plausible source of the per-update jitter. Walking `super`
    and multiplying `coord` matrices in double removes the per-level truncation.
  - `smooth`: presentation-only temporal filter on the captured world matrices (translation and
    rotation separately; e.g. a two-update average or a one-euro filter with a jump threshold so
    real motion is not lagged). Adds up to half an update of visual latency; never used in
    netplay by default.
  Phase 0 decides between `float` and `smooth` (section 6, experiment B).

### 4.3 Stage

World-space vertices, no per-object matrix (verified). Per object: visibility (object 0 always;
flags 0x4000 / 8-without-0x8000 / 0x40 in modes 0-1 / 0x100 in mode 2 hide; likely) then the
frustum test on the host against the *wide* frustum (replaces the +-257 screen test); polygons
are never dropped for x / y, only by the near rule (4.1). Builders: 0x20 / 0x28 flat, 0x34 /
0x3c lit (normals from the normal table, NCDT/NCDS semantics, Hills only), 0x35 / 0x3d prelit
(per-vertex colours, DPCT/DPCS = depth cue only), flag-0x2000 objects through 800426c4 /
800433c4 (guess: different packet layout; the audit will tell). UV / CLUT / tpage from the
pre-initialised packet words; UV flipbook animation: the u bytes the game rewrites in the
*current* buffer's packets (flag 0x80 objects), read after `_orig` (the capture of a frame reads
the packet buffer of the same idx). Static geometry goes into one vertex / index buffer per
stage (host cache), per-frame data is the visible-object list, camera, lights, and the animated
u bytes (a small per-polygon UV override table).

Depth cue (fog): `ir0 = clamp((DQA * hd + DQB) >> 12, 0, 4096)` with `hd = 65536 * H / SZ` as
gte.c `rtp` computes `dq`; `colour = lerp(lit_or_prelit, FC, ir0 / 4096)`. Per-vertex in the
vertex shader (as the GTE), per-pixel later as an option. FC / DQA / DQB / BK per stage are
verified (NOTES table); the capture reads them from the GTE registers at `stage_render` entry.

### 4.4 Textures

- Source: the game's VRAM, through the fast path's texture cache (`hires_gpu.c` "Texture
  cache"): key (page x/y, CLUT x/y, depth, window); decoded premultiplied RGBA8 256x256 layer
  with mips + STP coverage; invalidated by VRAM write stamps + content hash. The HLR material =
  (cache layer, semi mode, raw, textured, gouraud). No texture windows are ever set for 3D
  triangles (verified), so the key degenerates to (page, CLUT, depth).
- Sampling: trilinear + anisotropic (the cache's sampler), u/v from the packet bytes / 256 in
  page space; sub-texture bleed at mip levels >= 1 as documented (packed at 1-texel granularity,
  verified); at 4x6 > 99 % of textured pixels are magnified, so it is invisible there and
  acceptable at lower scales (measured for the cache already). `--tex-filter nearest` maps to a
  nearest sampler for level 0.
- **Replacement hook** (polish phase): `--hlr-tex-dump DIR` writes each *new* cache layer as
  `<fnv1a of page texels + CLUT>.png` (content-keyed, so the same texture gets the same name on
  every stage; never under the repo: default `<data dir>/textures/dump/`); `--hlr-tex-dir DIR`
  (and `[display] hlr_tex_dir`) loads `<hash>.png` of any size into a per-entry 2D texture with
  its own mips; the fragment shader scales u/v by the replacement's size / 256. Nothing
  game-derived is ever committed (`.gitignore` the default folders; `tools/pr_check.sh` can grep
  for PNGs under `runtime/` / `tools/` as a guard). Per the decision above, the fingerprint is
  computed from the decoded texels + CLUT (never from page / CLUT coordinates), and one function
  (`material_texture(key) -> layer or replacement`) is the only place both the cache and the
  material path get a texture from, so a pack only plugs in there.

### 4.5 Translucency, effects, shadows, order

- Opaque pass (depth write): stage + characters, front to back by object for early-Z (any order
  is correct). Equal depth on shared edges is handled by the depth test, no OT needed.
- Decal pass: shadows (64 triangles per fighter on `shadow_world_verts`, which lie on the floor
  by construction): depth test on, no depth write, polygon offset / depth bias toward the camera,
  blend reverse-subtract (B-F, likely; the GPU command counts show B-F dominates). Later option:
  a real projected shadow (polish).
- Transparent pass: effects, sorted by their PS1 OT slot (the 3-segment map on the billboard's
  minimum SZ, verified for 80055b70) back to front, depth test on, no depth write; blend per the
  e1 word's semi mode: B/2+F/2 = constant 0.5, B+F = add, B-F = reverse subtract, B+F/4 = 0.25 F
  add (the hires_gpu blend states, 15-bit and RGBA8 variants). Billboards: view-aligned quads at
  `WS * p` with screen-space offsets converted to view units (`off * z / H`) so sizes match;
  the 80055b70 family does no x / y / near culling (verified for 80055b70). A per-effect "depth
  test off" flag exists for billboards that visibly cut into the floor (the doc's concern) and is
  decided by looking, per builder, in phase 5.
- Within one pass the order of equal-depth translucent items follows the OT slot then the
  original packet order (stable sort), so the game's own ordering among effects is preserved.
- Background (OT D): the HLR clears colour to the two background quads (gradient top half,
  flat cyan bottom half, full target width) instead of letting hires draw them; depth cleared to
  far. HUD (OT A) draws afterwards through hires as today.

### 4.6 Lighting (match first, enhance later)

Phase targets the GTE formulas exactly (per-vertex, saturating like the GTE in `exact` /
`audit`, float in `on`). Enhancements once the match is proven: per-pixel evaluation of the same
lights (characters have real normals; the Hills stage too; prelit stages have none, so
"per-pixel" there means fog only), the dynamic point light as a true point light with falloff,
optional normal reconstruction from face geometry for prelit stages. All optional, off by
default, user decision (section 8).

### 4.7 2D, HUD, menus, FMV

Untouched: OT A / OT B / title / character select / FMV / load screens / KOEI logo go through the
original path. Character select draws 2D GT4 quads from 8008a930 / 8004df70 (verified: OT B);
there is nothing 3D to capture, so it stays 2D (a 3D character preview would be new content,
not a port). The gallery of HUD fixes (health bar scale in widescreen etc.) is not part of this
plan.

### 4.8 Motion: 30 -> 60 Hz (and beyond)

Fights update at 30 Hz (`VSync(2)`), the game draws at vblank N and nothing at N + 1. To show a
different picture at N + 1 the HLR layer must be re-rendered and re-composited without the game
drawing anything. Design (phase 6):

- `hires_gpu.c` gains an **over-layer** mode: when the HLR pass has run in a frame, everything the
  walk draws *after* it (OT C leftovers, OT A) goes into a separate RGBA16F premultiplied layer
  (opaque = alpha 1; B+F = (F, 0); B-F = (-F, 0); B+F/4 = (F/4, 0); B/2+F/2 = (F/2, 0.5)) instead
  of the target; the pre-HLR target content ("under": background + OT B) is copied to a texture at
  the HLR insertion point; present = `clamp(under_with_HLR) * (1 - A) + C`, done by a compose
  pass before `hires_gpu_present`. Per-op clamping of chained PS1 blends is lost inside the layer
  (only where several blended HUD primitives stack and saturate; rare; measured by
  `--renderer compare`-style diff on fight frames in phase 6).
- At N + 1 the HLR re-renders from `under` with camera and bone matrices at parameter t between
  the previous and the current captured update (`interp`: show (k-1, k) at t = 0.5 at N and k at
  N + 1, i.e. half an update of extra latency, 16.7 ms; `extrap`: k at N and k + 0.5 of (k - k-1)
  at N + 1, no latency, overshoot on direction changes). Items are matched by stable keys
  (fighter + part, shadow + fighter, effect slot + builder); unmatched items snap. Stage: camera
  only. Effects: position / colour lerp by slot.
- Netplay: the previous-frame data lives in the capture, so a rollback that re-captures also
  re-derives it; after a restore without a side-car the interpolation snaps (no history). Nothing
  here reads host time, so determinism is untouched; `extrap` is the netplay-friendly default
  only if it looks right (user decision).
- Higher than 60 Hz (VRR / 120 Hz windows): the same interpolation at the host's present rate,
  once the frontend presents more often than once per vblank (not planned here).

### 4.9 Anti-aliasing

MSAA on the HLR pass only: colour attachment = a multisampled texture of the target's format,
resolved into `s_target` at the end of the pass (SDL_GPU `resolve_texture` on the colour target
info); depth multisampled. The 2D layers stay as they are (xbr 2D rule, SSAA cap). `--hlr-msaa
1|2|4|8`, capped by `SDL_GPUTextureSupportsSampleCount`. Since the HLR output is not bit-exact
against software by nature, there is no new equality test to break; the state-hash tests still
apply.

### 4.10 Backend and shaders

- Device: the hires GPU backend's device (`hires_gpu_init(dev)` already creates the
  windowed renderer on it; headless has its own). The HLR creates its pipelines on
  `hires_gpu_device()` (new getter) when the backend is initialised and releases them in
  `hires_gpu_shutdown` (an API switch destroys the device: add `hlr_gpu_shutdown()` to its
  sequence and `hlr_gpu_init()` after the new device is up, mirroring the renderer switch).
- Shader formats: MSL source embedded in `hlr_gpu.c`; `runtime/shaders/hlr.glsl` with
  `#if VS_SCENE / FS_SCENE / VS_BILLBOARD / FS_COMPOSE ...` sections, added to `gen_spirv.py`
  `SOURCES['hlr']` -> `hlr_spirv.h` and to `gen_dxil.py` -> `hlr_dxil.h` (same register
  convention: GLSL set = HLSL space, vertex uniforms b0 space1, textures t0.. space2, fragment
  uniforms b0 space3). `gen_dxil.py --check-fresh` covers the new header; CI's clang job compiles
  it with the downloaded DXC. Plain float shaders: no 64-bit, no exact edge test, no `PORTABLE_MUL`
  worries.
- Pipelines: scene (opaque, skinned or static, per blend state 15-bit / RGBA8 x MSAA), decal,
  billboard (4 blend states), compose, plus the `exact` path which emits `HiresPrim`s into the
  existing hires pipelines (no new shader).
- Platforms: Metal, MoltenVK, Vulkan, D3D12 through SDL_GPU as today; CrossOver for the Windows
  build; `test_hlr_gpu`-style CTest without a disc (section 7).

---

## 5. Phases

| # | Phase | Scope | Guest addresses (confidence) | Effort | User-visible after |
|---|---|---|---|---|---|
| 0 | Kill-experiments | stage packet audit probe; head-jitter source; GsCOORDINATE2 chain check | 80044210, 8003fcb8..80041c14, 800426c4/800433c4 (likely); 80037144 parts (likely), +0x48 super (guess) | 1 week | nothing (go / no-go, NOTES.md entries) |
| 2a | Backend + composition + stage (exact) | hlr core, DMA filter, `external` pass, side-car-less restore, stage capture, audit + exact mode, menu / settings | as phase 0 + 800c03b8, 800c03b4, gp+0x5e4/5e8, 1f800000/08/0c/10 (verified), 800ed4c8 / H (verified) | 3 weeks | View > 3D Scene (Native) with Stage: identical picture in exact mode; audit line |
| 2b | Stage in 3D | depth pass, float camera, fog, MSAA, widescreen full frustum, background gradient, side-car | same | 1-2 weeks | real-3D stage at any aspect, no shadow pass, MSAA |
| 3 | Characters | parts, bones, skinning, per-part lights, float backface, `--hlr-bones` | 80037144, 80036b0c, 800338d8, 800c9cf0, 800c9d20, 800bfe28 (likely); faces (verified) | 3 weeks | steady characters (if phase 0 B says so), correct intersections |
| 4 | Shadows | decal pass | 8007ec5c, 800e70f0, 800b6158, 800b6264 (likely) | 3-4 days | shadows draped without z-fight / sort artefacts |
| 5 | Effects | 10 builders, containers' e1 words, OT-slot sorted transparent pass | 80055820..80056b40 (codes verified; args likely), 8004e0e8, overlay e1 writers (verified) | 2-3 weeks | complete fight scene in 3D; widescreen effects no longer pop (host culling) |
| 6 | Motion 60 Hz | over-layer, under copy, interpolation / extrapolation, menu | none new | 2 weeks | smooth 60 Hz fights |
| 7 | Story scenes | EVENTOVL stage (already works via stage_render), its character / camera code | overlay code (not read) | 1 week + RE | story cutscenes in 3D |
| 8 | Polish | texture replacement, per-pixel lighting, projected shadows, `clip` near plane, VRR | none | open | user-chosen enhancements |

Each phase is one or more PRs; the original path and every existing test stay intact; a phase's
switch defaults off until its acceptance passes, then on in a window (GPU renderer) and off
headless.

### Phase 0: kill-experiments (first; ~1 week)

**A. Stage packet audit probe** (the cheapest experiment that kills the plan if it fails).
A temporary read-only hook on `stage_render` (in a scratch branch, like phase 1's probe) walks
the stage root with the rules of NOTES "Stage", runs the integer GTE sequence of each builder on
a host `GteState` (`gte_exec_on`, the first covered change, ~40 lines in gte.c), and after
`_orig` compares the predicted packet set (addresses, XY words, colour words, OT slots) with
the scene OT (`collect()` from widescreen.c). Run `scenarios.py run --all` (12 VS stages, story,
practice) headless with it. Pass: < 0.1 % mismatched packets and 0 unexplained stage packets on
every stage, and the mismatches explained (e.g. the 0x2000 alt builders). Fail: the stage format
or the builders' rules are not understood; the plan's data model is wrong and must be fixed
before anything else (back to REA on 8003fcb8..80041c14, 800426c4 / 800433c4). This also
delivers the audit module of phase 2a.

**B. Head-jitter source** (decides whether the HLR can promise steady characters). Using the
Milena idle repro (NOTES: practice_milena inputs to 3560, Right taps 3600-3660, Down taps
3700-3760; head at 1x (205..250, 112..152) from ~3800), log per update, for each part of P1:
`workm` (+0x24..), `coord` (+0x4..), the +0x44 / +0x48 / +0x4c words, and the projected head
vertices (host float projection from `workm`). Then compute three series of the head's screen
position: (1) from `workm` (should reproduce the 0.24 px random walk), (2) from the float
product of the `coord` chain via `super` (if +0x48 is a valid pointer chain ending at a root),
(3) a smoothed (1). Metric: mean per-update displacement and the successive-step cosine, as
in NOTES. Pass for `float`: (2) cuts the mean step by > 2x with cosine near +1 (smooth
breathing). If `super` is not a chain, test (3) only. Either way the result goes to NOTES.md
("Character mesh and PGXP") with the layout at its proven level.

**C. GsCOORDINATE2 layout check** (part of B): RAM dump at a fight frame, verify that for each
part the word at +0x48 points to another 0x50-byte struct whose `workm` times this part's
`coord` reproduces this part's `workm` within integer rounding. One script
(`tools/hlr_probe.py` on `--ram-dump`), half a day.

Tools: a coverage-style probe build (phase 1's method, NOTES "Rendering pipeline"), `--ram-dump`,
`--gameplay-trace`, `tools/movelab.py` for poses, `tools/flicker_metric.py` for the before / after
of B. Nothing in these experiments needs to be merged except NOTES / symbols updates.

### Phase 2a: backend, composition, stage capture in exact mode (3 weeks)

Files: everything in section 2.7 except `hlr_char.c`, `hlr_shadow.c`, `hlr_fx.c`.
Steps (each a PR where sensible):
1. `bus.c` hook registry + hi-res filter; `gpu.c` `gpu_set_node_no_hires`; `HiresBackend.external`
   in hires.h / hires_gpu.c (software: NULL); `hires_gpu_device()`, `hires_gpu_tc_layer()`.
   Test: state hashes unchanged (the filter with an empty set is a no-op); `test_hires_gpu`
   unchanged.
2. `gte.c gte_exec_on`; `test_hlr.c` checks the guest GTE is restored and that a host NCDS /
   RTPT run equals a guest run on random registers (the unit test links gte.c already:
   `_t_pgxp` set in CMakeLists).
3. `hlr.c` frame store, packet set, switches, exit report (`hlr: N frames captured, M packets
   suppressed, ...`), menu actions, settings.
4. `hlr_stage.c` capture + audit (from phase 0 A), `--hlr audit` on the scenario library.
5. `--hlr exact`: emit `HiresPrim`s through the external pass in OT order. Acceptance: dumps
   byte-identical to `--hlr off` on `vs_hills`, `vs_canyon`, `vs_remains` (lit, prelit, alt
   builders) key frames and over a 2000-frame fight segment every 10th frame; `--state-hash`
   equal; `hlr-audit` 0 / 0 on all 12 stages.
6. Restore handling (drop frames on `ss_restore`), `test_savestate.sh` with HLR on.

### Phase 2b: stage in real 3D (1-2 weeks)

`hlr_gpu.c` scene pipelines (depth, MSAA, 15-bit / RGBA8), `hlr.glsl` + generated headers,
float camera (4.1), fog, background gradient, host frustum culling at the wide aspect,
`stage_render` skips the shadow pass while the stage capture is on, side-car. Acceptance:
visual diff against `--hlr off` on the 12 stage key frames (`tools/hlr_diff.py` writes
`frame_F_hlr.png`, `_orig.png`, `_diff.png`; the differences must be only where the painter's
sort was wrong, at polygon edges (sub-pixel), and in fog rounding; a contact sheet in the PR);
`test_widescreen.sh`-style run with the stage in 16:9 (no cracks: `check_ws_cracks.py` on HLR
dumps); the determinism table of 3.4.

### Phase 3: characters (3 weeks)

Wrapper `f_80037144` (parts, bones; per-part light matrices: `bone_light_matrices` 800338d8
writes LLM / LCM into the GTE before each part's NCCT, so a wrapper on 800338d8 (or on
`char_mesh_render` 80036b0c per part) reads them from the GTE control registers right after
`_orig` returns (verified they are written there); recomputing them from `char_lights` /
`dyn_light` (likely layout) is a later step, checked against those registers in audit mode),
face lists from the existing 80036138 / 800363c8 wrappers (or re-read from the model block),
skinning pipeline, float backface test,
`--hlr-bones game|float|smooth` per phase 0 B. Audit: the host RTPT of every vertex of every
part must reproduce `char_vertex_buffer` 800c9d38 words and NCCT the `char_colour_buffer`
800cb7c8 words (the data the game itself uses), and the kept face set must equal the integer
NCLIP result. Acceptance: audit 0 on the 12 `practice_<char>` replays (every character's whole
move set); `flicker_metric.py` and the head-flip % on the Milena repro at least as good as
PGXP + char_mesh.c today (1.89 % / max 3.65) and, if `float` or `smooth` passed phase 0, clearly
better (target: < 1 %); contact sheets of every character at near / far from `movelab.py` poses.
`char_mesh.c` extras skipped for captured fighters; `pgxp-mesh:` line reports 0 extras then.

### Phase 4: shadows (days)

Wrapper `f_8007ec5c` (after `_orig`: the 45 world vertices and 64 triangles with greys are
final in RAM), decal pipeline, blend reverse-subtract, depth bias. Audit: host RTPS of the 45
vertices == `shadow_screen_verts` 800e6f80. Acceptance: audit 0 on the replay library; visual
check on slopes (Hills, Canyon) where the painter's sort used to pop.

### Phase 5: effects (2-3 weeks)

Ten wrappers (one per builder; `a0`-`a3` plus the object they were called with: the argument
layouts must be read in REA first, ~2 days), the e1 draw-mode word that precedes each packet in
its OT slot (8004e0e8 and the overlay writers, verified to be the only overlay contribution),
the OT-slot sort, the transparent pipeline with the four blends. Audit per builder: host RTPT /
RTPS of the object's points == the packet XY words; the OT slot == the game's. Acceptance:
audit 0 on all 12 `practice_<char>` replays (every projectile and magic of every character) and
the VS library; contact sheets of each character's signature attacks; the widescreen edge no
longer pops effects (visual).

### Phase 6: motion 60 Hz (2 weeks)

Over-layer in hires_gpu.c (RGBA16F, premultiplied blends, compose pass in MSL + GLSL), `under`
copy, re-render at N + 1 with `interp` / `extrap`, keys for item matching, side-car history.
Acceptance: `--state-hash` equal on / off (the whole feature is presentation); a windowed capture
of 120 presented frames shows 60 distinct HLR frames; `--renderer compare`-style per-pixel
difference between "composed at N via the layer" and "drawn straight into the target" on fight
frames < 0.01 % of pixels off by > 1 step (HUD blend chains); the smoothness line
(`smooth:` in netplay) unchanged in a Mac <-> Mac loopback session with one peer on `extrap`.

### Phase 7: story scenes (1 week + RE)

The stage path already covers EVENTOVL scenes (`story` scenario: 800401f8, 8003fcb8, 800433c4
seen). Characters and camera there: trace which EVENTOVL functions draw (phase 1's packet
attribution method on the `story` and `story_continue_yes` scenarios, extended past the first
fight with `story_win_first_fight`), then either they call the main-exe character renderer (then
phase 3 covers them) or they have their own (capture or leave on packets). Acceptance: audit 0
over the story scenario; state hash equal.

### Phase 8: polish (open)

Texture dump / replacement (4.4), per-pixel lighting, projected shadows, `--hlr-near clip`
review per stage, per-stage sky as a real skybox (the two quads), VRR present rate. Each its
own PR with the same determinism table.

---

## 6. Risks, unknowns, decision points

| Risk / unknown | Level | Kills what | Retire it by |
|---|---|---|---|
| Stage format / builder rules not fully understood (0x2000 alt builders, UV animation table, flags) | likely | the data model; everything | **Phase 0 A** (first): audit probe, < 0.1 % mismatches on all 12 stages |
| Characters still shimmer from the game's bone math | verified cause; fix is a guess | the "PC quality characters" promise, not the plan | **Phase 0 B / C**: workm vs float chain vs smoothed, Milena repro metric |
| Bone part layout (0x70, word 0 -> GsCOORDINATE2, +0x64/+0x68) | likely | phase 3 | phase 0 C RAM-dump script; phase 3 audit (vertex buffer words must match) |
| Effect builder argument layouts | not read | phase 5 | REA decompile of the 10 builders (2 days) + audit |
| Composition inside hires_gpu's batching (render-pass break with an external pass; MSAA resolve into the same target; 15-bit target as MSAA colour) | runtime | phase 2 | first week of 2a: a dummy external pass that clears a rectangle; `SDL_GPUTextureSupportsSampleCount` for B5G5R5A1 on Metal / MoltenVK / D3D12 (fallback: MSAA only with RGBA8, documented) |
| Side-car in snapshots (netplay ring size, restore ordering with `hires_reseed`) | runtime | one-frame look change after rollbacks | phase 2a minimum (drop) first; side-car second; `test_savestate.sh` dump equality |
| Re-simulation window (3 vblanks) too short for a capture before the presented draw | runtime | one-frame fallback picture after a rollback | count in `test_netplay.py lan_hlr` how many presented draws had no capture; raise to 4 if > 0 |
| Depth-buffered look differs from the painter's look where the game relied on it (billboards cutting walls / floor, decals, near walls) | design | acceptance by eye | per-effect depth-test flag; `--hlr-near ps1`; contact sheets per stage / character in each PR |
| Over-layer loses per-op clamping of chained 2D blends (phase 6) | design | 60 Hz fidelity on HUD | measure on fight frames; keep the layer only at N + 1 if visible |
| Audit needs bit-exact host GTE (gte.c refactor touches a covered, hot file) | runtime | audit precision | `gte_exec_on` swaps state around the unchanged implementation (no change to the hot path); unit test of equality |
| Story / EVENTOVL characters have their own renderer | unknown | phase 7 scope | packet attribution on the story scenarios |
| Performance of the capture (host time per update) | low | 60 fps on slow hosts | budget 0.3 ms per update; measure with `--segments`; the governor already handles the GPU side |

---

## 7. Testing and tooling

- **`tools/pr_check.sh` group `hlr`**: triggered by `runtime/hlr/*`, `runtime/overrides/hlr_hooks.c`,
  `runtime/hlr_hook.h`, `runtime/shaders/hlr*`, `tools/test_hlr.sh`, `tools/hlr_*.py`. Runs
  (<= `PR_JOBS` games, `nice`): `test_hlr.sh` with `CASES="hash exact audit"` = one state-hash
  equality run (`vs_hills` key segment, `--renderer gpu --hlr on` vs off), one `--hlr exact` dump
  equality on 20 frames, one `--hlr audit` on `vs_canyon` (alt builders) and `practice_milena`
  (characters) key ranges; plus CTest `hlr` and `hlr_gpu`. ~3 minutes. Changing `hires_gpu.c`
  (renderer group) also runs the `hash` case since the external pass lives there.
- **Full suite additions** (`run_tests.sh`): `test_hlr.sh` all cases (12 stages exact + audit,
  the 12 practice replays audit, savestate rewind dumps, 16:9 crack check on HLR dumps),
  `test_netplay.py lan_hlr`, `replays.py run` with HLR on for 3 replays, `scenarios.py run --all`
  with `--hlr audit`.
- **Scenarios**: every VS stage scenario already exists (`vs_<stage>`, `hires`-tagged); add
  `hlr` tags that `scenarios.py hlr` runs with `--renderer gpu --hlr on` (state hash equal, audit
  0, non-black key frame from the HLR). Characters: the 12 `practice_<char>` replays cover every
  move; no new scenario needed. Story: `story`, `story_continue_yes`.
- **CTest without a disc** (`runtime/tests/test_hlr.c`, `test_hlr_gpu.c`; CI Windows / Linux):
  - `gte_exec_on` restores the guest GTE; host NCDS / NCCT / RTPT / DPCS equal the guest results
    on random registers (links gte.c);
  - float camera vs integer GTE projection on random vertices: within 0.75 px and never
    clamped where the GTE did not clamp;
  - stage root / object / command decoding on a synthetic RAM image (the test builds a tiny
    "stage" with each builder type, flags, an animation table) -> expected item list; same for a
    synthetic model block with 3 parts and faces across parts; shadow mesh; billboard offsets;
  - packet set / suppression filter; frame pairing by idx + counter; restore drops frames;
    side-car round trip;
  - `test_hlr_gpu` (skip 77 without a device, like `hires_gpu`): a synthetic frame rendered in
    `exact` mode through the hires pipelines equals the same primitives sent directly (0 pixels);
    the 3D pipelines render a known cube at a known camera and the readback has the expected
    pixels at a few probe points on Metal / Vulkan / D3D12 / WARP; MSAA resolve works on the
    15-bit and RGBA8 targets; shader headers fresh (`gen_dxil.py --check-fresh`).
- **Visual tooling**: `tools/hlr_diff.py A_DIR B_DIR OUT` (per-frame diff PNGs + a table of
  differing-pixel %, > 1 step %, PSNR, reusing the `hires-compare-steps` definitions),
  `tools/contact_sheet.py` for PR screenshots, `tools/flicker_metric.py` for characters,
  `tools/check_ws_cracks.py` for widescreen.
- **Netplay**: `lan_hlr` (one peer HLR, one software), and a Wine case in `test_netplay_wine.py`
  only when the D3D12 path is exercised by hand first (CrossOver, `--renderer d3d12 --hlr on`,
  not in `run_tests.sh` until it passes twice).
- **Machine policy** as in AGENTS.md: the `hlr` group is light; the HLR full-suite additions run
  one step at a time under `JOBS`.

---

## 8. Open questions for the user

1. **Faithful or enhanced by default?** Once the stage / characters match the original look in
   3D, should the defaults stay PS1-faithful (per-vertex GTE lighting, PS1 near-plane drop rule,
   nearest textures) with enhancements opt-in, or should enhancements (per-pixel lighting,
   anisotropic / mip textures, `clip` near plane, MSAA 4x) be on by default when the GPU renderer
   is active? Proposal: faithful defaults through phase 5, revisit at phase 8.
2. **Motion**: `interp` (half an update, 16.7 ms, of extra visual latency; exact in-between
   poses) or `extrap` (no latency; overshoot on reversals) as the default, and should netplay
   force `extrap` / off?
3. **Character jitter fix**: if phase 0 shows that only *smoothing* removes it (not the float
   chain), is a presentation-only temporal filter with a small lag acceptable, and in netplay?
4. **Where the HLR switch lives**: a separate "3D Scene" submenu (proposed) or folded into
   View > Renderer as "Native 3D (experimental)" with the per-subsystem items hidden behind a
   developer flag?
5. **Texture replacement**: wanted at all (it invites a modding workflow and a folder of
   game-derived PNGs on the user's disk), and if so, content-hash file names (proposed) or
   page / CLUT coordinates?
6. **Widescreen shadow pass**: once the stage capture is on by default, keep maintaining the
   sandboxed shadow pass for the Software renderer, or declare 16:9 stages a GPU-renderer feature?
7. **Scope of "story scenes"**: is phase 7 worth its RE time (overlay character / camera code),
   or is "stage in 3D, characters from packets" enough there?
8. **Priority of phases 3 vs 5**: characters first (the shimmer complaint) or effects first
   (completes the scene for depth correctness)? Proposal: characters.

---

## Appendix A: corrections to `docs/HIGH_LEVEL_RENDERER.md`

- "Why not draw from the packets with a depth buffer": add PR #35's result: a per-pixel depth
  test on the character packets does not reduce the head shimmer (2.86 -> 2.93 %); the cause is
  bone-chain quantisation, so only a renderer that re-derives or smooths bone transforms can
  improve it (section 4.2, phase 0 B).
- Backend interface sketch: `HlDraw.pos / nrm / uv / idx` must not point into guest RAM (stale
  after a restore / rollback); use the host cache keyed by (address, length, hash) and copies for
  per-update data (section 2.1).
- "Textures ... sampled the way hires_gpu.c already does": use the texture cache layers for the
  3D path; the exact / audit path uses the VRAM decode (section 4.4).
- Phase estimates: the backend is 3-4 weeks (three shader formats, composition, MSAA, the
  over-layer), not 1-2; add phase 0 and the 60 Hz phase.
- "Effects: depth test on": per-builder flag, decided by looking (section 4.5).
- The "PS1-exact compare mode" becomes `--hlr exact` + `--hlr audit` built on a host GTE
  evaluator (section 3.2), which is both cheaper (no second renderer) and stricter (word-level).
- Snapshot side-car: now specified as `ss_add_sidecar` (section 3.3), with the "drop on restore"
  fallback as the first deliverable.
