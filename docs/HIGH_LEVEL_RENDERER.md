# High-level renderer: replacement plan

Status: **plan only**. Nothing here is implemented. Phase 1 is done: the decompilation
research, then a runtime check, recorded in the "Phase 1 result" section below. The game facts
this plan relies on, with their confidence levels, are in `analysis/NOTES.md` "Rendering
pipeline (for a high-level renderer)" and `analysis/symbols.txt`. The facts phase 2 needs are
now *verified* at runtime. Facts still *likely* there come from decompilation that has not been
checked at runtime; the first step of each later phase is to check them.

## Goal

Draw the fight scene (stage, characters, shadows, effects) from the game's 3D data, meshes in
model space, bone and camera matrices, lights, materials, with a real 3D API: floating-point
vertices, a depth buffer, perspective-correct texturing, any resolution and aspect ratio, and
later better lighting or texture replacement. The HUD, menus, FMV and 2D screens stay on the
existing path (1x GPU + hi-res backend).

Hard rule, the same as for hi-res, PGXP and widescreen: **presentation only**. The guest must
see exactly the RAM, registers, packets, ordering tables and timing it sees today. State
hashes, replays, save states and netplay must not change. A run with the high-level renderer
on and one with it off must give byte-identical `--state-hash` files.

## How the game draws a fight frame (summary)

- One update every second vblank (30 Hz). `fight_round_loop` (8002beb0) runs the logic, then:
  background prims (OT D), `stage_render` (80044210), `fighters_render` (80037280: shadow
  8007ec5c + `fighter_render` 80037144 per fighter), effects (8004cb04, 80052240, 800511bc,
  80051b58), HUD (8007b874, OT A), `frame_submit` (80026628).
- Five libgs ordering tables per buffer, linked into a root OT and drawn in the order
  **background (D), B (menus only), scene (C, 2048 slots, back to front), HUD (A)**. The draw
  is started from the **VBlank callback** (80027268) at the first vblank after `frame_submit`,
  while the next update builds into the other buffer (packets and OTs are double-buffered by
  scratch 1f800000).
- An update straddles a vblank: camera, background, stage and characters right after the draw;
  then effects, HUD and `frame_submit` after the next (pads-only) vblank. Save states are taken
  at that middle vblank (see "Phase 1 result").
- Camera: libgs `GsSetRefView2` -> world-to-screen matrix GsWSMATRIX 800ed4c8, which contains a
  0.625 Y scale (display pixel aspect); projection H from `SetGeomScreen`, screen offset 0
  (centre 256,120).
- Stage: static world-space vertices, no per-object matrix; per-vertex normals lit by a light
  matrix (0x34/0x3c) or pre-lit vertex colours (0x35/0x3d), both depth-cued toward the GTE far
  colour; UV / CLUT / tpage fixed per polygon in pre-built packet buffers; UV flipbook
  animation by rewriting u bytes. Polygons are dropped whole beyond x +-255, y +-119, or when
  nearer than SZ 0x18f; OT slot from the furthest vertex.
- Characters: rigid skinning, one GsCOORDINATE2-like world matrix per bone part, vertices and
  normals in bone space, per-part light matrices (3 lights, light 0 a timed point light), NCLIP
  backface culling on integer screen positions; attachments drawn the same way.
- Shadows: a 45-vertex, 64-triangle mesh draped on the stage floor per fighter, subtractive
  semi-transparency, grey by height.
- Effects: view-aligned billboards (identity rotation, translation = WS * position), mostly
  semi-transparent, fading through vertex colour. They are FT4 / G4 / G3 / FT3 / GT4 / F3
  primitives from about 12 main-exe builders (80055820..80057100). The per-character overlay
  code calls those same builders and only adds draw-mode (e1) words of its own.
- Fog and lights: set once per stage by `stage_env_setup` (8003fa44) from tables in the
  executable (FC, DQA, BK, stage light matrices; DQB constant). Only Hills uses lit stage
  polygons.

## Architecture

### Capture, not replacement

Every renderer function keeps running as it does today: it builds its packets and OT links, so
the 1x VRAM, the GPU timing and everything the guest can observe are unchanged. Next to it, a
presentation-only hook **captures** what that call drew in 3D terms and suppresses those
packets in the hi-res target only. This is the pattern `widescreen.c` (stage_render) and
`char_mesh.c` (80036138 / 800363c8) already use: listed in `analysis/overrides.txt`, a C
wrapper that reads guest state read-only and then calls the generated `f_<addr>_orig`, with no
`override_call` / `--verify-overrides` round trip (they are not rewrites, so there is nothing to
verify; the state-hash tests prove they leave no trace).

Per frame, per buffer index:

1. **Capture** (guest render functions, during the update): record draw items into a
   `HlFrame` for buffer idx = scratch 1f800000. Each draw item remembers the packet addresses it
   corresponds to (the packet record + idx * stride).
2. **Suppress** (GPU linked-list DMA, during the vblank draw): `g_gpu_ll_hook` already walks the
   scene OT node by node. A packet that belongs to a captured item is not forwarded to the
   hi-res backend (the 1x GPU still draws it). Needs a third hook or a packet-address set in
   gpu.c; the packet address is known at DMA time.
3. **Render** (present): after the hi-res backend has drawn the root OT (background, unmatched
   scene packets, HUD), the 3D backend draws the captured `HlFrame` of the buffer that was just
   drawn, into the same target with a depth buffer, **between** the background and the HUD.
   Since OT A (HUD) is drawn after the scene, the simplest exact order is: hi-res draws OT D and
   B, then the 3D pass, then hi-res draws the rest (OT C leftovers, OT A). The OT DMA walk knows
   when it enters OT C and A (root slots), so the 3D pass can be triggered at the transition
   from slot 2 (C) to slot 1 (A) of the root OT.

Captured data must be double-buffered like the game's packets: the frame drawn at vblank N was
built during the update before, so capture into `HlFrame[idx]` and render `HlFrame[idx]` when
the root OT of idx is drawn.

**Save states and rollback** (corrected by phase 1): snapshots are taken at the vblank in the
**middle** of an update. At that point the stage and characters of the frame to be drawn next
are already built, but effects and HUD are not. After a restore, the next draw shows a frame
whose first half was built before the snapshot. A capture rebuilt after the restore would
miss it, and "the last capture" would be from a later update (seen in a
`--savestate-rewind` run).

So `HlFrame[2]` must be saved into and restored from every snapshot, as a presentation-only
side-car: not hashed, not part of the netplay identity. The alternative is to drop it on
restore and fall back to packets for the frame drawn next. Netplay re-simulation re-runs at
least 3 vblanks, so it re-captures before the frame it presents.

### Backend interface (sketch)

```c
typedef struct { float m[3][3]; float t[3]; } HlXform;    /* GTE 4.12 rotation + translation */

typedef struct {
    float h;                  /* GTE H (projection plane distance), 1x pixels */
    float cx, cy;             /* screen centre (256, 120) + OFX/OFY */
    HlXform world_to_view;    /* GsWSMATRIX 800ed4c8 (already Y-scaled by 0.625) */
    float near_sz;            /* 0x18f: the stage's drop distance, for fidelity options */
} HlCamera;

typedef struct {              /* everything the PS1 packet's mode bits decide */
    uint16_t tpage;           /* page x/y, colour depth, semi mode (0..3) */
    uint16_t clut;            /* CLUT x/y in VRAM */
    uint8_t  semi;            /* 0 = opaque, else 1 + PS1 mode (B/2+F/2, B+F, B-F, B+F/4) */
    uint8_t  raw;             /* raw texture (no modulation) */
    uint8_t  textured, gouraud;
} HlMaterial;

typedef struct {              /* world-space light set, GTE semantics (4.12) */
    float dir[3][3];          /* light matrix rows (directions) */
    float col[3][3];          /* light colour matrix */
    float back[3];            /* BK */
    float fog_col[3];         /* FC */
    float dqa, dqb;           /* depth cue: IR0 = dqb + dqa * h / sz */
} HlLights;

typedef struct {
    uint32_t key;             /* stable id: stage object / fighter part / effect slot */
    const HlXform *model;     /* model -> world (identity for the stage) */
    const int16_t *pos;       /* model-space vertices (SVECTOR) */
    uint32_t nverts;
    const int16_t *nrm;       /* optional normals (lit meshes) */
    const uint32_t *rgb;      /* optional per-vertex or per-face colours (pre-lit / lit) */
    const uint8_t *uv;        /* per face corner */
    const uint16_t *idx;      /* faces (3 or 4 indices) */
    uint32_t nfaces;
    HlMaterial mat;           /* per face in practice: an array, or one per sub-batch */
    uint8_t lighting;         /* none / GTE NCDS-like / NCC-like / depth cue only */
    uint8_t cull;             /* backface cull (characters) */
    uint8_t layer;            /* opaque / decal (shadow) / transparent (effects) */
    uint32_t packets[];       /* guest packet addresses this item replaces (suppression) */
} HlDraw;

typedef struct HlBackend {
    void (*begin)(int buf, const HlCamera *, const HlLights *stage);
    void (*draw)(const HlDraw *);
    void (*end)(int buf);
    void (*render)(int buf, void *target);   /* into the hi-res target, at the root-OT point */
} HlBackend;
```

Textures stay in the 1x VRAM (tpage + CLUT addressing), sampled the way `hires_gpu.c` already
does; the backend uploads the VRAM texture area when it changes (the hi-res GPU backend already
tracks dirty VRAM blocks). For mipmapped / anisotropic sampling, reuse the fast precision's
**texture cache** (`hires_gpu.c` "Texture cache", AGENTS.md "Texture cache"): its key is exactly
the texture half of `HlMaterial` plus the texture window, `(tpage x/y, depth, clut x/y, window
word)`; one entry is the whole 256x256 page decoded as the PS1 samples it under that CLUT and
window (premultiplied RGBA8, texel 0 = transparent, plus an STP-coverage layer), so the material's
u/v bytes address it unchanged and the semi / raw / gouraud bits stay per draw. A material is then
`(cache layer, semi, raw, textured, gouraud)`; `tc_lookup()` already validates the entry against
VRAM writes (block stamps + content hash) and re-decodes it after a page or CLUT change, so the
high-level backend needs no invalidation of its own. Per-face UV rectangles are not tile-aligned
(66 % of the fight's triangles have an odd edge), so mip levels are limited (4) rather than
per-tile; a renderer that knows the stage's texture atlas layout could cut sub-rectangles instead. Lighting is evaluated in the vertex shader with the GTE formulas
(NCDS/NCDT: colour = lerp(RGB * (BK + LCM * clamp(LLM * n)), FC, IR0); NCCS/NCCT without the
depth cue); exact integer GTE results are not needed because nothing in the target is read
back by the guest.

### What stays on the original path

- HUD and 2D (OT A), menus, character select, title, FMV, the KOEI logo, loading screens:
  untouched (1x GPU + hi-res).
- Background quads (OT D): drawn by hi-res as today; widescreen needs them widened (they are
  fixed 4:3 polygons), a small separate fix.
- Anything not captured (unknown packets in OT C, OT B) keeps drawing through hi-res, so a
  missing capture shows as today's picture, not as a hole.
- Story EVENTOVL scenes: they use `stage_render` and the same stage builders (seen in the
  `story` run), so the stage capture works there. Their camera code is in the overlay and not
  traced, but the capture reads GsWSMATRIX + H, not the camera variables. Their character code
  stays on packets until read.

### Why not draw from the packets with a depth buffer

The packets only hold screen-space integers (or PGXP precise positions) and a per-primitive OT
slot; reconstructing 3D from them loses the bone structure, the integer-degenerate faces and
the off-screen / near-plane polygons the game culled. Capturing at the renderer gives the full
meshes, real normals and matrices, and the polygons the game culled (widescreen gets them for
free, without the sandboxed shadow pass).

## Draw order: what changes with a depth buffer

- Opaque stage and characters: depth buffer; intersections become correct (the PS1 sorted
  stage polygons by their furthest vertex and character quads by their nearest).
- Shadows: decal layer after opaque geometry, depth test on, depth write off, polygon offset
  (they lie exactly on the floor). Subtractive blend = `reverse subtract`.
- Effects: transparent layer, sorted back to front by their PS1 OT slot (keeps the game's
  order among themselves), depth test on (so a wall hides a spark behind it) or off per effect
  if it looks wrong (billboards that used to overlap the floor now intersect it); no depth write.
- Semi modes: B/2+F/2 = blend constant 0.5; B+F = add; B-F = reverse subtract; B+F/4 = constant
  0.25 * F + B. Clamped at 1 by UNORM targets like the hardware.
- Near plane: the PS1 drops stage polygons whose vertices are all nearer than SZ 0x18f; a real
  near-clip at the same distance shows parts the PS1 never shows (cut walls). Option: keep the
  PS1's whole-polygon drop rule for fidelity.

## Guest equivalence

- The capture hooks only read guest memory and the GTE registers' inputs (matrices, vertices);
  they never write guest RAM, registers or GTE state, and the original function runs unchanged
  afterwards, as in `char_mesh.c`. If a hook needs GTE results it computes them on the host, not
  through the guest GTE (no GTE flag or register side effects).
- Packet suppression is in the hi-res forwarding only; the 1x GPU draws everything and the GPU
  timing (DMA length, busy time) is unchanged.
- Host state the capture keeps across frames (`HlFrame[2]`) is presentation-only. It is carried
  in save states as a side-car that is not hashed, so the frame drawn after a restore pairs
  correctly (see "Save states and rollback"). It must be registered with
  `widescreen_add_state_hook` if the wide pass can touch it
  (the widescreen shadow pass runs stage_render twice: capture only the real run, or capture
  the wide run instead of producing extras).
- Tests: the existing determinism scripts with the new renderer on (`--state-hash` equal to
  off), replays, `test_savestate.sh`, netplay `lan`; plus a new compare mode: render the
  captured frame with an **orthographic PS1-exact mode** (integer projection, OT order, no
  depth buffer) and compare it with the hi-res target at 1x1, which proves the capture found
  every packet with the right vertices, UVs and colours before switching to the real 3D mode.

## Phase 1 result (runtime check)

A temporary probe in a coverage build (removed again; method in NOTES.md "Rendering pipeline")
logged the following:
- render-function calls;
- GTE control-register writes;
- GTE registers at chosen function entries;
- every GPU linked-list node, with the guest function that wrote it;
- vblanks and save-state events.

The runs were: `fight_script` to frame 4201, every VS stage, `story`, the 12 `practice_<char>`
replays, and a `--savestate-rewind` run.

| Item | Result |
|------|--------|
| Frame structure, OT / buffer pairing | **Confirmed**, two corrections. (1) The draw happens at the first vblank after `frame_ready`; vblank parity does not matter. (2) An update runs in two halves around a pads-only vblank: camera / stage / characters before it, effects / HUD / submit after it. The frame built under idx i is drawn with idx i. **New**: save states are taken at that middle vblank, so a capture must be part of the snapshot (see "Save states and rollback"). Widescreen runs `stage_render_orig` twice per update (sandboxed wide pass, then the real one). Loading screens: there are no draws in the load gap, and one draw goes through a frame-end variant. |
| Camera | **Confirmed**, with a correction. The GTE RT/TR at every stage-builder entry equal GsWSMATRIX 800ed4c8, in both camera modes seen (0 = follow, 1 = round intro). OFX/OFY 0; H per update through `SetGeomScreen`. **Correction**: the modes come from a mode variable (800c0248), not a table. The "second camera matrix" 800cc624 belongs to an identity rig coordinate. Modes 2 and 3 never ran. |
| Fog, depth cue, BK, lights | **Found** (was unknown). `stage_env_setup` 8003fa44 sets them per stage from tables 800b1cac / 800b1bbc / 800b1c7c / 800b1ca0 / 800b1cf4. DQB is constant. NOTES has the values for all 12 stages. Only Hills uses the lit stage builders. |
| Stage | **Confirmed**: the root, object entries and command words decode in a RAM dump. A command's type selects the builder; the builders write GPU codes 0x34 / 0x3c. The stage builders seen per stage are in NOTES. Near plane, OT rule and UV animation were not re-checked at runtime: they stay *likely*, from code read in full. DATA.BNS on disc: **unknown** (not needed, since the capture reads RAM). |
| Characters | **Partly**: the producers (800363c8 / 80036138) and the per-part LLM / LCM / RT writes were seen. The bone-part layout stays *likely* (the face-list layout is verified by `char_mesh.c`). |
| Effects | **Corrected**: 80055b70 is an FT4 quad and 80056704 a G4 quad, not GT3s. 80055b70 does no culling. **Projectiles**: every character overlay calls the same ~12 main-exe builders and only adds e1 draw-mode words. So capturing the shared builders covers all 12 characters; there is no per-character capture code. |
| Packet attribution | **100 %** of the 971 scene-OT packets of a fight frame have a known producer (characters 709, shadows 128, stage 113, effects 21). Across all runs there are no unknown producers. Packet addresses of different producers interleave on the heap, so suppression needs exact addresses per item. |
| OT B, 8007ccdc | OT B is empty in fights; the title menu and character select use it. 8007ccdc writes no packets (fighter counters, game mode 6). |

## Phases (revised after phase 1)

1. **Ground truth**: done (above). Left over: the camera of the EVENTOVL scenes, camera modes 2
   and 3, the bone-part layout, culling in the effect containers. Each is checked at the start
   of the phase that needs it.
2. **Stage** (medium):
   - Capture from `stage_render`, reading the camera (GsWSMATRIX + H) and the fog / lights from
     the GTE control registers at its entry.
   - Builders: 0x34/0x3c lit, 0x35/0x3d prelit, F3 / F4, and the 0x2000 variants 800426c4 /
     800433c4. Also UV animation and the object flags.
   - Capture the real pass only in widescreen.
   - Snapshot side-car for `HlFrame[2]`.
   - Exact packet-address suppression.
   - The PS1-exact compare mode.

   **One to two weeks**, unchanged. The camera and fog unknowns are gone; the side-car is new
   work (a few days, in savestate.c's presentation hooks).
3. **Characters** (medium-large), **two to three weeks**, unchanged: bone matrices, per-part
   lights, `char_mesh_chains`.
4. **Shadows** (small): days.
5. **Effects**: **two to three weeks**, was "several weeks, mostly reading overlay code".
   Capture the ~12 builders of the 80055820..80057100 family (inputs: the object's world
   points, the identity rotation 800ed4a8, colours, UVs / CLUT / tpage). Also follow the
   preceding e1 draw-mode words (8004e0e8, overlay e1 writers) for semi modes. No overlay code
   has to be read.
6. **Polish** (open-ended): background widened for widescreen, texture replacement hooks,
   better lighting options, MSAA (the 3D pass need not be bit-exact against software).

Plus the backend: an SDL_GPU pipeline with depth, in three shader formats as for hires_gpu
(MSL, SPIR-V, DXIL), **one to two weeks**. Total from phase 2 to a full fight scene: about
7-10 weeks; the stage alone (phase 2 + backend) about 3-4 weeks.

## Risks

Resolved by phase 1:
- **Fog / lights unknown**: set per stage from executable tables, and readable from the GTE
  control registers at `stage_render` entry.
- **Camera modes / second matrix**: the GTE camera at the builders equals GsWSMATRIX in every
  mode seen; the second matrix is an identity rig coordinate.
- **Packet-to-item mapping**: every drawn scene packet has a known producer (writer tracking at
  DMA time). Effects have no hidden per-character builders.
- **Effects in overlays**: the overlays only call the shared builders. (Mixing packet-drawn
  leftovers with a depth-buffered scene is no longer expected for effects.)

Remaining:
- **Snapshot pairing** (new, concrete): `HlFrame[2]` must travel with save states, or the first
  frame after every rewind / netplay rollback that restores to the mid-update point draws a
  wrong frame. It needs a presentation-only side-car hook in savestate.c, which is not hashed.
- **Widescreen double pass**: capture the real pass and use the full mesh for the margins, or
  keep the extras path until the stage capture replaces it.
- **Exact packet addresses**: producers' buffers interleave on the heap, so suppression must
  use the exact packet address set of each captured item. The PS1-exact compare mode stays the
  safeguard.
- **GTE fidelity choices**: depth cue and lighting in float differ slightly from the GTE's
  saturating integer math; acceptable (presentation only), but the look must match.
- **Story EVENTOVL scenes, camera modes 2 / 3**: not seen at runtime; the capture reads the GTE
  camera, so they should work, but they need a check.
- **Backend work**: a new SDL_GPU pipeline with depth in three shader languages, tested on
  Metal, Vulkan and D3D12 like `hires_gpu.c`.

## Unknown

- How the stage packs in DATA.BNS are laid out on disc (only the RAM layout is known; not
  needed for a capture).
- `char_mesh_chains` (800367d8); the camera code of the EVENTOVL scenes; camera modes 2 and 3.
- Culling in the effect containers (80055b70 itself has none).

## Recommendation for phase 2 (stage capture): **go**

Reasons:
- Every input of the stage capture is now known and verified at runtime:
  - the camera (GsWSMATRIX + H, the same in every observed mode);
  - fog / depth cue / back colour / lights (per-stage tables, readable from the GTE at
    `stage_render` entry);
  - the in-RAM stage layout and command words;
  - the full set of stage builders per stage.
- Attribution is complete: every stage packet comes from one of the known builders. The
  builder and packet record give its exact address, so suppression can be exact.
- The pairing rule is clear (idx + update counter), and its one trap, the mid-update snapshot
  point, is identified with a concrete fix.

Conditions for phase 2:
- The snapshot side-car (or the packet fallback for the first frame after a restore).
- Real-pass-only capture in widescreen.
- The PS1-exact compare mode before any 3D output.
