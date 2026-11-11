# High-level renderer: replacement plan

Status: **plan only** (phase 1 = research, done; nothing here is implemented). The game facts
this plan relies on, with their confidence levels, are in `analysis/NOTES.md` "Rendering
pipeline (for a high-level renderer)" and `analysis/symbols.txt`. Facts marked *likely* there
come from decompilation that has not been checked at runtime; the first step of each phase
below is to check them.

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
  **background (D), B, scene (C, 2048 slots, back to front), HUD (A)**. The draw is started from
  the **VBlank callback** (80027268) one vblank later, while the next update builds into the
  other buffer (packets and OTs are double-buffered by scratch 1f800000).
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
  semi-transparent, fading through vertex colour; projectiles call per-character overlay code.

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
the root OT of idx is drawn. Save states and rollback: the captured frame is presentation
state, rebuilt on the next update; after a load, show nothing captured until the next capture
(or fall back to packets for that frame), like `hires_reseed`.

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
- Story EVENTOVL scenes: capture `stage_render` there too once checked; their overlay
  character code is unexplored, so it stays on packets until read.

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
- Host state the capture keeps across frames (`HlFrame[2]`) is presentation-only, not in save
  states, and must be registered with `widescreen_add_state_hook` if the wide pass can touch it
  (the widescreen shadow pass runs stage_render twice: capture only the real run, or capture
  the wide run instead of producing extras).
- Tests: the existing determinism scripts with the new renderer on (`--state-hash` equal to
  off), replays, `test_savestate.sh`, netplay `lan`; plus a new compare mode: render the
  captured frame with an **orthographic PS1-exact mode** (integer projection, OT order, no
  depth buffer) and compare it with the hi-res target at 1x1, which proves the capture found
  every packet with the right vertices, UVs and colours before switching to the real 3D mode.

## Phases

1. **Ground truth** (small): GTE control-register probe for FC / DQA / DQB / BK and the light
   matrices per stage; verify the *likely* layouts at runtime (stage root and command words,
   bone part structs, shadow and effect records) with `--ram-dump` and a temporary probe; check
   the OT B producer and the EVENTOVL scenes. Exit: the NOTES facts used below are *verified*.
2. **Stage** (medium): capture from `stage_render` (all builders, the 0x2000 variants, UV
   animation, object flags); camera; stage lights and depth cue. Suppress stage packets in
   hi-res. Exit: PS1-exact compare mode matches the hi-res target; the 3D mode renders the stage
   with a depth buffer; widescreen fills the margins from the full mesh (the shadow pass can
   then be retired for the stage, behind an option first).
3. **Characters** (medium-large): bone matrices from the part structs, bone-space vertices and
   normals, per-part light matrices and the dynamic light, attachments, `char_mesh_chains`
   (800367d8, not understood yet: biggest unknown of this phase). Replaces the char_mesh.c face
   rebuilding (no integer-degenerate faces exist in float). Exit: compare mode, then 3D mode.
4. **Shadows** (small): capture the 45-vertex draped mesh as a decal.
5. **Effects** (large, long tail): the billboard family (14 builders), the five containers, the
   projectile overlay code per character (12 characters x overlays). Do the shared builders
   first; leave unknown ones on packets (they still draw, without depth).
6. **Polish** (open-ended): background widened for widescreen, texture replacement hooks,
   better lighting options, MSAA (the 3D pass need not be bit-exact against software).

Rough effort: phase 1 a few days; phase 2 one to two weeks; phase 3 two to three weeks;
phase 4 days; phase 5 several weeks, mostly reading overlay code; plus the backend itself
(SDL_GPU pipeline with depth, three shader formats as for hires_gpu: MSL, SPIR-V, DXIL), one to
two weeks.

## Risks

- **Timing of capture vs draw**: capture runs in the update, the draw from the VBlank IRQ one
  vblank later, and rollback / save-state loads re-run updates without presenting. Pairing the
  right `HlFrame` with the right DMA must use the buffer index and a frame counter, not "the
  last capture". Getting it wrong shows a frame from the wrong update (one-frame stutter).
- **Packet-to-item mapping**: suppression must match exactly; a packet suppressed without a
  replacement leaves a hole, one not suppressed is drawn twice. The PS1-exact compare mode is
  the safeguard.
- **Effects in overlays**: per-character projectile code lives in the P1/P2 overlay slots; each
  needs its own capture (or stays on packets). Mixing packet-drawn effects (no depth) with a
  depth-buffered scene can look wrong where they overlap geometry.
- **GTE fidelity choices**: depth cue and lighting in float differ slightly from the GTE's
  saturating integer math; acceptable (presentation only), but the look must match.
- **Widescreen and PGXP interplay**: the widescreen shadow pass runs `stage_render` twice per
  update in a sandbox; the capture must ignore the sandboxed run or use it. PGXP becomes
  unnecessary for captured geometry but stays for the rest.
- **Story EVENTOVL scenes and camera modes 1-3** (`camera_update` table): not read; may set
  matrices differently.
- **Backend work**: a new SDL_GPU pipeline with depth in three shader languages, tested on
  Metal, Vulkan and D3D12 like `hires_gpu.c`.

## Unknown

- Where FC / DQA / DQB / BK are set (stage fog colours), and how the stage packs in DATA.BNS
  are laid out on disc (only the RAM layout is known).
- `char_mesh_chains` (800367d8), OT B, 8007ccdc, the second camera matrix (800cc624).
- Whether any effect has its own culling.
