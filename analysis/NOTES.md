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
- PGXP never changed the per-frame state hash, i.e. no game code observes the precise values;
  this says nothing yet about whether gameplay depends on the *integer* GTE results (open
  question below).

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
  scenarios). `800e7560` (32-bit, also named in the widescreen work) looks like the game's RNG state: `--watch 800e7560`
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
- **OPTION** loads `OPTOVL.BIN`: GAME OPTION, CONTROLLER, SOUND OPTION, RECORDS, MEMORY CARD, EXTRA, EXIT.
  Circle backs out of a page. RECORDS shows the 1P-battle ranking; EXTRA has CHARACTER PROFILES / IN
  GAME CONTROL / EXIT.

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

- `tools/scenarios.py` (22 scenarios, see its docstring and `tools/scenario_expect.json`): every VS
  stage (12), 1P BATTLE x3 characters, TEAM BATTLE, TIME ATTACK, ENDURANCE, PRACTICE, STORY up to its
  first fight, OPTION. Every character appears on both sides across the VS scenarios. `tools/fight_script.py` is the older single path used by the smoke test
  and the determinism script.
- Per-scenario check: exit status, no FATAL / dispatch miss / unhandled BIOS call / `--verify-overrides`
  mismatch, expected overlay files loaded in order, recorded fingerprint (overlays + stage packs) equal,
  VS scenarios read exactly their stage's pack, the fight BGM started before the key frame, the key
  frame is not black/flat. The fingerprints were recorded after looking at the key frames
  (`scenarios.py record`); if the game flow legitimately changes, re-record and re-look.

## Cleanup priority (readable C / overrides)

Highest value first. Move items to "done" with the PR that did them.

1. **Game-state root and per-fighter state struct**: needed for rollback (what to snapshot,
   what must be deterministic) and for any gameplay mod.
2. **Random number generator** (address unknown): rollback needs to know every RNG source.
3. **Pad input -> fighter commands**: where inputs enter game state (rollback, input display).
4. **Camera, projection and GTE call sites** (RTPS/RTPT, screen offset/H): PGXP and
   widescreen work here. Call sites are now listed (see "GTE projection call sites"); start
   with `80036b0c` (most fight vertices) and the `8003fcb8..80041c14` family.
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
- Which RAM address holds the mode / stage / character ids? Not searched (the scenarios fingerprint the
  CD reads instead). `--watch ADDR` (8 slots, 32-bit) is the probe to use.
- Where exactly are the 1P-battle opponent and stage chosen from the RNG (`800e7560`), and what is the
  order for a normal run (stage 1 = Villa)?
- Are Gene (`MGENE*.BIN`) and Soldier reachable outside STORY (cheat, unlock)? No portrait for them.
- Story: the first fight is lost without input; the scenario stops at its start, so later events are
  untested.
