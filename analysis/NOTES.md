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
  handles which mode: **unconfirmed**. Observed from the CD log (verified): a fight loads `Mxxx.BIN` (P1)
  then `Mxxx2.BIN` (P2); STORY loads `EVENT_SP.BIN` + `EVENTOVL.BIN`; OPTION loads `OPTOVL.BIN`.
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
- Command mix over a 7000-frame headless run (boot -> 1P battle -> fight, `tools/fight_script.py`;
  verified: counted in the GPU backend work): ~3.69M triangles; semi-transparent ones by mode:
  B/2+F/2 (mode 0) 0, B+F 57917, B-F 244204, B+F/4 4080, so subtractive blending dominates.
  No primitive ever sets or checks the mask bit, and no lines are drawn. 37622 rects, 3549
  fills, 3638 VRAM->VRAM copies (3547 with the source inside the framebuffer region), and
  29.6M CPU->VRAM upload pixels (29.26M inside the framebuffer region, i.e. mostly FMV).

### Rendering: what the 1x VRAM holds (runtime `gpu.c` skip rule, `--gpu-1x`)

The game never reads back what it draws into the framebuffer region (0,0)-(511,479):
- No VRAM->CPU read, texture page, CLUT, or VRAM->VRAM copy source touches drawn framebuffer
  pixels, and no primitive checks or sets the mask bit (verified: gpu.c's tile detector, which
  logs every guest observation of a skipped tile, reported 0 observations in all 24 scenarios
  (`scenarios.py run --all`), the 30 library replays and the Hills match replay; 1.0-1.2 M
  primitives skipped per scenario).
- The only copies with a framebuffer source are the 2x1 (0,0)->(0,0) no-op per frame, which stays
  inside the region (verified, same instrumentation).
- GPU timing is not emulated (always ready), so drawing less cannot change emulated time.

So since the performance work, the emulated VRAM no longer receives the pixels of polygons, rects
and lines whose draw area lies inside 512x480. Fills, uploads (FMV) and copies are still written.
Evidence that nothing else changed (verified):
- `--gpu-1x draw` (rule off) gives the per-frame `--state-hash` of the build before the change,
  byte-identical over the 11530 frames of `match_gradd_celia_hills`.
- With the rule on, final RAM (`--ram-dump`) is identical to the old build, and every gameplay
  field hash of all 30 replays is identical. Only the complete state hash lines (`s`, which hash
  VRAM) changed: 20 of each replay's lines before the first drawn frame stayed equal.
- Rasterizing these pixels was 49 % of the main thread in a plain 1x run and 14 % in the user
  configuration (macOS `sample`).

### Host profile after the performance work (macOS `sample`, M1 Pro, machine under load)

Main-thread self time by area, Hills fight (verified by sampling; percentages shift with load):
- Plain headless 1x (`--gpu-1x skip`): recompiled guest code 33-38 %, SPU 21-24 % (almost all
  `spu_render`: voices + reverb), GTE 20-22 %, CD/XA 8 %, GP0 decode 5 %, bus 3 %, 1x rasterizer
  (UV/colour tracking only) 2 %, PGXP 1 %.
- User configuration (GPU 4x6, 16:9, xbr, full colour, PGXP): waiting on the GPU fence 59 %,
  guest 9 %, hi-res backend 9 %, GTE 5 %, SPU 4 %, widescreen shadow pass 4 % (0.46 ms per
  pass: snapshot, wide run, restore, diff), 1x tracking 3 %. Wall time is GPU-bound there.
- Next candidates: the widescreen shadow pass's full RAM/scratchpad copies (a write journal would
  avoid them), the per-voice SPU loop, and the per-function `CYC`/`EVT_CHECK` overhead in the
  generated code (must keep cycle counts identical).

### What the render-quality modes see (runtime work, `--tex-filter` etc., AGENTS.md "Render quality")

- The fight HUD (name lettering "GRADD"/"ANJIE", health-bar frames, round markers, "PUSH START")
  is drawn with 2D primitives: rects or screen-aligned polygons whose u/v advance exactly one texel
  per 1x pixel. With `--tex-filter xbr` it takes the 2D (edge-aware) rule and stays sharp, while
  the stage and the characters around it come out bilinear. (verified: 4x6 dumps of fight frame
  4200, every mode, compared by eye; the lettering is not blurred in xbr mode, it is in bilinear)
- Count over the 7000-frame boot -> fight run (`tools/fight_script.py`, software 4x6, PGXP on;
  `hires-filter:` summary line): 3,667,527 textured triangles, 352,532 of them (9.6 %) 2D by the
  1:1 u/v test, plus 39,316 textured rects. (verified: counter in hires.c be_tri / be_rect)
- Stage and character textures are noisy photographic 1x art with no flat-colour edges, so an
  exact-colour edge rule finds nothing there; bilinear is what removes the 4x6 blockiness.
  (verified: same dumps)
- The floor/wall tile grout lines are part of the textures, not polygon seams: filtering keeps
  them where nearest has them, with no lighter/darker seam at polygon boundaries thanks to the
  footprint clamp. (verified: pixel columns across a tile boundary, base vs bilinear, frame 4200)
- Gradients that gain most from full-precision colour: the additive hit glows (B+F) and gouraud
  character shading; the stage has little fog. (likely: visual comparison only)

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
Verified by trace over the recorded replays (`tools/replays.py`, 6 fights incl. a full best-of-3
match): health is 200 at each round start and hits 0 exactly at the KO; the other fighter's
`round_wins` goes +1 at that KO and both reset to 0 after a fighter reaches 2 wins (match over);
`round_time` goes +2 per update and freezes from the KO to the next round; the gauge stays in
0..1800 and drops sharply ~30 times per 3000 random fight frames (attack costs, see "Gauge and command inputs"). The rest of
the fighter struct (0x1dc bytes) is covered by the replays' struct hashes but not mapped.
Rollback-relevant: gameplay state reacts to pad input within one frame (an edited input at frame F
changes the fighter struct at the start of F+1).
### GTE projection call sites (PGXP work)

- 34 functions contain RTPS/RTPT (`symbols.txt`, evidence `code`): two families of small
  per-primitive-type renderers (`8003fcb8..80041c14`, 6 functions with 2 RTPT each, and
  `80055820..80057100`, 14 functions with 1 RTPT each, lit variants use NCDS/NCCS), plus
  single sites (`80036b0c`, `8007ec5c`, `80028c70`, ...). They use the scratchpad
  (`0x1f800000`) as work area. (verified: generated code)
- The usual pattern is RTPT, then `swc2 SXY0..2` **straight into the primitive packet**
  (offsets 8/20/32 for GT3-like, 8/12/16 for F3-like), followed by integer compares of the
  stored X/Y for clipping/culling; some sites store to the stack first and copy with lw/sw.
  (verified: generated code; the copies are handled by the PGXP lw/sw hooks)
- Producer probe, fight frames 3700-4400 (Gradd vs Milena, temple stage): of the polygon
  vertices that reached the GPU with a tracked GTE origin, ~63% came from `80036b0c`, ~16% from
  `80041c14`, ~10% from `8007ec5c`, ~1.3% from `8004163c`, the rest from the `80055xxx` family.
  ~95% of all polygon vertices in those fight frames carry a valid precise value; over the whole
  boot->fight run it is ~62-65% (title, menus and character select draw 2D polygons). Only
  ~0.5% of the misses equal a recent GTE result, so nearly all misses are CPU-computed 2D
  polygons (HUD, menus), not lost associations. No stale-tag misses observed. (verified: probe)
- **Superseded (see "Character mesh and PGXP" below):** the next bullet's explanation was wrong.
  The holes were the faces the integer backface test drops, not duplicated vertices; the
  character mesh is no longer welded. Kept for the history of the measurement.
- Character meshes depend on integer snapping (verified: dumps + `pgxp-check`). Over a boot->fight run
  ~24% of precise vertex uses (572K of 2.43M in 4300 frames) share their SXY word with another use that has a *different*
  precise value (all 388 logged samples came from `80036b0c`, e.g. word 48,-24 at
  0.92,0.66 and 0.24,0.93). They are distinct GTE results (separate RTPT calls for neighbouring
  pieces of hair, face, shoulder pads, limbs) that the PS1 merges by rounding. Drawn at their
  precise positions the pieces separate by up to 1 px: the hair breaks into spikes with gaps,
  the face behind shows through as a dark smear, shoulder/arm pieces look torn (fight-script
  frame 4209 at 4x6). Welding equal words per batch (`pgxp_weld`) removes it. Whether the mesh
  data really duplicates shared vertices per piece (the likely reason) is a guess: the mesh
  format is not decoded yet.
- Different meshes also share SXY words by coincidence (verified, fight-script frame 5338: word
  100,-94 from the stage path `80041c14` at w=9073 and from `80036b0c` at w~3065). Welding by
  word alone gave the character triangle the stage vertex's W (texture warp; caught by the
  depth-ratio check). The weld is therefore keyed by word *and* similar depth (ratio <= 1.25).
  Single-site triangles spanning a depth ratio > 2 are legitimate (stage `80041c14` near the
  camera, w 830..1812 with clamped off-screen vertices), so the check flags only mixed-site ones.
- GPU backend with PGXP (renderer facts, not game facts; verified by `--renderer compare`): the
  fight geometry is 80% precise vertices, so 2.75M of the triangles in a 7000-frame run take
  the shader's exact-coverage path instead of the GPU rasterizer (232K with `--pgxp off`).
  Perspective u/v in float32 differed from hires.c's double in 12.5K pixels over the fight
  frames; with 2^-38 fixed point that drops to 29.
- Stage walls facing the camera give triangles whose three precise vertices have exactly the
  same W (verified: debug print, fight-script frame 5040, the arch at the right edge: q = 1 and
  zero q gradient), so their texture mapping is affine and texel ties on whole rows are common.
  Real-pass stage polygons reach well past the 4:3 edge (1x x 529..593 in that frame): the
  culling keeps a polygon if any part is inside x +-255, so these are halves of quads straddling
  the edge (likely). With PGXP and widescreen both renderers now take the affine path for them
  (AGENTS.md "PGXP").
- PGXP never changed the per-frame state hash, i.e. no game code observes the precise values;
  this says nothing yet about whether gameplay depends on the *integer* GTE results (open
  question below).

### Character mesh and PGXP (the "shimmering polygons" report)

User report: at 4x6 with PGXP on, fighters' polygons "shift around a ton" from frame to frame,
faces look like a jumble of facets, limb pieces poke through each other, worse at a distance;
also there with `--pgxp off` but much worse with PGXP on.

- **No level of detail.** `fighter_render` (80037144) always draws the mesh at fighter +0xe8 (and
  the attachment at +0x1a0); `char_mesh_render` has no distance test. Over `practice_milena`
  the same 4 triangle face lists with the same counts were used at every depth from SZ 525 to
  23022 (verified: override probe). The "lower-poly" look at a distance is the integer culling
  below, not another model.
- **Mesh layout** (verified: override reads every face each call and the rebuilt faces render
  correctly; decompilation of 80036b0c / 80036138 / 800363c8): per bone part the vertices are
  projected once with RTPT into the vertex buffer 800c9d38 ({SXY, SZ}, 8 bytes) and lit (NCCT)
  into 800cb7c8; faces reference both by s16 indices, so vertices are shared between faces and a
  face may use vertices of two parts (joints are closed by topology, not by rounding). Face
  records and packets: symbols.txt 80036138 / 800363c8.
- **Integer backface test** (verified): a triangle is kept only if NCLIP > 0 on the integer SXY
  words; a quad if NCLIP(D,B,C) < 0 or NCLIP(A,B,C) > 0. A face whose integer projection is
  degenerate (all on one line or one pixel) is dropped. On the PS1 that costs nothing (it has no
  area at 1x); at precise positions it does. Share of the faces facing the camera (precise NCLIP)
  that the game drops, by face depth SZ, `practice_milena`, 9980 frames (verified: probe):
  2000-6000 (near / mid) 17-28 %, 6000-10000 29-33 %, 10000-16000 (far) 41-59 %, about 40 % over
  the run. They change from frame to frame as the pose and camera move.
- **What PGXP made of it** (verified: probe + dumps): the hi-res target drew the kept faces at
  precise positions, so the dropped ones were holes through which the faces behind showed (far
  arm, inside of the hair, the other side of the body), flickering every update. That is the
  "cracks / torn limbs / dark smear" that welding was added for. Welding hid part of it by
  snapping every vertex sharing an SXY word with an earlier one (at similar depth) to that first
  value; a distant fighter has many distinct vertices per pixel, so ~28 % of the precise vertex uses moved
  by up to ~1 px (1x, i.e. 4x6 hi-res px; median ~0.6 px), towards whichever vertex happened to
  come first: a different one every frame. The distance between the welded values has no peak
  near 0 (log histogram rising to 0.5-1 px; 3D distance 8-500 view units), i.e. they were not
  duplicates of one point. Also ~4 % of the kept triangles are back-facing at precise positions
  (inverted slivers). Integer snapping with `--pgxp off` moves each vertex by at most 0.5 px and
  keeps the integer mesh closed (degenerate faces have no area there), which is why off looked
  steadier than on.
- **Fix (runtime/overrides/char_mesh.c):** before 80036138 / 800363c8 run, the same face list is
  read; every face the game is about to drop but which faces the camera at precise positions is
  rebuilt (the game's own packet words, UV words from the packet buffer, the game's OT slot) and
  drawn into the hi-res target only when the GPU DMA reaches that slot. Mesh vertices get
  `PgxpVert.cull`: they are not welded, and their triangles that face away at precise positions
  are left out of the hi-res target. Guest state is untouched (replays, state hashes).
  Measured with `tools/flicker_metric.py` (mean temporal second difference over the fighter,
  4x6 widescreen GPU, 30 updates; lower = steadier): idle Celia (timeout_draw 4300, mid-far)
  old on 1.34, off 0.98, new on 1.04 (pixels flickering by > 64: 317 / 245 / 193 per update);
  idle Gradd 4.05 / 3.98 / 3.87; walking Milena (practice_milena 8400) 3.04 / 2.80 / 2.87;
  close Milena (9600) 1.33 / 1.14 / 1.23 (verified: measurement; the metric includes the real
  animation, so it only ranks). By eye: faces, hands and limbs are whole and steady.
- Not covered: other meshes the game culls the same way (stage builders, effects) still lose
  their integer-degenerate faces at precise positions; they are large, so this was not visible.
  Whether `char_mesh_chains` (800367d8) vertices reach these face lists: guess (they are written
  to the same vertex buffer).

## Memory card

- Access path (verified: trace): never SIO0. A libmcrd-style library (`800a4xxx`-`800a7xxx`) calls
  the BIOS: InitCARD/StartCARD, `_card_info` (A0:ab), `_card_load` (A0:ac), `_card_write`
  (B0:4e, only to clear the changed flag), `_new_card` (B0:50) and the `bu00:` file calls
  open/lseek/read/write/close/firstfile/nextfile/erase/format (B0:32..45). Reads and writes are
  asynchronous (open mode bit 0x8000): the call returns 0 and the BIOS later posts SwCARD
  (f4000001) and HwCARD (f0000011) events (IOE 0004, ERROR 8000, TIMOUT 0100, NEW 2000) that
  the library receives through callbacks it registered at `800a6518..65a4`. One 128-byte sector
  per event, so a save of N sectors takes ~N frames. The library also walks the kernel DCB table
  (words at 0x150/0x154) looking for device "bu"; HLE must provide it (bios.c).
- What it saves (verified): MEMORY CARD > SAVE writes one 1-block file `BASLUS-00732DESTREGA`
  (8192 bytes, standard `SC` icon header; title in Shift-JIS "DESTREGA: TRY STORY MODE" for a
  fresh game, other titles "NEXT SCENE" / "STORY MODE CLEARED" exist in the message table). Data
  starts at file offset 0x200 (1 checksum byte, then the option block: ROUND COUNT verified at
  +8; story/unlock progress is presumably in the rest, unverified). The game also reads the first
  6 sectors at boot (frame ~100) and applies them to the live option block `800bee64` without
  any menu action; MEMORY CARD > LOAD re-reads. AUTO SAVE ON/OFF is a menu item (not exercised).
- Startup sequence with a card: `_card_info` -> NEW, `_new_card`+write sector 63 (accept),
  `_card_load`, `firstfile`, `_card_info`, `_card_load`. Real cards report NEW until the first
  write; the runtime models that flag (memcard.c).
- Not exercised yet: second slot, FORMAT flow, full card, unformatted/corrupt cards, deleting.
  Unformatted card = ERROR event on `_card_load`; the game's messages for these are in the table
  at `800b5474`. (guess: untested in game)

## Collision

- Stage collision is a separate list of polygons (up to 4 vertices, precomputed 4.12 normal),
  not the render mesh. Moving spheres (characters, weapon tips) are tested against it by
  `sphere_vs_poly` (`800465a4`). Layouts in `runtime/overrides/collision.c`. (verified: override)

## Input / hardware use

- Pads: libpad drives SIO0 directly (not BIOS InitPAD). Memory cards: see "Memory card" below.
- **The game only uses a digital pad** (verified: temporary SIO0 probe in `sio.c`, boot -> title ->
  menus -> a fight, both ports). Every transfer is `01 42 00 ..` (poll); libpad never sends
  `43` (config mode), `44` (set analog), `4D` (rumble mapping) or `45..4C`, so it never switches a
  DualShock to analog and only the 16 digital button bits matter. The runtime answers as a digital
  pad (ID 0x41) and maps the host's left stick to the D-pad.
- From frame ~3034 (in a fight) port 0's poll carries `40 01` in the two bytes after the ID
  exchange, where a DualShock in analog/config-unlocked mode would read motor values; a digital
  pad ignores them. (likely: libpad's actuator buffer being sent, the game never enables the
  motors through `4D`, so there is no rumble to emulate; guess: the values are a default
  actuator table, not game intent). Port 1 was only probed in menus.
- CD: libcd at register level; XA audio and the 5 STR movies live in `XADATA.BNS`.

- **Netplay smoothness** (measured with `tools/netplay_smooth.py`, windowed, 60 Hz, on a VS
  replay's fight; this Mac was shared with other jobs, load average 130-340 on 10 CPUs, so absolute
  numbers are poor and only back-to-back pairs compare). "Before" = `--net-sync-hard
  --no-scale-governor` (the old behaviour), "after" = defaults:

  | run | held > 1 vblank | interval p50 / p95 | audio underruns |
  |---|---|---|---|
  | Mac<->Mac clean, before / after | 54% / 20-21% | 30.7 / 84 -> 16.1 / 38 ms | 1276 -> 47 |
  | Mac<->Mac 20+10 ms, 2% loss, before / after | 58% / 11-12% | 51 / 141 -> 17.0 / 28.5 ms | 1570 -> 3 |
  | Mac<->Mac clean, new sync only (no governor) | 51-53% | 26-29 / 61-68 ms | ~1100 |
  | Mac host <-> Wine joiner, before / after | 90% / 83% (Mac); 95% / 86% (Wine) | 106 / 264 -> 83 / 438 ms | ~2500 -> ~2100 |

  What held the frames: almost entirely "slow host" (the frame's own work was over budget), not
  rollbacks (re-simulation 4-11 ms avg, max 8-17 ms; a late frame from a rollback in 0-35 of ~2800)
  and not time sync. Even an offline single window at render scale 4x6 with the GPU renderer held
  48% of its frames under this load (2.3 ms/frame at 1x1), so the default hi-res scale is what does
  not fit when the machine is busy; the scale governor (smooth.c) fixes that and is the main gain.
  Gradual time sync removes the frames held by sync waits (9-12 -> 0 per run). Under Wine
  (software renderer) the joiner stayed at 60-80 ms/frame even at 1x1 under this load: that part is
  Wine/x86 emulation speed and the machine's load, not netplay; the Mac then waits for each late
  input (stalls 220-340 s per run) whatever we do. Headless Wine runs ~1.3 ms/frame when the
  machine is idle, so on an idle machine the Wine side should keep up at a low scale.

## Menus, modes, characters, stages (found while building `tools/scenarios.py`)

All verified by headless runs (dumps + `--trace` CD log), frame numbers are for the scripted boot
(Start at 1200 skips the logo; the title menu is up by ~1300). Scripts are deterministic, so the
frame numbers repeat exactly; they are not "game constants".

- **Title menu**: a *circular* 8-item list with the cursor in the middle, starting on 1P BATTLE.
  Down from there: STORY, VS, TEAM BATTLE, TIME ATTACK, ENDURANCE, PRACTICE, OPTION (then back to
  1P BATTLE). Up x1 from the start is OPTION. One Down/Up per tap; Start confirms.
- **Character select** (1P BATTLE, TIME ATTACK, ENDURANCE, PRACTICE, VS): ready ~250 frames after
  confirming the title item. The cursor starts on Gradd (P1) / Celia (P2, VS only); Right cycles the
  12 portraits: Gradd, Celia, Tieme, Anjie, Milena, Laon, Zauber, Rohzen, Falma, Doyle, Reyus,
  Cougar (portrait labels as printed; the overlays are named differently, see below). Cross confirms.
  MGENE/MSOLD overlays exist but there is no portrait for them (Soldier "Relics" is the first story
  opponent; Gene is unreachable so far).
- **VS needs pad 2**: after P1 confirms, the screen waits for pad 2 (the runtime's `--press2` feeds
  it). Then a STAGE SELECT map appears (~110 frames after both confirmed); Right cycles 12 stages:
  Hills, Cave, Villa, Canyon, Gate, Castle 1, Castle 2, Throne, Remains, Hamlet, Shrine, Fortress
  (names as printed under the thumbnails). Cross starts the fight ~350 frames later (BGM XA read).
  VS is the only mode that lets you choose the stage; every other stage can only be reached through
  it (1P/Time Attack/Endurance pick opponent and stage themselves).
- **Overlays per fight** (verified: `--trace` CD seeks mapped to the disc listing): P1's character is
  `Mxxx.BIN`, P2's is `Mxxx2.BIN`. Portrait -> file stem: Gradd MGRAD, Celia MSEREA, Tieme MTEEM,
  Anjie MANZEAL, Milena MMILENA, Zauber MZAUBER, Laon MLAON, Falma MFALMA, Doyle MDOIL, Reyus MREUS,
  Cougar MKUGA, Rohzen MROSEN (all twelve seen loaded as P1 and 11 as P2; Rohzen as P2 in `vs_villa`).
- **Stage data** lives in `DATA.BNS`: one ~49-sector pack per stage, read as two seeks at these sector
  offsets (inside DATA.BNS): Hamlet 7756, Fortress 7861, Villa 7973, Remains 8096, Hills 8212,
  Canyon 8329, Cave 8447, Gate 8580, Castle 1 8702, Castle 2 8831, Throne 8959, Shrine 9086 (second
  seek +49). Character data is read below 7700, event/story data above 9200. This is the "which stage
  is this" fingerprint the scenario checks use; no RAM address for stage/mode/character was looked for
  (unknown).
- **1P BATTLE / TIME ATTACK / ENDURANCE / PRACTICE**: after a character is confirmed with Cross the
  game picks opponent and stage itself. The same character gets a *different opponent/stage depending
  on the timing of the inputs* (Gradd met Milena on Villa in `fight_script.py`, Doyle on Villa in the
  scenarios). `800e7560` (32-bit; the Psy-Q `rand` seed, verified from the code, see "Fight state") looks like the game's RNG state: `--watch 800e7560`
  on `1p_gradd` shows it set to 0xa9 at frame 167, then changing at the first Start (~1405), the cursor
  moves and at the confirm (2157/2161) and every 2-4 frames once the fight loads. Likely (not traced to
  the opponent selection code): the opponent/stage come from this RNG, so any change in boot timing
  (anything that shifts when RNG calls happen) changes them. The scenarios therefore treat 1P / team /
  time attack / endurance / practice as `loose`: P1's overlay must load and *some* known stage pack
  must be read; only VS scenarios (stage chosen by input) and story (fixed) pin an exact fingerprint. Practice puts the player on
  the Hills field vs Celia (MSEREA2) with TOTAL/DAMAGE counters; Time Attack and Endurance have a
  countdown (55 -> ...) in the HUD.
- **TEAM BATTLE**: one pad fills both 3-man teams (Cross to take a slot, Right to move on; 6 Crosses),
  then STAGE SELECT, then "3 ON TEAM" fights.
- **STORY** (NEW GAME at the first menu, Cross): fully automatic after Cross: FMV intro, then a text
  crawl over a 3D hamlet (EVENT_SP.BIN + EVENTOVL.BIN loaded at ~6300), event scenes with 3D
  characters (~9500-14000), "BATTLE 1 Gradd vs Relics" (~14300, MGRAD + MSOLD2, Hamlet pack 7756) and the
  fight from ~14650. With no input Gradd loses and "CONTINUE?" appears ~16500.
- **Fastest power-on path to VS** (verified: headless dumps every 10-20 frames, `--boot-to-vs`, the
  netplay boot shortcut `k_boot_vs` in netplay.c): with no memory card, Start is accepted from about
  frame 200 while the KOEI logo fades in (presses at 20-140 do nothing); Start at 210 goes straight to
  the title with its menu already up at ~220 (the intro FMV never starts). Down at 240 and 270 (one
  item per tap, presses held 6 vblanks), Start at 300: the VS character select is on screen by ~380
  (Gradd vs Celia, "0-0 DRAW 0"). The scripted path of `scenarios.py` (Start at 1200, Down x2, Start at
  1480) reaches the same screen at ~1560, so the shortcut is 1180 vblanks (19.7 s) sooner. Start at
  240 instead of 210 also works; a second Start on the title picks the highlighted item at once.
- **After a VS match** (verified: `match_gradd_celia_hills` dumps, frames 7000-14000): after the
  winner's animation the game returns by itself to the VS character select, keeping a win counter
  under VS ("0-1 DRAW 0"); the next pick, stage select and fight follow as before. A rematch never
  needs the title screen.
- **OPTION** loads `OPTOVL.BIN`: GAME OPTION, CONTROLLER, SOUND OPTION, RECORDS, MEMORY CARD, EXTRA, EXIT.
  Circle backs out of a page. RECORDS shows the 1P-battle ranking; EXTRA has CHARACTER PROFILES / IN
  GAME CONTROL / EXIT.

## Gauge, command inputs, options and the round timer (found while widening the replays)

All from headless traces (`tools/movelab.py`, the `practice_*` / `timeout_*` replays); P1 on the left
facing right, `f` = Right, `b` = Left.

- **The fighter's "gauge" (+0xe) is an attack-energy meter, not (only) a magic gauge.** It refills 6 per
  vblank up to 1800 and every attack input that starts an attack takes ~588 off in one update
  (600 less the refill, so three attacks from full). Seen for Gradd: Square, Triangle, Circle with any
  direction (f / b / d / u), quarter circles (qcf / qcb) and dragon-punch motions with Square, Triangle
  or Circle all spent ~588 (and hurt the dummy 6..39 hp); Cross alone, shoulder buttons, button pairs
  spent nothing. An attack pressed with less than ~600 left does nothing. (verified: movelab probe on
  Gradd; whether other characters are the same is likely, the 12 `practice_*` replays have gauge drops
  of 33..39 per battery.) This replaces the earlier reading "drops ~30 times = magic spends".
- Motion inputs are read at 30 Hz updates; 6-vblank steps (the replay generator's tap length) work
  for qcf / qcb / dp / hcf. A battery of 84 inputs (taps, direction + button, qcf, qcb, dp, double
  taps, holds, button pairs) shows Gradd reaching 19 distinct animation pointers (struct +0x114) before
  the match ended; each attack button has its own entry and qcf / qcb / dp + Square add new ones, the
  directions alone do not (likely: the match ended after ~35 inputs, so Circle / pairs were not seen in
  that run; the practice replays run the whole battery on every character but no per-character move
  list was extracted). Specials / magic that need more than the battery (e.g. gauge-full or
  button-pair combos) were **not identified**: unknown.
- Fighter struct fields seen moving with inputs (guess, one character): +0x20 holds a latch of the
  pressed face buttons (16 Cross, 32 Square, 64 Triangle, 128 Circle); +0x114 / +0x138 (two copies) point
  to the current animation / move data in the character overlay's data (20 distinct values in 17000
  frames of the battery; 0 when the fighter is not in a fight); +0x34 a second pointer (6 values);
  +0x158 an attack class 1/2/4 (Triangle 1, Square 2, Circle 4); +0x188 a counter during attacks.
- **PRACTICE** (title item 7, character select as the other modes): the fight is up ~2700 frames after
  the title Start, 4:3 Hills vs a Celia dummy (`MSEREA2.BIN`) that stands at the far side and **refills to
  200 hp**, no KO, `round_time` keeps counting (verified: 12 practice replays, 0 KOs in ~7000 frames
  of attacks). Cheapest way to exercise a character's code for thousands of frames.
- **GAME OPTION page** (OPTION > first item): rows GAME LEVEL (NORMAL), ROUND COUNT (2), ROUND TIME
  (NO LIMIT), HANDICAP (OFF), QUICK SELECT (OFF), LIFE RECOVER (TIME), DEFAULT, EXIT. Right changes a
  value (ROUND TIME: NO LIMIT -> 30 -> 60 -> 99). `time_limit_setting` (800e344c) is 3 for NO LIMIT
  (the default; a VS fight never times out) and 0 / 1 / 2 for 30 / 60 / 99 s (verified with `--watch`).
  Circle leaves a page, then the title cursor sits on OPTION (VS is Down x3 from there).
- **Round timer** (verified, `timeout_*` replays): `round_time` counts +1 per vblank and the round ends
  at 1800 for a 30 s limit. The fighter with more hp wins the round on time (`round_wins` 1,0 / 0,1);
  **equal hp is a draw and both fighters get a round win** (1,1); with 2 rounds to win a second draw ends
  the match 2,2 (draw match); the next round starts after ~200 frames. hp and gauge reset each round.
- **Story**: with no input Gradd loses the first fight and `CONTINUE ?` (YES / NO) appears ~16700.
  YES -> "BATTLE 1" again and the fight restarts (~17300); NO (Right, Cross) -> GAME OVER, then the
  title, the logo and the intro FMV again (verified: `story_continue_yes`, `story_game_over`).

## Overlay entry points (fixed bug, verified)

The per-character function tables in the main exe's data (from `0x800b310c`, 4 pointers per character
for each of P1/P2) point into the overlay slots, and `8004c1e8` calls them with `jalr`. Rohzen's P2
overlay (`MROSEN2.BIN`) starts with 24 bytes of string data (`"...%d..."`) before its first function, so
its entries are `8001a284 / a2b4 / a2e4 / a314` instead of the other characters' `8001a26c / a29c /
a2cc / a2fc`. REA's procedure list had merged `8001a284` into a bogus function at `8001a268` that
covers the string data, so it was never a recompiled entry: playing P2 Rohzen hit `dispatch: no
recompiled function at 8001a284` (fatal with `--no-interp`, interpreter fallback otherwise).
Fix (`recomp/recomp.py`): data words in the main executable that point into the overlay slots are now
overlay seeds too (as already done for overlay data and `jal` targets). A static check over all P1/P2
overlays, EVENT_SP/EVENTOVL and OPTOVL found no other function-looking referenced address that was
not recompiled. Scenario `vs_villa` (Tieme vs Rohzen P2) covers it. File offset = address - slot base
(the 4-byte header sits at the slot base itself, code is not shifted).

## Test coverage facts

- `tools/scenarios.py` (24 scenarios, see its docstring and `tools/scenario_expect.json`): every VS
  stage (12), 1P BATTLE x3 characters, TEAM BATTLE, TIME ATTACK, ENDURANCE, PRACTICE, STORY up to its
  first fight, CONTINUE? YES / NO (GAME OVER), OPTION and the changed GAME OPTION page. Every character appears on both sides across the VS scenarios. `tools/fight_script.py` is the older single path used by the smoke test
  and the determinism script.
- Per-scenario check: exit status, no FATAL / dispatch miss / unhandled BIOS call / `--verify-overrides`
  mismatch, expected overlay files loaded in order, recorded fingerprint (overlays + stage packs) equal,
  VS scenarios read exactly their stage's pack, the fight BGM started before the key frame, the key
  frame is not black/flat. The fingerprints were recorded after looking at the key frames
  (`scenarios.py record`); if the game flow legitimately changes, re-record and re-look.

- Vulkan renderer (Windows/Linux port; verified on MoltenVK with `tools/test_vulkan.sh`): the
  7000-frame fight path gives the same Vulkan-vs-software results as Metal (0 differing pixels with
  `--pgxp off` at 2x2 and 4x6, 4:3 and 16:9). So every fight-frame primitive kind the game sends
  (list above) is covered by the GLSL port. The port learned nothing new about the game's own
  code; no addresses were added to `symbols.txt`.
- Direct3D 12 renderer (DXIL from the same GLSL; verified under CrossOver 26, vkd3d over MoltenVK):
  the full game at 4x6 16:9 `--pgxp off` gives 0 differing pixels against software over 500
  compared frames (131 fight frames), and `--renderer gpu` 4x6 gives per-frame state hashes and
  dumped frames byte-identical to Metal on macOS. Like the Vulkan port, nothing new about the
  game's code; no `symbols.txt` change. Live switching between graphics APIs mid-fight (headless
  and windowed, 9-14 switches) leaves the per-frame state hash equal to a run without switches:
  `verified` that the renderer choice cannot reach emulated state.
- Windows build under Wine (`tools/test_wine.sh`, MinGW GCC cross build, CrossOver 26):
  `verified` that the boot-to-fight state hash matches macOS for every frame (4000/4000, at 1x and
  at 4x6 widescreen) with `--no-interp`. So the IRQ path (libetc `setjmp` at 0x8009b8bc resumed at
  0x8009b924 on every interrupt, `ReturnFromException` = C `longjmp` through recompiled frames)
  works with MinGW's setjmp/longjmp on Win64. It also shows that GCC-compiled generated code
  behaves the same as clang-compiled code.

## Gameplay coverage (`tools/coverage.py`)

Measured with the coverage build (`coverage.py build`, then `run`: all 24 scenarios + all 30 replays incl. the
heavy story one), one counter per recompiled function (first execution recorded).

- **Replays** (30 files, 1.6 MB): characters: every one of the 12 is P1 in a `practice_*` replay running the
  84-input command battery (~10k frames each) and appears in 3..7 VS / 1P replays (Gradd and Celia the
  most; each of the 12 also on the P2 side: the P2 overlays `Mxxx2.BIN` are separate code); stages: all 12
  (Hills 4, Villa 2, the rest 1 each, plus one scenario each); modes: VS, 1P battle vs CPU (Reyus), PRACTICE
  (12), story (win of the first fight and the scenes after it), options (round time / handicap / life
  recover / round count). Outcomes: KO round wins and a decided best-of-3 (Gradd-Celia), time-out round
  win for P1 and for P2, draw on time (round_wins 1,1; match 2,2), gauge spending in every replay.
- **Scenarios** add: team battle, time attack, endurance, option sub-pages, GAME OPTION changes, story
  CONTINUE? YES and NO -> GAME OVER.
- **Functions executed: 1425 of 2070 (68.8%)** across everything above: main exe 968 / 1323; each
  character overlay (P1 and P2 side) 12..21 of 20..28 (e.g. MGRAD 19/22, MSEREA 15/23, MANZEAL 13/20),
  OPTOVL 34/44, EVENT_SP 9/21, EVENTOVL 12/22, **MSOLD 0/22 and MGENE 0/24 and MGENE2 0/24** (Soldier and
  Gene are not selectable; MSOLD2 is the story opponent, 21/22). `coverage.py report -v` lists the
  never-run functions per image. What they are is unknown: the same 5..9 functions per character are
  missing in most overlays, so they are probably things the battery cannot trigger (specials needing a
  full gauge or a combo, win / lose poses, throws, story-only moves) - guess.
- **Not covered** (honest list): per-character special / magic moves were *not* individually identified
  (only the generic battery runs), memory-card save / load screens in the game, team battle fights,
  CONTINUE? in modes other than story, endings / credits, the rest of the story after the first
  scenes, TIME ATTACK / ENDURANCE beyond their first fight, VS with ROUND COUNT 1, 99 s / NO LIMIT
  time-outs, Gene / Soldier as playable fighters, Gradd as P2 beyond one 1500-frame random fight.
- Test runtime (the machine had load averages 100-370 from other work, so wall times are unreliable):
  default replay set 29 replays, 255k frames, heavy `--all` +26k (story); headless runs are ~200x real
  time when idle (30k frames ~ 13 s), i.e. ~110 s CPU for the default set, ~30 s wall on 10 idle cores
  (estimate). Measured: `replays.py run` (default set + the 3 variants + selftest, 33 jobs, -j 10) 93 s wall at load
  average 62-80 on 10 cores; under load 100-370 the whole `coverage.py run` (everything, one at a time) took ~40 min.

## Save states and the snapshot-safe point (runtime/savestate.c)

- **Where emulated state lives** (verified: `--savestate-rewind` over every scenario, every re-run
  frame hashes like its first run): `g_cpu` (rt.c, incl. cycles / next_event / COP0); bus.c (RAM,
  scratchpad, scheduler slots incl. their callbacks, I_STAT/I_MASK, timers, DMA registers and
  completion times, misc I/O words, `g_frame_start`); bios.c (events, SysEnqIntRP chains, the
  HookEntryInt buffer address, pad buffers, card completion queue, directory cursor,
  `g_pad_buttons`); gpu.c (1x VRAM, GPUSTAT bits, draw/texture/display state, the GP0 FIFO and
  VRAM transfers, polyline state); gte.c; spu.c (RAM, regs, voices incl. ADPCM history / ADSR /
  sweeps, reverb, noise, capture position, the unplayed part of the CD-audio ring); cdrom.c
  (registers, position, 8 sector buffers, interrupt queue, XA decoder); mdec.c (tables, received
  parameter words, unread output words); sio.c; memcard.c (images, changed flags, open files); the
  frontend frame counter (scripted input is keyed by it). Presentation-only state (hi-res target,
  PGXP associations, widescreen captures, GPU tracking for the UV clamp) is not saved; a load
  re-seeds the hi-res target from the 1x VRAM and drops PGXP associations.
- Kernel tables and the guest stack are in RAM / scratchpad (the fight stack is in the scratchpad).
- **Snapshot-safe point** (verified: tests): recompiled guest code is a C call chain, so the host
  stack holds the guest's return addresses. Generated code keeps no guest value in a C local across
  a call or an `EVT_CHECK` (conditions and jump targets are consumed first), so at any `EVT_CHECK`
  the globals are the whole machine *given the host call chain*. A snapshot is taken in the VBlank
  scheduler event (`bus.c vblank_cb` -> `ss_vblank_begin`, before `frontend_vblank`) and can only be
  restored at a vblank whose host context is identical: same host return-address chain
  (`backtrace()` / `CaptureStackBackTrace`), same host frame address, same shadow call stack.
  Excluded: vblanks inside an interrupt handler (`take_interrupt` keeps the interrupted `Cpu` in a
  local; 16-18 per boot-to-fight run) and inside C helpers that keep guest state in locals
  (`g_ss_unsafe`: bios.c event delivery and SysEnqIntRP calls, override verification, the
  interpreter). The widescreen shadow pass runs with `next_event = ~0`, so no vblank happens in it.
- **How often it recurs** (verified: `--ss-stats 3800`, frames 3800-8000 of the smoke fight, 4200
  vblanks): fights alternate. The vblank after a 30 Hz update's work is spent in `vsync_wait`
  (8009b670) with one host context (2100 = every second vblank); the other vblank lands in the middle
  of the update (1740 in `char_mesh_quads_to_ot` 800363c8, the rest spread over ~20 points in the
  renderers). So a fight has exactly one restorable point per game update. Menus and the title wait
  in VSync too (few contexts, recurring); FMV playback waits at 80049d64 (STR loop). Character select
  around frame 2450 of the smoke script did not recur within 120 frames (one-off contexts).
- Cost (measured, Apple M-series, headless): 3.55 MB per snapshot (3.68 with a card), save avg
  0.08-0.15 ms (max 0.45), load avg 0.18-0.29 ms (max 0.9) incl. PGXP reset.

## Rollback netplay: what the game allows (runtime/netplay.c)

- **Restorable points in fights** (verified: `tools/test_netplay.py`, `NETPLAY_DEBUG=1`): the
  VSync-wait vblank of each game update recurs with one host context, but the mid-update vblank
  lands in one of ~20 contexts, some rare. Restoring at a mid-update vblank therefore often had to
  go 10-20 frames back (max rollback depth 24 vblanks, 35 ms re-simulation); waiting up to 2 vblanks
  for a context with a snapshot within 2 frames of the bad frame cut the max depth to 10 vblanks
  (~12 ms) at the same latency.
- **Loop exits break rollback** (verified: the `match` case without the round gate): after a match
  ends (KO -> win animation -> next match), mispredictions made shortly before the game left its loop
  were found when no snapshot with the new host context existed: 9 / 11 impossible rollbacks per
  peer, then 500+ final-frame desyncs. Gating prediction on `round_time` advancing (it freezes from
  the KO, verified/trace in "Fight state") removed all of them in every case; the fight loop keeps
  running through the KO animation for much longer than the 8-vblank window, so predictions made
  before the KO stay correctable (likely: no impossible rollback seen in 6 cases x 2 peers since).
- A mispredicted input that the game ignored still changed the complete state hash while every
  gameplay field stayed equal (seen in those desyncs: equal gameplay hashes, different state
  hashes). Where the pad state lands in RAM (libpad / the game's own button buffers): guess, not
  traced. It is why the full-state hash is part of the exchanged hashes.
- **Re-simulation cost** (measured, Apple M-series): 1.1-1.5 ms per re-simulated vblank in fights
  (fight emulation ~0.9 ms/vblank + a snapshot ~0.1 ms + restore once per rollback ~0.2 ms); avg
  2.5-8 ms per rollback, max ~12 ms at 20 ms one-way latency with jitter. At 60 Hz that fits in a
  frame with the default window of 8 vblanks.

- Netplay between the macOS build and the Windows build under Wine (`tools/test_netplay_wine.py`,
  CrossOver 26): `verified` that every confirmed frame's gameplay hash (and the full state hash every
  30 frames) is identical on a clang/arm64 peer and a MinGW-GCC/x86-64 peer and equal to an offline
  run, through a VS fight with rollbacks over a lossy simulated link, in both host directions. So
  rollback (snapshot restore at a matching host call chain) works the same on both hosts. Nothing new
  about the game's own code; no addresses added to `symbols.txt`.

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
   widescreen work here. Call sites are now listed (see "GTE projection call sites"); start
   with `80036b0c` (most fight vertices) and the `8003fcb8..80041c14` family.
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
- Which RAM address holds the mode / stage / character ids? Not searched (the scenarios fingerprint the
  CD reads instead). `--watch ADDR` (8 slots, 32-bit) is the probe to use.
- Where exactly are the 1P-battle opponent and stage chosen from the RNG (`800e7560`), and what is the
  order for a normal run (stage 1 = Villa)?
- Are Gene (`MGENE*.BIN`) and Soldier reachable outside STORY (cheat, unlock)? No portrait for them.
- Story: the first fight is lost without input; the scenario stops at its start, so later events are
  untested.
