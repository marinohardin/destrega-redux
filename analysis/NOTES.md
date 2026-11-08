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
- CD: libcd at register level; XA audio and the 5 STR movies live in `XADATA.BNS`.

## Test coverage facts

- `tools/fight_script.py` reaches: title -> 1P battle -> the temple stage (Gradd vs Milena), then
  Celia's brick/green/crates stage from about frame 8000. It does not reach the canyon, castle,
  catacomb or Tieme stages, story mode, VS mode or options. (verified: dumps)

## Cleanup priority (readable C / overrides)

Highest value first. Move items to "done" with the PR that did them.

1. **Game-state root and per-fighter state struct**: needed for rollback (what to snapshot,
   what must be deterministic) and for any gameplay mod.
2. **Random number generator** (address unknown): rollback needs to know every RNG source.
3. **Pad input -> fighter commands**: where inputs enter game state (rollback, input display).
4. **Camera, projection and GTE call sites** (RTPS/RTPT, screen offset/H): PGXP and
   widescreen work here.
5. **Visibility culling / off-screen tests**: widescreen must widen these without touching
   gameplay.
6. **Fight frame update** (somewhere under `8002beb0`, see the call stack above).
7. **Overlay loaders** (`8004c1e8`, `8003e36c`, `8006b35c`): which mode loads which file.

Done:
- `sphere_vs_poly` (`800465a4`), PR #3.

## Open questions

- Purpose of the original port's extra seeds `800a8e28` and `800a9528`.
- Does any gameplay code depend on the GTE results beyond the screen coordinates it draws
  with (e.g. on-screen tests that change behaviour)? Matters for widescreen and PGXP.
