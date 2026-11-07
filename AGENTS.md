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
- Memory card: reported as **absent** (no saves yet).

## Verification

`tools/run_tests.sh`: GTE/GPU/SPU/MDEC unit tests + a headless smoke test that
boots to a fight via scripted input and fails on FATAL / dispatch misses /
unhandled BIOS calls / black screen. Useful headless flags:
`--headless --frames N --dump DIR --dump-every N --press F:MASK,... --wav out.wav --trace`.
Button mask bits (active high in `--press`): Select 0x1, Start 0x8, Up 0x10,
Right 0x20, Down 0x40, Left 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800,
Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000.
`python3 tools/contact_sheet.py out.png frames...` tiles screenshots for review.
Headless runs ~200x real time (30k frames ≈ 13 s).

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
  16.16 fixed point in 1x units (integers today) so GTE sub-pixel precision can be fed later.
- UV clamp: a hi-res pixel centre can lie before the polygon's first texel (texture minified
  by more than ~1.3 texel per pixel), so interpolated u/v are clamped to the triangle's vertex
  UV range before the texture-window mask (otherwise the lookup reads a neighbouring texture
  in VRAM: thin lines along stage tile edges). Skipped at 1x1, where sample positions equal the
  1x GPU's and the result must stay bit-identical. `hires-uv:` in the run summary counts
  textured samples, those clamped, and those still out of range when fetched (must be 0;
  checked by the determinism script at every scale).
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
  Sx x Sy, including Sx != Sy; UV-bleed regression; 0 vs 4 worker threads identical), `tools/test_hires_determinism.sh` (8 concurrent runs, some with worker threads: off,
  2x2, 4x4, 8x8, 2x3, 4x6, a resize schedule with Sx != Sy, and the 1x1 equivalence run; all
  state hashes equal, >=95% of fight frames hi-res, 0 out-of-range UV fetches, UV clamp a no-op at 1x1). `tools/run_tests.sh` runs the unit
  tests, `tools/smoke_test.sh` and the determinism script concurrently (~1.7 min).

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
