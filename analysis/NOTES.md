# Destrega reverse-engineering notes

Running knowledge about the game, built up as a side effect of other work. Per-address facts
(one line each) live in `analysis/symbols.txt`; this file holds what doesn't fit on a line:
how subsystems fit together, data layouts, the cleanup priority list and open questions.
Runtime/emulator design belongs in `AGENTS.md`, not here: this file is about the *game*.

Keep it short and true. A confident wrong note costs more than a missing one.

## Confidence

Every fact here and in `symbols.txt` carries one of three levels. Pick the lowest that applies.

- **verified**: you observed it at runtime or it is unambiguous in code you read in full.
  Examples: a probe or trace showed the value/call; an override passes `--verify-overrides`;
  a test asserts it; the function is short and its decompilation leaves no doubt.
  Say how (the `evidence` field in `symbols.txt`; a few words here).
- **likely**: strong static evidence that was not checked at runtime. Examples: the REA/Ghidra
  decompilation reads clearly but you did not trace it; a name inferred from a library
  signature or the strings it uses.
- **guess**: a hypothesis from indirect evidence (call-stack position, timing, which data it
  touches). Write it down if it would save the next person time, labelled as a guess.

Rules:
- Never raise a confidence level without new evidence; when you do, update the evidence too.
- When you find something wrong, fix or delete it in the same change. Don't leave both versions.
- "Unknown" is a fine answer: record an address with name `-` and what you saw.
- Distinguish what the game does from what our runtime does ("the game draws X" vs "gpu.c does Y").

## Where this came from

Seeded from the original port's `AGENTS.md`, the hi-res rendering work (PR #2) and the
readable hit-detection override (PR #3).

## Game structure

- Boot: Psy-Q crt0 (`800a44e8`) -> `main` (`8002b80c`). The whole game runs from `main`'s loop;
  modes (title, menus, fights, story events, options) are overlays loaded into two fixed slots
  (P1 `80010000`, P2 `8001a268`) by three loader functions (see `symbols.txt`). Which loader
  handles which mode: **unconfirmed**.
- Frame rate: fights update at 30 Hz by design (`frame_sync`, VSync(2)). (verified: see
  AGENTS.md). Menu/title update rate: not measured.
- Fight call stack (verified, from guest stack dumps at the end of headless fights):
  `main > 8006b35c > 8006acc4 > 8002c820 > 8002beb0 > 80037280 > 80037144 > 80036b0c > 800363c8`.
  None of the inner frames is identified yet; the per-frame fight update is somewhere in here
  (guess).

## Rendering (what the game sends to the GPU)

All verified with GPU command logging during title -> 1P battle -> fights (hi-res work):
- Two 512x240 framebuffers at VRAM (0,0) and (0,240), alternated; draw area 512x232.
  Display is 512 wide shown at 4:3, so pixels are non-square (hence the 4x6 hi-res scale).
- Textures and CLUTs live at VRAM x >= 512. No primitive ever samples the framebuffer region.
- VRAM->CPU reads are small (3x12) in the texture/CLUT area; VRAM->VRAM copies are glyph tiles
  plus a 2x1 (0,0)->(0,0) no-op copy every frame.
- FMV: MDEC slices uploaded to the framebuffer as 24x240 strips, 24-bit display, 15 fps intro.
- Each fight frame starts with a flat untextured triangle in a light cyan (192,255,255) over the
  lower screen (1x y 116..235), drawn before the stage. Any hairline gap between stage
  polygons at high resolution shows this colour. (verified: pixel probe, Celia's stage)
- Character shadows/effects include thin additive (B+F) gouraud slivers with near-black vertex
  colours; their colour gradient overshoots badly off the 1x sample grid (fixed in hires.c by
  clamping to the colour range the 1x draw produced). (verified: pixel probe)
- About 21% of textured triangles cover no 1x pixel at all (mostly character-mesh slivers).

## Camera, projection and culling

From the widescreen work (REA decompilation of the functions named in `symbols.txt`, a GTE
control-register probe, and the widescreen tests).

- Screen space is centred: the GTE screen offset OFX/OFY is 0 throughout boot -> title -> fight
  (verified: probe of ctc2 writes, none changed them). The GPU drawing offset must therefore add
  the screen centre (256, 120) (likely: implied by every culling bound below and the 512x232 draw
  area; not probed).
- The projection plane distance H is the camera zoom: `SetGeomScreen` (80095f98, called from
  80093d08) rewrites it while the stage intro and fight camera move, between about 0x38a and
  0x3bb (verified: probe; 0x3e8 at boot).
- **Stage** (`stage_render`, 80044210): per stage object, `stage_object_visible` (80043fd4)
  projects 4 bounding points and rejects the object unless max x > -257, min x < 257,
  max y > -121, min y < 121; then one builder per packet type (8003fcb8 .. 80041c14, plus the
  flag-0x2000 variants 800426c4/800433c4 with their helpers 800423e0/80042fec) rejects each
  polygon unless max x > -255, min x < 255, max y > -119, min y < 119 and its largest SZ > 0x18f
  (near plane). Survivors get depth-cued colours (NCDS/DPCT) and are linked into the ordering
  table. (likely: decompilation; the bounds are verified in the sense that widening exactly
  these 50 immediates fills the widescreen margins, see `analysis/widescreen_cull.txt`.)
  The builders write only the header, colours and positions of each packet; the UV / CLUT /
  texpage words are not written per frame (likely: packets are pre-initialised per polygon).
- `stage_render` also advances per-object texture animation counters, but only for objects that
  passed the visibility test (likely: decompilation, the 0x80 flag block). So the visibility
  test changes RAM, though only render state.
- **Characters** are never culled by screen position: the mesh code (80036b0c -> 80036138 /
  800363c8) does a GTE NCLIP backface test only (likely: decompilation).
- **Ordering table**: 2048 slots per buffer, pointer in `ot_pointer_table` (800c0724, indexed
  by the double-buffer index at scratch 1f800000). Depth -> slot is a 3-segment piecewise-linear
  map of SZ using scratch 1f80000c (near), 1f800010 (mid), 1f800008 (far): 0x155 slots up to
  near, 0x400 up to mid, 0x2ab up to far, then slot 0x7ff. `fighter_ot_depth_range` (80035594)
  sets near/mid from the fighters' depths each update (only for fighters within x +-257 /
  y +-121), so the fighters get the finest depth resolution. (likely: decompilation)
- Other screen-space tests seen but not used for culling the stage: `sound_pan_from_screen_x`
  (80028c70) pans sound effects by projected x (|x| <= 256); several HUD/menu functions compare
  against 120/121/256 (not investigated: a scan for `slti` with these immediates lists ~40).

## Fight state (what gameplay is made of)

Fighter structs at 800c9938 (P1) and 800c9b14 (P2), 0x1dc bytes each (verified: probe of
health; layout partly likely). Known fields: +0x8 u8 (alive/active flag, likely), +0xc s16
health (200 at round start, falls on hits; verified probe), +0xe s16 gauge (likely), +0xa0..+0xac
position vector (s32 x, y, z; likely), +0xc8 the model/camera block passed to the renderers.
Round globals: `round_time` 800e379c, `round_wins` 800e3788/800e378c, `round_start_hp`
800e3780/800e3784, `time_limit_setting` 800e344c, `game_mode` 800e3794 (guess). RNG: Psy-Q
`rand` (8008d198) with `rand_seed` at 800e7560 (verified: code); the round-end code calls it.
Whether other RNG sources exist: unknown. `--gameplay-trace FILE` prints these per frame.

## Collision

- Stage collision is a separate list of polygons (up to 4 vertices, precomputed 4.12 normal),
  not the render mesh. Moving spheres (characters, weapon tips) are tested against it by
  `sphere_vs_poly` (`800465a4`). Layouts in `runtime/overrides/collision.c`. (verified: override)

## Input / hardware use

- Pads: libpad drives SIO0 directly (not BIOS InitPAD). Memory card is reported absent, so the
  game never saves. (verified: runtime)
- CD: libcd at register level; XA audio and the 5 STR movies live in `XADATA.BNS`.

## Test coverage facts

- `tools/fight_script.py` reaches: title -> 1P battle -> the temple stage (Gradd vs Milena), then
  Celia's brick/green/crates stage from about frame 8000. It does not reach the canyon, castle,
  catacomb or Tieme stages, story mode, VS mode or options. (verified: dumps)

## Cleanup priority (readable C / overrides)

Highest value first. Move items to "done" with the PR that did them.

1. **Game-state root and per-fighter state struct** (fighters at 800c9938/800c9b14, round
   globals around 800e3780, see "Fight state"): needed for rollback (what to snapshot,
   what must be deterministic) and for any gameplay mod.
2. **Random number generator**: Psy-Q `rand` 8008d198 / seed 800e7560 found; rollback still
   needs to know whether there are other sources.
3. **Pad input -> fighter commands**: where inputs enter game state (rollback, input display).
4. **Camera, projection and GTE call sites** (RTPS/RTPT, screen offset/H; see "Camera,
   projection and culling" for the stage and character renderers): PGXP and
   widescreen work here.
5. **Visibility culling / off-screen tests** (stage: done for widescreen as a shadow pass, see
   AGENTS.md "Widescreen"; a readable `stage_render` would let it widen culling natively, and
   effects may have their own tests): widescreen must widen these without touching
   gameplay.
6. **Fight frame update** (somewhere under `8002beb0`, see the call stack above).
7. **Overlay loaders** (`8004c1e8`, `8003e36c`, `8006b35c`): which mode loads which file.

Done:
- `sphere_vs_poly` (`800465a4`), PR #3.

## Open questions

- Purpose of the original port's extra seeds `800a8e28` and `800a9528`.
- Effects (projectiles, hit sparks) are drawn by other code (8004cb04, 80052240, 80055b70,
  80056704 project vertices) whose culling, if any, was not examined: they may pop in at the
  widescreen edges.
- Does any gameplay code depend on the GTE results beyond the screen coordinates it draws
  with (e.g. on-screen tests that change behaviour)? Matters for widescreen and PGXP.
