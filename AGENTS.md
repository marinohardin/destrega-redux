# Destrega static recompilation — agent guide

PS1 *Destrega* (Koei 1998, NTSC-U `SLUS_007.32`) statically recompiled from
MIPS R3000 to C, linked against a native PS1 hardware runtime (C11 + SDL3).
Analysis is done with REA (Ghidra provider). The game boots, plays its FMV,
reaches the title screen, and is playable (1P battle, practice, story, VS).

## Read these first

1. `recomp/recomp.py` — driver: inputs, seeds, overlays, outputs.
2. `recomp/analyze.py` — flow tracing, jump tables, delay-slot handling.
3. `recomp/codegen.py` — exactly what C each MIPS instruction becomes.
4. `runtime/psx.h`, `runtime/recomp.h` — the contract between generated code and runtime.
5. `runtime/rt.c` (dispatch, interrupts), `runtime/bios.c` (kernel HLE), `runtime/bus.c` (memory map, DMA, timers, scheduler).
6. `analysis/NOTES.md` and `analysis/symbols.txt` — what we know about the *game's* code and data.

## Record what you learn about the game (required)

Every task that touches game code teaches something about it. Before you open a PR, add what
you learned to `analysis/symbols.txt` (one line per address: name, confidence, evidence, note)
and, for anything bigger than a line (data layouts, how a subsystem works, a cleanup candidate
and why it matters), to `analysis/NOTES.md`. Commit it in the same PR as the work.

- Tag every fact `verified`, `likely` or `guess` as defined in NOTES.md ("Confidence"), and
  pick the lowest level that applies. Verified means you saw it at runtime (probe, trace,
  test, `--verify-overrides`) or read the whole function and it is unambiguous; a clear
  decompilation you did not run is `likely`; anything inferred indirectly is a `guess`.
- Never raise a level without new evidence. Fix or delete anything you find to be wrong.
- If a function would be worth turning into readable C later, add it to NOTES.md's
  "Cleanup priority" list with the reason, instead of doing it inside an unrelated task.
- Unknown is fine: an address with name `-` and what you observed still saves the next person time.

## Pipeline (real commands)

```sh
python3 tools/extract_disc.py Destrega/Destrega.bin disc   # ISO9660 -> disc/ (needed by recomp)
python3 tools/psxexe2elf.py disc/SLUS_007.32 build/elf/SLUS_007.32.elf   # for REA/Ghidra
./tools/rea_export_overlays.sh                             # REA procedure lists for overlays (slow, cached)
python3 recomp/recomp.py                                   # -> gen/*.c (≈1 s); CMake also runs this
cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake -j10
./build/cmake/destrega                                     # play from the data pack (first launch imports)
./build/cmake/destrega --import Destrega/Destrega.cue      # CLI import -> data pack
tools/run_tests.sh                                         # verification (see below)
```

On macOS the default build also runs `tools/make_app.sh` (target
`destrega_app`) to produce `build/cmake/Destrega.app`: the binary plus every
non-system dylib (Homebrew SDL3) relinked via `@rpath`, the icon from
`assets/icon/destrega.png`, ad-hoc signed. Options: `DESTREGA_MACOS_APP`,
`DESTREGA_APP_SIGN`, `DESTREGA_APP_VERSION`, `DESTREGA_APP_ICON`. The icon is
original procedural art from `tools/make_icon.py` (deterministic; needs
Pillow, NumPy and macOS's Herculanum font). Never put game-derived art in the
repo. Launching the bundle's binary from a non-GUI shell can stall in macOS's
first-launch check; test it from Finder/`open`, or use the plain binary.

The main-exe REA export is `analysis/rea_list_procedures_main.json` (MCP
`list_procedures`); a reusable REA snapshot is `analysis/SLUS_007.32.rea-snapshot`
(`open_binary` with `snapshot_path`). REA's Ghidra MIPS lane only admits
ELF `e_flags = EF_MIPS_ARCH_32` with **no ABI bits** — `psxexe2elf.py` writes
`0x50000000`. The REA CLI needs `GHIDRA_INSTALL_DIR`/`JAVA_HOME` (see the script).

## Game data (bring your own disc, Ship of Harkinian-style)

`runtime/disc.c` is the only code that touches game data. Sources: a raw
MODE2/2352 `.bin`, a `.cue`, or the data pack `destrega.pak` written by
`disc_import()` (zlib blocks of 16 raw sectors + offset table; every sector of
the disc, byte-exact). The import verifies `SLUS_007.32` (641024 bytes,
CRC-32 57818cb0 = US SLUS-00732). Default pack location is SDL's pref path
(`~/Library/Application Support/Destrega/`); override with `--data-dir`.
A positional path argument runs directly from that image/pack (dev use).
`tools/extract_disc.py` reads both .bin and .pak.

## Boot path

`runtime/main.c` opens the pack (or the given image), `disc_verify()`,
`disc_load_file("SLUS_007.32")` via ISO9660,
copies text to `0x800244d0`, sets `sp=0x801fff00` (SYSTEM.CNF), `SR=0x40000401`,
`bios_init()` (kernel tables at 0x200/0x674/0x874 → HLE trampolines at 0x3000),
then `rt_call(0x800a44e8)` (Psy-Q crt0 → `main` at `0x8002b80c`). Everything
else is driven from the game's own loop; the frontend runs inside the VBlank
scheduler event (`frontend_vblank`: present, input, audio, 60 Hz pacing).

## Code model

- One C function per MIPS function: `f_<addr>` (main), `f_<addr>_<OVL>` (overlays).
  Registers live in `g_cpu`; delay slots are executed before the branch takes
  effect; branch conditions / jump targets are captured first.
- Direct calls within an image (and overlay→main) are C calls; everything else
  goes through `rt_call` (binary search of `g_recomp_table`). Overlay entries are
  verified against RAM with an FNV-1a hash of the function's original bytes.
- Unknown targets fall back to `interp_run` (MIPS interpreter) which re-enters
  native code at the first recompiled entry. A `dispatch: no recompiled function`
  log line means a missing seed: add it to `analysis/extra_seeds.txt`
  (`<hex> [OVLNAME]`) or improve seeding in `recomp.py`.
  In normal play the interpreter is never entered: the one mid-function address it
  used to bridge (0x8009b924, the libetc `setjmp` return point resumed on every
  IRQ) is a seeded native entry. `--no-interp` makes any interpreter entry fatal;
  `tools/run_tests.sh` runs the smoke test with it. The fallback stays in release
  builds as a safety net for paths not yet exercised.
- **Readable overrides:** a function listed in `analysis/overrides.txt` is still
  generated, but as `f_<addr>_orig`; `f_<addr>` is hand-written C in
  `runtime/overrides/` (structs/offsets named, fixed-point math explained).
  `override_call()` (override.c) runs the new version directly, or with
  `--verify-overrides` runs both from the same snapshot and compares the result,
  callee-saved registers and all of RAM/scratchpad (except the dead stack).
  `tools/soak.sh SEED` does a random-input soak with checks on. Guest code the
  override still calls goes through `guest_call()`; watch for undeclared
  register arguments (e.g. `f_80044918` reads a3 = normal.y).
  Done so far: `f_800465a4` -> `sphere_vs_poly` (collision.c).
  The game's stack moves into the scratchpad (0x1f800000) during fights.
- Cycle accounting: `CYC(n)` (2 cycles/insn), `EVT_CHECK()` at function entry and
  backward branches runs `rt_event()` → scheduler + IRQ delivery.

## Overlays (from REA decompilation of FUN_8004c1e8 / FUN_8003e36c / FUN_8006b35c)

Files are `memcpy`'d verbatim (4-byte header, code at +4) into
P1 slot `0x80010000` (`Mxxx.BIN`, `EVENT_SP.BIN`, `OPTOVL.BIN`) or
P2 slot `0x8001a268` (`Mxxx2.BIN`, `EVENTOVL.BIN`). See `recomp/overlays.py`.

## Interrupts / BIOS (non-obvious)

- libetc's `ResetCallback` (0x8009b8bc) `setjmp`s into a buffer at 0x800be224 and
  passes it to `HookEntryInt`. On an IRQ, `rt.c:take_interrupt` saves the CPU,
  runs SysEnqIntRP chains (libpad's VBlank handler), loads the hook buffer and
  resumes at its `ra` (0x8009b924, a mid-function address seeded as its own
  recompiled entry via `extra_seeds.txt`); the dispatcher at 0x8009b994 ends in
  `ReturnFromException` = C `longjmp` back.
- Pads: libpad drives **SIO0 directly** (`runtime/sio.c`), not BIOS InitPAD.
- CD: libcd at register level (`runtime/cdrom.c`); BIOS leaves CD int-enable = 0x1f.
  XA audio + STR video (5 movies, file channel 1) live in `XADATA.BNS`.
- Memory cards: `runtime/memcard.c` (+ BIOS HLE in `bios.c`), see "Memory cards" below.

## Verification

`tools/run_tests.sh`: GTE/GPU/SPU/MDEC unit tests + a headless smoke test that
boots to a fight via scripted input and fails on FATAL / dispatch misses /
unhandled BIOS calls / black screen. Useful headless flags:
`--headless --frames N --dump DIR --dump-every N --press F:MASK,... --wav out.wav --trace`.
Button mask bits (active high in `--press`): Select 0x1, Start 0x8, Up 0x10,
Right 0x20, Down 0x40, Left 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800,
Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000.
`--press2 F:MASK,...` is the same for pad 2 (VS mode waits for it). Presses are taps (held 6 frames).
`python3 tools/contact_sheet.py out.png frames...` tiles screenshots for review.

### Scenario library (`tools/scenarios.py`)

Named, deterministic input scripts (a small DSL: `@1200 start down*2 cross/100`, see the file's
docstring) that reach every VS stage (12), 1P battle with several characters, team battle, time
attack, endurance, practice, story up to its first fight, and the option menu, with the frame range
of interest (`key`) and automatic checks that each one arrived (the `--trace` CD log shows which
overlay and which stage pack of `DATA.BNS` were loaded; plus a non-black key frame).
`scenarios.py list | run NAME... | run --all | hires | record | raw "DSL" FRAMES`. Needs `disc/_listing.txt`
(`tools/extract_disc.py`). To add one, append an `add(...)`/`add_vs(...)` call, run it, look at the key
frame (`--keep`, `tools/contact_sheet.py`), then `scenarios.py record NAME` to store the fingerprint in
`tools/scenario_expect.json`. A scenario that exposes a real bug is `xfail=` (the run must still fail
that way) and the bug goes in `analysis/NOTES.md`. `run_tests.sh` runs all scenarios (`--no-interp`,
`--verify-overrides`) and the hi-res determinism / `--hires-verify` check on the `hires`-tagged
subset (every stage) concurrently with the rest; `SCENARIOS=0 tools/run_tests.sh` skips them.
Headless runs ~200x real time (30k frames ≈ 13 s).

## Memory cards (`runtime/memcard.[ch]`, `bios.c`)

Standard raw 128 KiB images (`.mcd`, same as common emulators; DexDrive/`.gme`-style headered files
are refused, never overwritten). Two slots. The game uses the BIOS card API, not SIO0 (details in
`analysis/NOTES.md`), so cards are emulated at BIOS level: `_card_info/_card_load/_card_write/_card_read`
and the `bu00:`/`bu10:` file calls run on the in-memory image immediately; the SwCARD/HwCARD
completion events are queued and delivered from `bios_irq_prologue` (interrupt context, so on
a vblank or other IRQ) ~60k cycles per sector later. SIO0 still answers 0x81 as "no card".

- Options: `--memcard1 PATH`, `--memcard2 PATH` (created formatted-empty if missing), `--no-memcard`.
  Windowed default: `memcard1.mcd` next to `destrega.pak`, slot 2 empty. **Headless has no card
  unless given one** so tests never see a user's saves.
- The image is written atomically (temp file + rename) after each completed card operation and at exit.
- `--state-hash` includes the card images and the changed flag; `--ram-dump FILE` writes RAM at exit.
- Tests: `runtime/tests/test_memcard.c` (format, chained files, directory, erase, persistence,
  refusing bad images) and `tools/test_memcard.sh` (scripted save of a changed option from a fresh card, load in
  a new run, identical hashes and card files across identical runs).
- **Rollback netplay (future, not implemented): card writes must be suppressed during netplay**
  (a rolled-back frame must not leave files behind, and the two peers' cards differ). The hook is
  `memcard_set_write_protect(true)` (in-memory state keeps updating, flushing becomes a no-op);
  card state must also be part of any snapshot (`memcard_hash` shows what it covers).

## Application menus (`runtime/menu.h`, `menu_mac.m`, `menu_stub.c`, `main.c`)

macOS gets a native **File** menu inserted after the app menu (SDL3 keeps building the standard
app/Window menus; Quit still works). `menu_mac.m` (Cocoa, ARC, Apple only; `menu_stub.c` no-ops
elsewhere) only builds items; each item calls `frontend_menu_action(NAME, ARG)` which **queues**. `main.c`
runs the queue at the top of each VBlank (`menu_script_and_poll`), i.e. between frames on the main
thread, so menus cannot disturb emulation. Headless runs never create menus.

- **Load Game Image… (Cmd+O)**: native open dialog (SDL), `.cue/.bin/.iso`, then `import_image()` ->
  `disc_import()` (the same code as the first-launch dialog and `--import`) into the data pack, then
  `restart_process()` = clean re-exec (`DESTREGA_RESTARTED=1`, same argv minus `--menu-action` and any
  positional image). Errors show a native alert and the game keeps running. `disc_import` parks the
  running disc's module state while importing, so it is safe mid-game. Checks live in `disc.c`: wrong type,
  raw sync pattern (a 2048-byte-sector `.iso` cannot hold the XA/FMV sectors and is refused with that
  explanation; a raw 2352 `.iso` is fine), whole sectors, SLUS_007.32 CRC, and the ISO9660 volume size vs the
  file size (catches truncated copies).
- **Show Game Files in Finder (Cmd+Shift+R)**: opens the folder of the pack in use (or the loose image).
- **Save Games** submenu (the UI deliberately never says "memory card" except for the file type; one slot, card
  slot 1; slot 2, `--memcard2` and `--no-memcard` stay in the runtime for development/tests): Show Save Games in
  Finder; Import Save Games… (open dialog "PlayStation memory card image (.mcd)"; validates 128 KiB + "MC" header
  + checksum, confirms, keeps `memcard1-backup-YYYYmmdd-HHMMSS.mcd`, copies in place, re-attaches); Export Save
  Games… (writes a standard .mcd after flushing); Delete Save Game… (the `BASLUS-00732*` file, confirmed).
  Every swap flushes first and goes through `memcard_attach`, which closes open handles and raises the
  card-changed flag like a real swap (`memcard_mark_new` after a delete).
- **View > Widescreen (16:9)** (checkable; added to SDL's View menu if it has one, else a new
  menu after File; F9 too): action `widescreen[=on|off]`, see "Widescreen".
- **View > Renderer > Software / GPU**: action `renderer[=software|gpu]`, see "GPU renderer".
- **Full screen** on every platform from the SDL event loop in `main.c` (so not a Cocoa-only
  feature): **Alt+Enter** and **F11**, plus **Ctrl+Cmd+F** on macOS, queue the action
  `fullscreen` (`=on|off`, none = toggle; one toggle per frame, because Ctrl+Cmd+F can arrive both
  as the menu key equivalent and as an SDL key event). These keys never reach the pad (the Enter
  of Alt+Enter is not START). macOS: **View > Enter Full Screen / Exit Full Screen** (title
  follows the state, Ctrl+Cmd+F; replaces SDL's own "Toggle Full Screen" item, which called
  `-[NSWindow toggleFullScreen:]` behind SDL's back). `SDL_SetWindowFullscreen` uses the desktop
  mode (a native full-screen Space on macOS); SDL keeps and restores the windowed size and
  position. The hi-res auto scale follows the new pixel size in `present_frame` like any resize
  (widescreen and both renderers included); the log shows `fullscreen: entered/left, window WxH
  px` and the new `render scale`. Headless runs have no window and only log the request.
- Test hook: `--menu-action FRAME:NAME[=ARG]` (repeatable) runs the same handlers on that frame:
  `load=FILE`, `reveal-game`, `reveal-saves`, `save-import=FILE`, `save-export=FILE`, `save-delete`,
  `widescreen[=on|off]`, `renderer[=software|gpu|compare]`, `fullscreen[=on|off]`. Headless: no dialogs (the file argument is
  required), confirmations are auto-accepted and logged, nothing opens Finder, and card actions refuse to run
  unless `--data-dir` or `--memcard1` says where the cards are (never the user's real folder by accident).
  Log lines start with `menu:`; errors with `menu: ERROR:`.
- Tests: `tools/test_menu.sh` (in `run_tests.sh`): load from `.cue` and raw `.iso` into a scratch data dir
  with state hash equal to a plain run on that pack, 9 rejection cases, save import with backup (the game
  really loads the imported save: ROUND COUNT byte), export, delete, invalid files. It rebuilds
  a disc image from the pack with `tools/pak_to_bin.py` when `Destrega/Destrega.cue` is absent.
- Not done: saves other than the game's own file, Windows/Linux menus.

## High-resolution rendering (`runtime/hires.[ch]`)

Presentation-only. `gpu.c` stays the authoritative 1x GPU: every GP0 command still runs
on the 1x VRAM and all CPU-visible state (VRAM reads, GPUSTAT, copies, CLUTs) comes from it.
Each drawing command is *also* forwarded through `g_hires` (a `HiresBackend`: command in ->
target out; swap it for a GPU backend later) to a separate target covering VRAM
(0,0)-(512,480) at independent integer scales Sx (horizontal) x Sy (vertical)
(`hires_set_region` changes the region). Nothing in the target is ever read by the emulated
machine, so resolution cannot affect gameplay (`tools/test_hires_determinism.sh` proves it by
hashing all emulated state per frame).

- Rules match the 1x GPU: same top-left fill rule, colour/texture math, mask bit and
  semi-transparency (applied against hi-res destination pixels). At 1x1 the target is
  bit-identical to the 1x VRAM region (`--hires-verify`).
- Coverage = 1x geometry scaled by Sx/Sy (a w*h rect covers exactly Sx*w x Sy*h pixels).
  Colour/u/v come from the 1x plane equations at each hi-res pixel centre. Vertices are
  16.16 fixed point in 1x units: integers, or sub-pixel GTE positions with PGXP (below).
- UV clamp: a hi-res pixel centre can lie outside the polygon (texture minified by more than
  ~1.3 texel per pixel, and always near the right/bottom edges), so interpolated u/v are clamped
  before the texture-window mask, otherwise the lookup reads a neighbouring texture in VRAM
  (thin lines along tile and polygon edges). The clamp range is the u/v range the **1x
  rasterizer actually sampled for that triangle** (`gpu.c` `uv_track_span`, recorded only while
  a hi-res target exists, extremes taken at span ends because u/v are linear in x; the 1x draw
  now runs before the hi-res tri is forwarded). The vertex UV range is not tight enough: the 1x
  GPU never draws a triangle's last column/row, so it never reads the far-end texels, which are
  often an adjacent tile's border. If 1x sampled nothing the vertex range is the fallback.
  Textured rects sample one texel per 1x pixel with the same u/v as 1x, so they need nothing.
  Skipped at 1x1, where sample positions equal the 1x GPU's (it wraps rather than clamps in
  rare pixels) and the result must stay bit-identical. `hires-uv:` in the run summary counts
  textured samples, those clamped, those still outside the 1x-sampled range when fetched (must
  be 0; checked by the determinism script at every scale) and how many triangles used the
  1x-sampled range.
- Threading (hi-res path only): `hires_set_threads` / `--hires-threads N` (default: performance
  cores - 1) starts a worker pool. A primitive whose hi-res bbox is >= 4000 px
  (`--hires-par-min`) is split into row chunks, rasterized fork-join (the emulation thread
  takes chunks too and waits), so command order within every pixel is preserved and the result
  is identical to serial (unit-tested, state hashes unchanged). Workers spin ~50 us after the
  last job, then sleep. Smaller primitives run inline. Texture reads come from 1x VRAM while
  the emulation thread waits, so nothing races. The 1x GPU and emulated state stay on the
  emulation thread. Windowed present converts rows on the same pool and uploads only when the
  displayed rows were drawn since the last upload (`hires_dirty_rows`); uploading the 15-bit
  target as XBGR1555 was slower (SDL converts on the CPU), so it is converted to RGBA8888.
- Textures/CLUTs are sampled from the 1x VRAM (they are 1x assets, so texels are Sx x Sy
  blocks). A textured primitive whose page overlaps the framebuffer region samples 1x VRAM
  and is logged once per page. Dither is off above 1x1 (`--hires-dither` keeps it). Lines: the
  thickness follows the scale of the axis perpendicular to the line's major axis (x-major
  lines are Sy thick, y-major lines Sx thick). Uploads (FMV slices) are nearest-upscaled into
  the target; fills and VRAM copies are scaled.
- Falls back to presenting 1x VRAM when the display is 24-bit (FMV), disabled, wider than
  the region (e.g. 640 px modes) or outside it.
- Auto scale (default): the 4:3 letterbox viewport (vw x vh) inside the window's physical
  pixels (`SDL_GetWindowSizeInPixels`; the window is HiDPI) is covered without ever
  upsampling at present: `Sx = ceil(vw / display_w)`, `Sy = ceil(vh / display_h)` using the
  actual display mode (normally 512x240), capped per axis by `--max-scale N|NxM`
  (default 4x6, see measurements in the PR). A 1440p-ish window renders 2048x1440 (4x6).
  `--scale N|NxM` forces a scale (0 = off). On a resize the scales are recomputed; if they
  changed, the target is reallocated and seeded from the current 1x VRAM.
  Presented with linear filtering into the letterboxed viewport.
- Options: `--scale N|NxM`, `--max-scale N|NxM`, `--hires-dither`, `--window WxH` (initial
  window, points), `--resize F:WxH,...` (headless: simulated window sizes in physical pixels),
  `--state-hash FILE`, `--hires-verify`, `--fight-from F` (default 3700) and
  `--hires-threads N`, `--hires-par-min PX`, `--segments F,F,...` (frame boundaries of the
  per-segment ms/frame report printed at exit, headless and windowed; default 1200,1800,2400,3000
  = intro, title, character select, stage intro, then fight), `--hires-min-fight PCT` (exit 4 unless that % of frames >= F could be shown from the hi-res
  target; the run prints `hires-frames: ...` with hi-res vs 1x-fallback counts). `--press`,
  `--frames` and these work windowed too; a windowed `--frames` run prints the host
  emu/present time over the fight frames. `--dump` writes frames at the render scale;
  `tools/contact_sheet.py --cell WxH --cols N` tiles them; `tools/fight_script.py` prints a
  boot->fight `--press` script.
- Tests: `runtime/tests/test_hires.c` (rasterizer vs hand-checked counts at several
  Sx x Sy, including Sx != Sy; UV-bleed and 1x-sampled-range regressions; 0 vs 4 worker threads identical), `tools/test_hires_determinism.sh` (12 runs, 8 at a time, some with worker threads: off,
  2x2, 4x4, 8x8, 2x3, 4x6, a resize schedule with Sx != Sy, and the 1x1 equivalence run; all
  state hashes equal, >=95% of fight frames hi-res, 0 UV fetches outside the 1x-sampled range, UV clamp a no-op at 1x1). `tools/run_tests.sh` runs the unit
  tests, `tools/smoke_test.sh` and the determinism script concurrently (~1.7 min).

## PGXP: sub-pixel vertices and perspective-correct textures (`runtime/pgxp.[ch]`)

Presentation-only, like hi-res. Removes the vertex wobble and affine texture warping in the
hi-res target; the guest still sees exactly the integer GTE results, RAM and 1x VRAM.

- `gte.c` RTPS/RTPT also compute the projection without the integer steps (`pgxp_project`:
  H / unrounded SZ instead of UNR on the integer SZ3, unsaturated MAC1/2) and store
  {integer SXY word, dx, dy, depth} in a ring pool; the tag (pool index) rides in a 3-entry FIFO
  next to SXY0..2. No value when SX/SY/SZ/IR were clamped or the two differ by >= 4 px.
- Tags follow the word: `codegen.py` emits `pgxp_lw/sw/mov/mfc2/mtc2/lwc2/swc2` hooks
  (register moves = `addu/or rd, rs, $zero`, `addiu rt, rs, 0`); `interp.c` mirrors all but
  moves. Shadow tags: one per CPU register, per RAM/scratchpad word, plus an I/O sink slot that
  carries an SW to the GP0 port. GPU DMA (block and linked list), the GP0 port and BIOS GPU_cw
  pass each word's tag to `gpu_write_gp0_tag`. A tag is used only if the pool entry's integer
  word still equals the GP0 word (`pgxp_lookup`), so stale tags (ALU ops, sh/sb rewrites,
  ring reuse) just fall back to the integer vertex. No invalidation anywhere.
- **Welding (required, see NOTES.md "GTE projection call sites")**: the character meshes rely on
  integer snapping. Vertices of different primitives that are meant to meet (hair/face/limb
  pieces) are distinct GTE results that only coincide after rounding to the same SXY word; drawn
  at their own precise positions they separate by up to 1 px: cracks, torn shoulders, back
  geometry and a dark smear showing through the face. So within one batch (one GPU linked-list
  DMA, or one vblank) every use of an SXY word gets one position: the first precise value seen
  for it, or the integer if it was first sent without one (`pgxp_weld`). Vertices on different
  integer pixels stay independent, so the stage and the character outlines remain wobble-free.
  A precise value whose W differs by more than 1.25x is another mesh on the same pixel (stage
  vertex w=9073 vs character vertex w=3065 on one word) and is welded separately, keeping its W.
- `gpu.c` adds the offsets to polygon vertices for `g_hires` only (`HiresVtx.x/y` 16.16 plus
  `HiresVtx.w`, 0 = none). Lines and rects (2D sprites, HUD, text) are untouched.
- `hires.c`: a textured triangle with `w` on all three vertices is textured perspective-
  correctly (q = 1/w, u*q, v*q planes; exact every 8 hi-res pixels, linear in between);
  otherwise affine as before. Equal `w` on all three (walls facing the camera) takes the affine
  path, which is the same mapping: with q constant, exact texel-boundary ties are common and the
  double (software) and 2^-38 fixed-point (GPU) planes rounded them differently (whole rows one
  texel apart; 52K pixels in a 16:9 4x6 compare run before, 28 after). The UV clamp to the 1x-sampled range stays: perspective u/v inside
  the triangle are a convex combination of the vertex u/v, so the clamp only acts at the edges /
  outside, as for affine; it is also applied at 1x1 when a triangle has precise vertices
  (samples are off the 1x grid there too). The gouraud colour clamp is unchanged (colours stay
  affine).
- Option `--pgxp on|off|affine` (default on; `affine` = precise positions, PS1 texture mapping).
  Off = previous output exactly (tracking disabled, `w` = 0). The hooks are guarded by
  `g_pgxp`. At 1x1 with PGXP on, `--hires-verify` reports mismatches by design (vertices leave
  the integer grid); equivalence is tested with `--pgxp off`.
- Run summary: `pgxp-check:` (vertices actually drawn: on-screen vertices > 1.5 px from their
  integer, depth ratio > 2 inside a triangle between vertices from different GTE call sites,
  values older than 2 vblanks, and "split" = an SXY word at a similar depth drawn at two
  positions in one batch, i.e. a crack; all must be 0, the determinism script and the s23
  scenario check it), `pgxp-weld:` (uses snapped), `--pgxp-diag N` logs up to N offenders with the GTE
  call site, `--dump-from F` limits `--dump` to frames >= F. `pgxp: H of N polygon vertices
  precise, ... misses: S stale tag, G equal to a recent GTE result` (G estimates GTE vertices whose association was lost; the rest of the
  misses are CPU-computed 2D polygons). Fight at 4x6: see the PR / NOTES.md for numbers.
- Tests: `runtime/tests/test_pgxp.c` (GTE registers bit-identical on/off; GTE -> register ->
  RAM -> lw -> move -> sw -> GP0 path; dropped after sh rewrite, stale register, untracked
  store; FIFO shift; perspective only with three depths; welding within and not across batches), and the determinism matrix runs
  `--pgxp on/affine` at several scales against a `--pgxp off`, hi-res-off reference.
- For other backends (`HiresBackend`): `w` is optional; ignore it for affine texturing.
- GPU renderer (`hires_gpu.c`, see its header): fractional positions are drawn with the exact
  edge test, using the triangle grown by 1 px, or the bounding box for slivers. Perspective
  u/v use 2^-38 fixed-point planes with an exact division. Results at 4x6 against software:
  - `--pgxp off` and `affine`: 0 differing pixels;
  - `on`: 29 pixels over 331 fight frames, texel choice where double and fixed point round
    differently (test limit `CMP_MAX_PGXP`, 0.001 %).

  GPU 4x6 fight on a loaded machine: 10.3 ms/frame with `--pgxp off`, 11.7 with `affine`,
  15.2 with `on`. Tracking alone costs nothing measurable.
## GPU renderer (`runtime/hires_gpu.[ch]`, `--renderer gpu`)

A second `HiresBackend` on SDL3's SDL_GPU (Metal, MSL shaders embedded in the C file). Same
contract as the software backend: commands in, target out, presentation only. It is
**bit-identical to the software backend** at every scale tested (1x1, 2x3, 4x6, 8x8 over a
7000-frame fight run) and to the 1x VRAM at 1x1 (with `--pgxp off`; for `on` see the PGXP
section). Keep it that way: a rule change in `hires.c`
needs the same change in the shader. The header comment of `hires_gpu.c` has the details.

- `--renderer software|gpu|compare`. Default: **gpu in a macOS window** (if the SDL GPU renderer
  or the backend cannot be created: `hires-gpu: GPU renderer unavailable ...` and software),
  software headless (the tests' reference). `--renderer software` forces software and the
  plain SDL renderer (no live switch to GPU then). **View > Renderer > Software / GPU**
  (radio check marks; menu action `renderer[=software|gpu|compare]`, no argument toggles)
  switches at any time between frames: `hires_set_backend` + `hires_set_scale` at the current
  scale, i.e. the old target is destroyed and the new one seeded from the 1x VRAM, as on a
  resize. The window always uses SDL's GPU renderer, which presents either target. `compare` draws every command on both
  backends, presents the software one and every `--compare-every N` frames (default 30) reads
  both back: `hires-compare:` lines at exit (all / fight frames, pixels differing, by >1 step,
  mask-bit only, worst frame); `--compare-max PCT` exits 6 if a fight frame differs more.
  Headless works (own device, needs `SDL_InitSubSystem(VIDEO)` with the default driver; the
  `offscreen` driver has no GPU backend). Without Metal it logs `GPU renderer unavailable` and
  uses software. Windowed it presents straight from the GPU texture (SDL GPU renderer +
  `SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`); readback only for `--dump`, `--hires-verify`
  and compare (`HiresBackend.read`).
- Exactness: target B5G5R5A1 (shader writes PS1 red into `.b` = the low 5 bits, so the download
  is raw PS1 u16, alpha = mask bit). Attributes are the 1x plane equations evaluated as exact
  rationals in 64-bit integers per pixel (Metal supports `long`). Semi modes are fixed-function
  blends (B/2+F/2 via constant 0.5 and F/2-1/4; see the file comment for the one F=0 tie case).
  Triangles with a vertex outside the target are drawn as their clipped bbox with an exact
  edge test in the shader: GPU clipping produces inexact vertices that flip shared-edge
  fill-rule ties (single-pixel diffs at 1x1).
- The software backend's span-start evaluation was a `double` expression; for Sy=6 (1/6 is
  inexact) it rounded exact-integer u/v down and picked neighbouring texels. It is now the same
  exact rational floor as the shader (`EXACT_AT` in `hires.c`), which made the two agree.
- Batching: per-primitive storage-buffer records, one draw per blend pipeline key per group;
  a group closes when a primitive would be reordered against an earlier overlapping one of a
  later key (32x32 1x-tile masks). Render passes break only for VRAM texture uploads, puts
  and VRAM copies. One command buffer per view/readback, 2 frames in flight.
  `hires-gpu:` at exit prints submits, passes/draws/prims per submit, uploads, copies.
- Speed (headless fight, frames 3700-4500, machine heavily loaded): 4x6 software 34.6 vs GPU
  11.1 ms/frame; 8x8 software 95.1 vs GPU 19.7 ms/frame (no hi-res: ~6 ms). Windowed 4x6 fight:
  emu 1.3 ms + present 6.6 ms per frame.
- Not implemented / approximate: mode 0 + mask check, mode 0 with F=0 over odd B (off by one;
  mode 0 unused by the game), VRAM->VRAM copies that overlap themselves inside the target are
  chunked by rows, `hires-uv:` sample counts are 0 on the GPU (no shader counters), MSL only
  (no SPIR-V/DXIL), no `--hires-threads` (n/a).
- Widescreen guard band: supported (`set_guard`): `s_ox` is negative with the default region
  at x = 0, so the clip rectangle's x is packed signed, the shader's 1x column uses floor
  division and the 32x32 tile masks are shifted by the guard width. Guard-band-only packets
  (`g_hires_shadow`, set by gpu.c around `gpu_shadow_packet`) do not mark VRAM blocks dirty
  (they never touch VRAM; marking them caused re-uploads and 10x the render passes).
  `--renderer compare` compares the guard band too. `--pgxp off`: 0 differing pixels at 2x2,
  4:3 and 16:9 (6000 frames; `ws2c` in `test_widescreen.sh`). PGXP on, 4x6, 7000 frames, every
  10th: 24 (4:3) and 28 (16:9) differing pixels over 331 fight frames (`ws46c`, limit
  `CMP_MAX_PGXP`). With PGXP the wide pass is bracketed by `pgxp_snapshot_save/restore`
  (registered with `widescreen_add_state_hook` in `main.c`: tag tables copied, pool and
  recent-word writes journalled and undone), so the real pass draws exactly what it would at
  4:3 (`pgxp-check` counts identical). Extra (guard-band) packets carry no tags: a vertex gets a
  precise position only if the real geometry already welded that SXY word in the batch
  (`pgxp_weld_peek`, which inserts nothing), which closes seams at the 4:3 edge.
- Tests: `tools/test_hires_determinism.sh` adds `g4x6` (state hash), `gverify1` (1x1 ==
  1x VRAM), `c1x1` and `c4x6` (compare vs software, `--pgxp off`, `CMP_MAX` default 0 %), and
  `cp4x6` (compare with `--pgxp on`, `CMP_MAX_PGXP` default 0.001 %). `g4x6` runs with PGXP on
  and must pass the `pgxp-check`.
  `tools/test_widescreen.sh` adds `ws46g` (16:9 on the GPU) and `wsrender` (renderer and
  widescreen switched back and forth mid-fight, state hash = 4:3 software run).

## Widescreen (`--widescreen`, default off)

16:9 fights with **zero** change to emulated state (proved per frame by
`tools/test_widescreen.sh`). Menus, FMV, character select and other non-stage screens stay 4:3.

- Guard band (hi-res only): `hires_set_guard(88)` gives the hi-res target 88 virtual 1x columns
  left and right of the 512-wide region (`HiresBackend.set_guard`, optional). Primitives whose
  draw area touches the region edge are clipped against the widened area there; fills that span
  the whole region and untextured rects across the whole draw area (fades) extend into it;
  uploads/copies never do. A triangle crossing the edge uses its vertex UV/colour range for the
  clamp (the 1x-sampled range would smear it outside). 1x VRAM is unchanged: the 512-wide part of
  the target still equals it at 1x1 (`--hires-verify` in the test).
- The game culls stage polygons at screen x +-255 and stage objects at +-257 (see NOTES.md
  "Camera, projection and culling"). Those 50 `slti` immediates are listed in
  `analysis/widescreen_cull.txt`; `recomp.py` emits them as `WS_IMM(orig, wide)` (`recomp.h`),
  which is the original value unless `g_widescreen_cull` is set.
- **Why a shadow pass**: running the widened culling for real changes RAM (packets, OT, per-object
  animation counters) and emulated time (more instructions, longer GPU DMA), and that measurably
  changed the fight (different positions within ~2000 frames). So `stage_render` (80044210) is an
  override (`runtime/overrides/widescreen.c`): snapshot RAM/scratchpad/CPU/GTE, run the renderer
  with `g_widescreen_cull` on, interrupts held off (`next_event = ~0`) and hardware access
  blocked (`g_bus_sandbox` in bus.c; any access rejects the pass), record the 2048 OT slots,
  restore, run it again normally (the run the game sees), record again; per slot the packets the
  wide pass has and the real one lacks (compared by payload) are the extras. The GPU linked-list
  DMA calls `g_gpu_ll_hook` per node; on reaching a slot the extras are drawn into the hi-res
  target only (`gpu_shadow_packet`: no 1x drawing, no IRQ, GPU state restored).
- Presentation: 16:9 (or the window's aspect, clamped to 4:3 .. 4:3 * 688/512) while the stage
  is being drawn (a `WS_IMM` evaluated within the last 6 vblanks), 4:3 otherwise; the HUD keeps
  its 4:3 layout in the centre. The render scale is picked for the widest presentation the window
  allows (`hires_pick_scale_ar`), so switching never reallocates. Default window 1280x720.
  Headless dumps use `--window`/`--resize` as the window.
- Options/diagnostics: `--widescreen`, `--gameplay-trace FILE` (per frame: health, gauge,
  position of both fighters, round time, wins, RNG seed, fighter-struct hash), `--ram-snap DIR`
  / `--ram-snap-every N` (raw RAM dumps for diffing). The run summary prints
  `widescreen: N shadow passes, M extra stage packets, R rejected` and `widescreen-frames: ...`.
- Runtime switch (`runtime/widescreen.h`): View > Widescreen (16:9) (checkable, `menu_mac.m`)
  and F9 queue the menu action `widescreen` (`=on`/`=off`, none = toggle), which calls
  `frontend_set_widescreen(bool)`; the switch takes effect in that vblank, between frames.
  The first switch-on allocates the guard band (a hi-res reconfigure, like a resize);
  switching off keeps it and only stops the shadow pass and the wide presentation.
  `--widescreen` is the startup state. Headless: `--menu-action F:widescreen[=on|off]` or
  `--widescreen-at F:0|1,...` (both used by `wstoggle` in the test script).
- `stage_render` deliberately bypasses `override_call`: `--verify-overrides` cannot rewind the
  scheduler, so interrupts/CD events inside the renderer (story scenes) gave false mismatches.
  Story-mode EVENTOVL scenes use the same renderer and widen too (checked by hand: 15000 frames
  of the `story` scenario, state hash identical to 4:3).
- Host-side state that generated code mutates outside guest state (e.g. PGXP tags) must be
  registered with `widescreen_add_state_hook(save, restore)`, or the wide pass leaks into the
  real one.

## Pitfalls

- Branches may target another branch's delay slot (e.g. 0x800951c0); the tracer
  must not treat delay slots as visited flow positions.
- Jump-table bases are often loaded into a saved register far from the `jr`
  (`Analyzer.const_of` whole-function fallback).
- `.text` contains data on both sides of code (0x800244d0–0x80026618 and
  ≥0x800aa000): never linear-sweep; only trace from seeds.
- Interrupt delivery restores the whole `g_cpu` — keep `cycles`/`next_event`
  and the shadow call stack depth (`g_depth`) from *after* the handler.
- `gen/` is generated; never edit it by hand.
- DMA completion races (fixed in `bus.c`; symptom: KOEI splash / intro FMV
  lagging, stuck in the STR wait loop at 0x80049e90): libcd's CD interrupt
  handler read-modify-writes DICR, which acked an MDEC slice-done flag that
  became set in between, killing libpress's slice->callback chain. Fixes:
  per-channel completion times, completions deferred while an interrupt
  handler runs, queued (not merged) repeat completions, and writes with the
  read-only bit 31 set are treated as RMW (no flag ack). Check with
  `--trace` (prints per-second MDEC frame/CD sector counts; intro = 15 fps).
- Fights run at 30 updates/s by design (`f_80027218(1)` -> `VSync(2)`); not a bug.
