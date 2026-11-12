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
tools/pr_check.sh                                          # check for every PR (see "Verification")
tools/run_tests.sh                                         # full suite, before a release
```

On macOS the default build also runs `tools/make_app.sh` (target
`destrega_app`) to produce `build/cmake/Destrega.app`: the binary plus every
non-system dylib (Homebrew SDL3) relinked via `@rpath`, the icon from
`assets/icon/destrega.png`, ad-hoc signed. Options: `DESTREGA_MACOS_APP`,
`DESTREGA_APP_SIGN`, `DESTREGA_APP_VERSION`, `DESTREGA_APP_ICON`. The icon is
original procedural art from `tools/make_icon.py` (deterministic; needs
Pillow, NumPy and macOS's Herculanum font). Never put game-derived art in the
repo; the one exception (owner decision, 2026-11-11) is the few README screenshots in
`docs/screenshots/` (captured from this port, downscaled JPEGs). Launching the bundle's binary from a non-GUI shell can stall in macOS's
first-launch check; test it from Finder/`open`, or use the plain binary.

### Windows and Linux

Same CMake project; the compiler must be **clang or GCC** (the runtime uses `__int128`,
`__builtin_*`, `always_inline`, inline `asm` hints; MSVC/clang-cl are not supported). Needs
SDL **3.4+** (`SDL_CreateGPURenderer(device, window)`), zlib, Python 3 and the disc as on macOS.

- **Windows**: MSYS2, environment **CLANG64** (`pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja,
  pkgconf,python,sdl3,zlib}`), then the same `cmake -S . -B build/cmake -G Ninja && cmake --build
  build/cmake` from the CLANG64 shell; run `build/cmake/destrega.exe` with `clang64/bin` (SDL3.dll,
  zlib1.dll) on `PATH` or the DLLs copied next to it. Console-subsystem executable (logs on
  stdout/stderr). `runtime/win/destrega.rc` embeds a manifest with the UTF-8 code page (fopen/rename
  take the UTF-8 paths SDL gives, e.g. a non-ASCII user folder), and the main thread gets a 16 MiB
  stack (`--stack`; Windows' default is 1 MiB, macOS/Linux give 8). Data pack and cards live in
  `%APPDATA%\Destrega\` (SDL's pref path). Load Game Image restarts by `CreateProcess` + wait +
  exit with the child's code (`plat_exec`), so a waiting caller sees the restarted run.
- **Linux**: any distribution with SDL 3.4 (or build SDL from source, as CI does); data in
  `~/.local/share/Destrega/`. GCC and clang both build the runtime; clang is the one tested on
  the generated code.
- Platform differences live in `runtime/platform.[ch]` (file replace via `MoveFileEx`, `fsync`/
  `_commit`, `realpath`/`_fullpath`, `setenv`/`_putenv_s`, exec, executable path, thread CPU time,
  performance-core count, the hi-res worker pool's threads: pthreads or Win32 SRW locks +
  condition variables, the monotonic clock / sleep, the host call chain for save states:
  `backtrace` / `CaptureStackBackTrace`) and `runtime/platform_net.c` (this machine's IPv4
  addresses for Host Game: `getifaddrs` / `GetAdaptersAddresses`; separate so the unit tests that
  link platform.c need no socket libraries). Sockets stay in `net.c` (BSD sockets / Winsock; on
  Windows created with `WSA_FLAG_NO_HANDLE_INHERIT`: `SetHandleInformation` on a bound socket
  made it receive nothing under Wine). `\` is a path separator on Windows (cue -> bin, folders).
  Native menus on macOS (`menu_mac.m`) and Windows (`menu_win.c`); Linux gets the in-window menu
  bar (`menu_overlay.c`, glue in `menu_stub.c`; see "Application menus"). Alt+Enter/F11 full
  screen and F9 widescreen work everywhere.
- GPU renderer: Windows tries **Direct3D 12** (DXIL) first, then Vulkan (SPIR-V), then software;
  Linux Vulkan, then software (see "GPU renderer", "Graphics APIs"). `--renderer` works everywhere;
  View > Renderer on macOS and Windows.
- **Cross-compiling the Windows exe** (macOS or Linux, MinGW-w64 GCC: Homebrew `mingw-w64`):
  `cmake -S . -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
  -DCMAKE_PREFIX_PATH=<prefix>` where `<prefix>` holds SDL3 and zlib built for
  `x86_64-w64-mingw32` (SDL's `SDL3-devel-*-mingw` release, or SDL/zlib from source configured
  with the same toolchain file), then copy `<prefix>/bin/SDL3.dll` next to `build/win/destrega.exe`.
  With MinGW GCC the exe links `-static` (GCC's `_Thread_local` uses emutls on libwinpthread),
  so it needs only SDL3.dll plus the system UCRT. (A minimal zlib without the `gz*` file API is
  enough: replays are read with `inflate`, `replay.c read_maybe_gz`.) GCC builds the generated code fine (`__int128`
  included); it evaluates function arguments in a different order than clang, which once broke
  `--state-hash` (`f(p(&n), n)`: keep such calls in separate statements).
- **Wine / CrossOver**: `tools/test_netplay_wine.py` plays netplay between the native and the Windows build
  (see "Rollback netplay"); `tools/test_wine.sh` runs the same headless boot-to-fight (`--no-interp
  --verify-overrides`) natively and under Wine and requires identical per-frame state hashes
  and dumped frames (`WINE=` the wine command, `WINE_ARGS=` its options; CrossOver:
  `WINE=".../CrossOver-Hosted Application/wine" WINE_ARGS="--bottle destrega-test --no-gui"`, a
  dedicated bottle made with `cxbottle --bottle destrega-test --create --template win10_64`).
  Passes with CrossOver 26 (x86-64 under Rosetta): 4000/4000 frames identical at 1x and at 4x6 widescreen
  with `--hires-threads 6` (Win32 worker pool), frames dumped at 4x6 byte-identical too. Under CrossOver the GPU renderer runs on **Direct3D 12**
  (vkd3d over MoltenVK, adapter "AMD Compatibility Mode"): `--renderer gpu` at 4x6 gives the same
  state hashes and byte-identical dumped frames as Metal on the Mac, and `--renderer compare` (4x6,
  16:9, `--pgxp off`, 5000 frames, every 10th) 0 differing pixels in 500 frames on both. Its SDL_GPU
  Vulkan device (MoltenVK 1.2.10 via winevulkan) reports conformance version 0.0.0.0, and SDL
  rejects non-conformant Vulkan drivers, so Vulkan is refused there (greyed out in the menu).
  `--state-hash`/`--gameplay-trace` files are written in binary mode (LF) on every platform.
- Not verified on a real Windows/Linux machine with the game: there is no disc in CI. CI
  (`.github/workflows/build.yml`) builds everything except the generated code: configure with
  `-DDESTREGA_STUB_GEN=ON` to link the runtime against `tools/ci/stub_gen` (empty function table,
  stubs that exit) and `-DDESTREGA_TESTS=ON` for the CTest unit tests; the executable then starts,
  parses options and fails with "no data pack". Windows CI: MSYS2 CLANG64, no Vulkan driver and
  Microsoft's WARP for Direct3D 12 (CTest `hires_gpu` runs on D3D12 there and passes; `switch` and
  an explicit `vulkan` run, which must be refused cleanly, exit 77). Linux CI: gcc and clang, the GPU test on Mesa lavapipe
  under Xvfb, `tools/gen_spirv.py --check`, `tools/gen_dxil.py --check-fresh` and (clang job)
  `gen_dxil.py --check` with the official Linux DXC release downloaded.

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
  Done so far: `f_800465a4` -> `sphere_vs_poly` (collision.c). Presentation-only wrappers that run
  the original unchanged and bypass `override_call`: `f_80044210` (widescreen.c) and
  `f_80036138` / `f_800363c8` (char_mesh.c, PGXP faces).
  The game's stack moves into the scratchpad (0x1f800000) during fights.
- Cycle accounting: `CYC(n)` (2 cycles/insn), `EVT_CHECK()` at function entry and
  backward branches runs `rt_event()` → scheduler + IRQ delivery.
- **Idle-loop fast-forward** (`analysis/idle_loops.txt`: `<backward branch> <helper> <loop start>
  <fnv1a of the loop words>`; `recomp.py` checks the hash, codegen emits `helper(); EVT_CHECK();`
  on that edge; helpers in `recomp.h`). A helper advances the loop over iterations that provably
  only add cycles and change a counter, and never past the iteration whose `EVT_CHECK` would fire,
  so cycles, memory, registers and PGXP tags end exactly as if every iteration had run. Only one:
  `idle_vsync_wait` (libetc `vsync_wait` 8009b670, 14 instructions per spin while the vblank
  count 800bf2b4 is below the target; ~6400 spins per frame, 73 M in the Hills replay). Per-frame
  state hashes are identical with `--no-idle-skip` (runs every iteration) on five replays; the
  exit log prints `idle-skip: on, N loop iterations fast-forwarded`. The interpreter does not
  apply it (it only runs as a fallback). A new entry needs the same proof: list everything the
  loop body touches and show nothing but an event can change the exit condition.

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

**Policy.** The machine is shared: the old all-at-once `run_tests.sh` (load ~450 on 10 cores)
made the whole Mac unusable for an hour.
- Every PR: `tools/pr_check.sh` (a few minutes, `nice -n 10`, at most `PR_JOBS` (3) game processes).
- Before a release, or when explicitly asked: the full `tools/run_tests.sh`.
- Never two heavy test runs at once (yours or another agent's), and check `uptime` first: with a
  load already near the core count, wait or lower `PR_JOBS` / `JOBS`.

**PR check** (`tools/pr_check.sh [--all-groups] [--group NAME] [--base REF] [--dry-run]`): steps
one after another (`tools/testlib.sh`), each with at most `PR_JOBS` games at once; a summary table
with each step's result and time, the load average at start / end / peak (sampled every 10 s into
`build/pr/load.log`) and the exit status. Always: build (`-DDESTREGA_TESTS=ON`), the CTest unit
tests (without `hires_gpu*`), `smoke_test.sh`, `quick_check.sh`. Groups, picked from the files
changed since the merge base with `origin/main` (plus uncommitted / untracked files; the header of
the script has the exact patterns), each a small subset:
- **net** (`runtime/net*`, `netplay*`, `portmap*`, `platform_net*`, `smooth*`): `test_netplay.py`
  lan, wan, desync, menu_session (+ ping once it exists), `test_portmap.py`, and
  `test_netplay_wine.py` mac_hosts / win_hosts only when `build/win/destrega.exe` and CrossOver exist.
- **renderer** (`hires*`, `gpu.[ch]`, `pgxp*`, `shaders/`, `widescreen*`, `overrides/char_mesh.c`):
  CTest `hires_gpu*`, `test_hires_determinism.sh` with `CASES="s4x6 verify1 c4x6"` and
  `test_render_quality.sh` with `CASES="call169 cxbr43 glive"` (each plus its `off` reference).
- **menus** (`menu_*`, `settings*`, `input*`, `controls_*`, `main.c`): `test_menu.sh` (its ~1 GB of
  scratch is deleted after a pass), `test_input.py`, `test_governor.py` when present.
- **hlr** (`runtime/hlr/`, `hlr_hook.h`, `tools/test_hlr.py`, `runtime/tests/test_hlr.c`): CTest `hlr`,
  `hlr_gpu`, `test_hlr.py` audit / exact / hash / rewind on a subset (castle1 remains gate, Metal), and
  `test_netplay.py lan_hlr`.
- **core** (other `runtime/*.c|h`, `runtime/overrides/`, `recomp/`, `analysis/overrides.txt`,
  `extra_seeds.txt`, procedure / overlay lists, `CMakeLists.txt`): scenarios `vs_hills`, `story`,
  `option_game`, `practice`; replays `practice_gradd`, `opts_handicap_recover`,
  `vs_falma_reyus_cave`; `test_savestate.sh`; `test_bgm_loop.py` (a fight and a menu BGM played past
  their loop point; `tools/xa_loop_check.py` finds a stuck loop in the `--wav`).
- A group's own test scripts trigger it too; docs and other tools add nothing.

**Full suite** (`tools/run_tests.sh`, the release suite; same coverage as ever): runs under
`nice -n 10`, its steps one after another, each with at most `JOBS` game processes (default: half the
performance cores, 4 on an M1 Pro); `DRY_RUN=1` lists the steps, `ONLY="unit smoke ..."` runs only those. The sub-scripts take `JOBS` too
(`test_hires_determinism.sh`, `test_widescreen.sh`, `test_render_quality.sh`, `test_savestate.sh`,
`quick_check.sh`, `test_windowed_determinism.sh`, `test_menu.sh`, `test_input.py`; the Python
runners `-j`), and the hi-res and render-quality scripts `CASES="..."` for a subset.

`tools/run_tests.sh`: GTE/GPU/SPU/MDEC unit tests + a headless smoke test that
boots to a fight via scripted input and fails on FATAL / dispatch misses /
unhandled BIOS calls / black screen. Useful headless flags:
`--headless --frames N --dump DIR --dump-every N --press F:MASK,... --wav out.wav --trace`.
Button mask bits (active high in `--press`): Select 0x1, Start 0x8, Up 0x10,
Right 0x20, Down 0x40, Left 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800,
Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000.
`--press2 F:MASK,...` is the same for pad 2 (VS mode waits for it). Presses are taps (held 6 frames).
`python3 tools/contact_sheet.py out.png frames...` tiles screenshots for review.

**Quick check** (`tools/quick_check.sh`, ~25 s; part of `tools/pr_check.sh`, or alone after any
runtime/renderer change): two replays (`vs_tieme_rohzen_villa`, `1p_reyus_cpu`: every frame's
gameplay hashes and the state hash vs the recording); GPU == software (`--renderer compare
--pgxp off --compare-max 0`, every 10th frame) on the Hills match replay at the 3834x2328 window
(4x6 + widescreen guard band; hi-res from frame 3600 via `--resize`, so it is fast) with the
headless defaults and with xbr + full colour; then, alone, a timing run of the user configuration
(GPU, widescreen, 3834x2328 -> 4x6, xbr, full colour, PGXP on, 4000 frames) printing ms/frame per
segment against `tools/quick_check_baseline.txt` (one line per machine, keyed by CPU name;
`--update-baseline` writes this machine's). Timing only warns (> `TOL`% (25) and > 1 ms slower):
the machine is noisy. Exit 1 only for gameplay or renderer differences. Files in `build/quick/`.
The old Metal shader (64-bit divisions) shows as ~3x slower in every hi-res segment.

GPU backend checks: `run_tests.sh` also builds `runtime/tests/test_hires_gpu.c` (GPU vs software
on random command streams, no disc needed) and runs it on the default driver (`gpu-auto`, Metal
on macOS) and on Vulkan (`gpu-vulkan`: MoltenVK; "skipped" when there is no Vulkan loader). The
full-game Vulkan compare matrix is `tools/test_vulkan.sh` (a few minutes; not in run_tests.sh).
CI (`.github/workflows/build.yml`, Windows + Linux, no disc) runs the module unit tests and
`test_hires_gpu` through CTest; see "Windows and Linux".

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
subset (every stage; plus widescreen with PGXP on/off and the crack check `tools/check_ws_cracks.py`); `SCENARIOS=0 tools/run_tests.sh` skips them.
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
- **Netplay: no cards, nothing written.** A rolled-back frame must not leave files behind and the
  two peers' cards differ, so `netplay.c` flushes, detaches both slots (`memcard_attach(p, NULL)`)
  and sets `memcard_set_write_protect(true)` at the first vblank (before the game touches a card);
  the game sees no card on either peer. The Save Games actions and Load Game Image are refused
  during netplay (see "Rollback netplay"). Card state is part of every save state (`memcard_state_io`: images, changed flags, open files; a
  load that changes an image marks it dirty).

## Save states (`runtime/savestate.[ch]`)

Complete snapshots of the emulated machine, taken and restored only at the frame boundary.

- Every hardware module has one visitor `<module>_state_io(StateIO *)` at the bottom of its file
  (rt.c cpu, bus.c, bios.c, gpu.c, gte.c, spu.c, cdrom.c, mdec.c, sio.c, memcard.c) listing its
  state variables; the same visitor saves, loads and hashes (`SS_VAR`, `ss_io`, header-only so
  module unit tests still link alone). **A new piece of emulated state must be added to its
  module's visitor**, or rewinds will diverge (the tests below catch it). Scheduler callbacks are
  host pointers: saved raw, hashed only as null / non-null (comparable across processes).
  Presentation-only state is not saved; a load re-seeds the hi-res target from the 1x VRAM
  (`hires_reseed`) and drops PGXP associations.
- **Frame boundary**: `bus.c vblank_cb` calls `ss_vblank_begin()` before `frontend_vblank()` and
  `ss_vblank_end()` after it (inputs for the frame are final there). Guest code is a C call chain,
  so a snapshot is only restorable while the host is at the same point of that chain: the
  snapshot stores the host return-address chain (`backtrace()`; `CaptureStackBackTrace` on Windows),
  the host frame address and the shadow call stack, and `ss_restore` refuses a different context.
  Not snapshot-safe: inside an interrupt handler, or while `g_ss_unsafe > 0` (C helpers that keep
  guest state in locals: bios.c event delivery / SysEnqIntRP calls, override verification, the
  interpreter). Increment `g_ss_unsafe` around any new C code that calls guest code and keeps
  guest-derived values in locals across the call. Details and measurements: analysis/NOTES.md
  "Save states and the snapshot-safe point". In fights only every second vblank (the one where the
  game waits in `vsync_wait` 8009b670) is restorable: one point per 30 Hz update.
- Cost: ~3.6 MB per snapshot; save ~0.1 ms, load ~0.2 ms average (< 1 ms max).
- Re-simulated frames (`ss_resimulating()`): `frontend_vblank` only advances the SPU (735 samples)
  and the `--press` script; no presentation, audio output, dumps or per-frame logs.
  Windowed runs render exactly 735 SPU samples per vblank, always (see "Windowed determinism" below);
  `ss_deterministic_audio()` is now only informational.
- Options: `--savestate-test N:M,...` (save at N, restore at the first frame >= M with the same
  context, re-run; every frame's complete state hash must equal the first pass; a point whose
  context does not recur is retried later), `--savestate-rewind E:D` (the same every E frames for
  the whole run), `--savestate-inplace E` (save, overwrite all state with garbage, load: round
  trip), `--ss-hash FILE` (per-frame complete state hash of the final timeline), `--ss-stats F`
  (safe points and host contexts at vblanks from frame F). Failures exit with status 7.
- Tests: `tools/test_savestate.sh` (11 points across logo, FMV/XA, title, menus, character select,
  stage load and fight; a whole run rewinding every 37 frames with a memory card, whose per-frame
  hashes, `--state-hash` and card file must equal a plain run's; in-place round trips) and
  `scenarios.py savestate` (every scenario with rewinds every 41 frames + round trips every 13:
  `--state-hash` and complete state hash equal to a plain run). Both run in `tools/run_tests.sh`.

### Windowed determinism

Emulated state never depends on host timing, windowed or headless. The only thing that used to:
`frontend_vblank` rendered an extra 367 SPU samples whenever the host audio queue ran low, so two
windowed runs with identical input diverged at frame ~189 (audio device timing). Now the SPU advances
735 samples per vblank unconditionally; an underrun (queue < 2 blocks) is covered on the **host** side
by queueing a faded copy of the tail of the block just rendered (nothing is rendered, no state is
touched), the stream starts with two blocks of silence, and a block is dropped when 6 are queued.
`audio: N host-side underrun pads in F frames` is printed at exit (and per 600 frames with the fps
line). Other host-time readers (checked): `SDL_GetTicksNS` / `plat_mono_ns` only feed pacing and
statistics (main.c, savestate.c, net.c packet timers; netplay timing decides when a frame runs, never
its content), no `time()`/`rand()` reaches the guest (the RTC and `rand` are the guest's own), the
hi-res worker pool and the GPU renderer are presentation only (state hashes proven equal in
`test_hires_determinism.sh`), `s_fast` (Tab) only skips audio. `tools/test_windowed_determinism.sh`
(in `run_tests.sh`): two real windowed runs plus a headless one from boot into a fight, per-frame
`--state-hash` files byte-identical (needs a window and an audio device; ~80 s).

## Replays: gameplay regression tests (`runtime/replay.c`, `tools/replays.py`)

- `--record FILE`: both pads' input per frame (the pad state after the frontend's per-frame work,
  whatever produced it: keyboard, gamepad, `--press`), disc identity (SLUS_007.32 CRC + sector
  count), build identity (hash of the recompiled-function table, informational), a 16-bit hash of
  each gameplay field at the start of every frame (both fighters' health, gauge, position, FNV of the
  whole 0x1dc-byte fighter struct; round time, wins, RNG seed; addresses in analysis/symbols.txt) and
  the complete emulated-state hash (`ss_hash_all`) every 60 frames. Works windowed and headless
  (forces deterministic SPU rendering). Text format documented at the top of replay.c, may be gzipped.
  **Recordings hold inputs and hashes only, never game values** (no game data in the repo).
- `--replay FILE`: drives both pads from the file (other input is overridden), stops at its last
  frame, compares every frame. First divergence: `replay: DIVERGED at frame F` with every field
  marked DIFFERS / same, its live value, and its value at the last frame where everything matched;
  exit status 7. A differing complete state hash with equal gameplay fields is a warning
  (`--replay-strict` makes it fatal). Get the expected values by replaying with the old build and
  `--gameplay-trace`.
- Library (`tools/replays/*.dtr.gz`, `replays.py list`): see "Coverage" for the matrix. Random-input
  fights (seeded taps, chords, motions, charges, walks, jumps on both pads: KOs, round wins, a decided
  match): Gradd-Celia best-of-3 on Hills, and 11 more VS fights so all 12 stages are covered, plus a
  1P battle against the CPU. **`practice_<char>` (12)**: PRACTICE mode, the character performs the
  whole command battery of `tools/movelab.py` (84 inputs: taps, direction + button, qcf / qcb / dp,
  double taps, holds, button pairs, 80 frames apart), so every character's attack code runs.
  **Options and timer** (`timeout_*`, `opts_*`): GAME OPTION changed through the menu (ROUND TIME 30 s):
  both idle = two draws on time (round_wins 1,1 then 2,2), P1 / P2 ahead = won on time;
  handicap / life recover / round count changed. **`story_win_first_fight`** (heavy, 26k frames): story mode
  through the intro, wins the first fight with random input, SAVE DATA? NO and the next event scenes. `replays.py run` runs the default set; `--all` adds the heavy ones.
  `run` also replays three with `--savestate-rewind`, hi-res + widescreen and 4x4 / PGXP off (must still
  match) and checks that an edited input is reported as a divergence. Runtime: see "Coverage".
- **Re-recording (only for an intentional gameplay change)**: a failing replay means emulated
  gameplay changed. Find out why first (`replays.py trace NAME OUT` with both builds and compare the
  values). If the change is intended (e.g. an emulation fix), `python3 tools/replays.py record
  [NAME...]`, check the summary it prints (KOs, rounds, matches) and commit the new `.dtr.gz` files
  with the reason in the commit message. Adding one: append an `add_vs(...)` / `add_1p(...)` call.

## Coverage (`tools/coverage.py`, `tools/movelab.py`)

- `coverage.py matrix`: characters / stages / modes covered by replays and scenarios (static). 
- `coverage.py build` / `run` / `report [-v]`: a build with `-DDESTREGA_COVERAGE=ON` (`build/cov`,
  `COV_HIT(name)` at the top of every generated function, first-hit flag + name list written to
  `$DESTREGA_COVERAGE_OUT` at exit; zero cost in the normal build) runs all scenarios and replays
  (`DESTREGA_BIN` selects the binary in `scenarios.py` / `replays.py`) and lists recompiled functions
  never executed, per image. Numbers: analysis/NOTES.md "Gameplay coverage".
- `movelab.py probe|map CHAR`: experiments with command inputs against an idle P2 (gauge spend, damage,
  animation pointers; `DESTREGA_STRUCT_TRACE=1` adds both fighter structs to `--gameplay-trace`).

## Rollback netplay (`runtime/netplay.c`, `runtime/net.[ch]`)

GGPO-style rollback on top of save states, two peers, UDP. Started from the **Netplay** menu
(below) or the command line.

- **Netplay menu** (macOS `menu_mac.m` and Windows `menu_win.c`; top level, after View): Host
  Game… (port, default 7000, and input delay, default 2, both remembered in `settings.ini`
  `[netplay]`, see "Settings file"; the dialog lists this computer's IPv4
  addresses, `plat_local_ipv4`), Join Game… (address:port, remembered, and delay; a **Paste**
  button, since the game has no Edit menu and Cmd+V does not reach a Mac text field), **Copy My
  Address** (while hosting; titled "Copy My Home-Network Address" when the router gave no internet
  address), Disconnect (enabled while a session or an attempt runs), a disabled status line and
  **Copy Out-of-Sync Report Location** (enabled once this process wrote a desync report). Status
  line (`netplay_status_text`): Not connected / Waiting for an opponent on port N (your address:
  A:P, or what the router said, see "Automatic port forwarding") / Connecting to X:P... /
  Connected to X, ping N ms, connection excellent|good|fair|poor (+ "your opponent's computer is
  running slowly" / "this computer is running slowly") / Your opponent left / Out of sync since
  frame F (a report was saved). The quality word comes from the last second's ping, rollbacks/s
  and % of time waiting for the opponent (thresholds in `netplay_ui.c quality()`). While hosting
  or connecting a small non-modal panel says so with Cancel; hosting: "Give your opponent this
  address: A:P" (the router's public address) and the home-network address, or, without one, the
  router's answer, the home-network address and "Players outside your home network need your
  internet address instead", plus a **Copy Address** button and "Copied ... to the clipboard"
  after a copy. The game keeps running offline. The Host dialog has a check box "Ask the router to
  open the port automatically" (`[netplay] port_mapping`). UI words: Netplay, Host Game, Join
  Game, opponent (no console jargon). Menu actions (also `--menu-action`, every platform, Linux
  included): `netplay-host=PORT[,DELAY]`, `netplay-join=HOST:PORT[,DELAY]`, `netplay-disconnect`,
  `netplay-copy-address`, `netplay-copy-report`.
- **Netplay UI code** (`runtime/netplay_ui.[ch]`, presentation only, excluded from the build
  identity in `tools/source_ident.py`): status line, panel text (logged as `netplay: panel: ...`
  whenever it changes, on every platform), the copy actions and the parsing of typed / pasted
  input. netplay.c only exposes its state (`netplay_ui_info`) and calls `netplay_ui_panel_update()`;
  the menus call `menu_netplay_progress(text, copy)`. **Copy My Address** copies the router's
  external address and port when portmap.c has a public one (`portmap_address` = PM_RES_PUBLIC),
  else this computer's home-network address (a private one first, `plat_local_ipv4`) and says why
  (router still asking, CGNAT, refused / no answer, port opening off); it is refused when not hosting
  (`menu: REFUSED netplay-copy-address: not hosting a game ...`). `SDL_SetClipboardText` in a window
  (`frontend_copy_text`, main.c); headless runs only log `netplay: copy address: A:P (...)
  [headless: the clipboard is left alone]`. No outside "what is my IP" service is ever asked.
  **Input**: the Host / Join dialogs and the two actions trim spaces, tabs, line breaks, no-break
  spaces and quotes, take `host:port`, `host port`, `udp://host:port`, an address pasted into the
  port field (the port after the last `:`), and refuse with a plain sentence (alert; the dialog
  then opens again with what was typed): a port outside 1..65535, a missing port (actions; the
  dialogs add the remembered port), characters other than letters, digits, `.` and `-`, an IPv6
  address, an input delay outside 1..30.
- **Synchronised start = fresh boot on both sides.** The game is already running when someone
  picks Host / Join, so the two machines differ. A short lobby exchange runs between frames
  (`lobby_poll`, packet types 5-7): the joiner sends a hello every 200 ms, the host checks the
  same identity as the session handshake (protocol, disc, game code, emulation sources, rollback
  window) and either
  refuses with a plain-language reason (the joiner gets an alert; the host keeps waiting) or
  welcomes with the agreed delay (the larger of the two). Then both re-execute themselves
  (`frontend_restart_with` in main.c, the same clean restart as Load Game Image) with `--host` /
  `--join`, `--net-delay D` and `--net-from-menu`; the normal handshake runs at frame 1 of the new
  boot. Sending a running snapshot instead is not possible in general: a snapshot only restores
  under the same host call chain (savestate.c), which the other process does not have at an
  arbitrary moment. A restart keeps the future `--menu-action` steps, re-based to the new boot's
  frames, and drops earlier netplay options. Sockets are close-on-exec so the port is free again.
- `--net-from-menu` (set by that restart): every failure is a native alert and offline play, never
  an exit: no answer (join gives up after `--net-timeout` + 5 s = 20 s), refused settings / disc /
  version, port in use, the opponent not reconnecting after the restart (30 s), losing the
  connection (no data for `--net-timeout`, 15 s), the opponent leaving, and a desync (report
  written, then the session ends). A desync does not change the exit status in that mode.
- **Ending a session** (`session_end`: Disconnect, the opponent's BYE, lost connection, desync from
  the menu): sends BYE, prints the statistics, stops the hash log, re-attaches the cards the session
  detached and lifts the write protection, releases both pads (the frontend's own input, incl. a
  local pad 2, takes over next frame) and keeps playing offline from the current state. Command-line
  sessions end the same way when the opponent leaves or the link is lost (they used to exit).

- `--host PORT` (player 1) / `--join HOST:PORT` (player 2). Each peer's own input is whatever its
  frontend puts into pad 1 (keyboard, gamepad, `--press`); netplay writes both pads at the end of
  every vblank (`netplay_vblank_end`, from `ss_vblank_end`). Handshake: the joiner sends hello until
  the host answers; both check protocol version, disc identity (SLUS_007.32 CRC + sector count),
  build identity and equal `--net-delay` / `--net-window`, and refuse otherwise. `--net-port`
  picks the joiner's local port.
- **Build identity = game code + emulation sources, never the platform** (protocol 2). Two hashes:
  the recompiled-code table (`replay_build_id`: each recompiled function's address, overlay, bounds
  and the hash of its original MIPS bytes, so "which game code runs natively") and
  `DESTREGA_SOURCE_IDENT` (`tools/source_ident.py`, generated into the build directory by CMake):
  a hash of the emulation runtime (`runtime/*.c|h`, `runtime/overrides/`), the recompiler
  (`recomp/*.py`) and its analysis inputs (seeds, overrides, procedure / overlay lists), line
  endings normalised. Left out: what only one platform compiles or what is presentation-only and
  proven not to reach emulated state (menus, `platform*.c|h`, `hires*.c|h`, shaders, Windows
  resources, host input / settings / controls windows, the host's router port mapping `portmap.c|h`,
  the smoothness statistics and render-scale governor `smooth.c|h`, the Netplay status / panel
  text, copy actions and address parsing `netplay_ui.c|h`),
  plus tools / tests / docs. No compiler, OS or build type: the Mac (clang) and Windows
  (MinGW GCC) builds of the same commit have identical per-frame state hashes (`test_wine.sh`), so
  they emulate the same machine and must be allowed to play; any source change that could alter
  emulation is refused even if it looks harmless (both players update). The refusal is the
  plain-language "the two players run different versions of Destrega" with both ids.
  `--net-test-ident build=HEX|source=HEX` fakes a difference for tests.
- **Frames are vblanks (60 Hz)**, not game updates: inputs, packets, delay and window are counted
  in vblanks (the game polls the pad per vblank in menus, and fights only run every second one, so
  60 Hz is the only clock that covers both). Local input sampled at the end of frame F is used for
  F + delay (`--net-delay`, default 2, minimum 1). A missing remote input is predicted (its last
  known value) up to `--net-window` vblanks (default 8 = 4 game updates = 133 ms) past the last
  confirmed one; beyond that the peer waits.
- **Rollback**: a received input that differs from the one used marks the frame; at the start of
  the next vblank the newest snapshot at or before it whose host context matches is restored
  (`ss_restore`, savestate.c rules) and the frames up to now are re-simulated with the corrected
  inputs. Snapshots: one per vblank where restorable, a ring of 24. In fights only every second
  vblank's context recurs, so a rollback waits up to 2 vblanks for one within 2 frames of the bad
  frame (deeper restores re-simulate more). During re-simulation `ss_resimulating()` is true:
  `frontend_vblank` only renders (and discards) the vblank's audio, nothing is presented or queued
  to the audio device, hi-res rendering is off except for the last 3 re-simulated vblanks (so the
  target holds the final frame), cards are detached anyway.
- **Prediction only during a round** (`in_round`: `round_time` 800e379c advanced in the last 4
  vblanks). Menus, intros, KO / win animations and loads run in lockstep (input delay only):
  there the game leaves its loop at some point, and a misprediction found after the game left the
  snapshot's host context cannot be rolled back. Without this gate the `match` test hit ~10
  impossible rollbacks and desyncs at the end of each match (NOTES.md). An impossible rollback is
  still counted and logged as a last resort; desync detection then reports what it caused.
- **Time sync**: each packet carries the sender's frame and its frame advantage; the peer that is
  ahead by >= 1 frame on average slows down by that much (at most 8 frames), **spread over the next
  frames**: 1 ms per frame (2 ms while more than 2.5 frames are owed), i.e. 6-12% slower for a
  moment instead of a visible halt; then ~45 frames for both averages to show it before deciding
  again. Windowed, `netplay_pace_extra_ns()` is added to main.c's pacing tick; headless paced
  runs shift their pace origin. (It used to sleep the whole amount once a second: the picture froze
  for up to 133 ms, and windowed the frontend's pacing then caught up by running fast.)
- **Boot shortcut** (default; `--net-full-boot` turns it off, and both peers must agree: it is part
  of the handshake's settings check): a session's fresh boot goes straight to VS mode's character
  select, P1 = host, P2 = joiner. Frames 1-306 of the session play a fixed pad-1 script on both
  peers (`k_boot_vs`: Start at 210 skips the KOEI logo and the intro FMV into the title menu,
  Down, Down, Start at 300 on VS), whatever the players press meanwhile; the select screen is up by
  frame ~380 instead of ~1560 on the scripted normal path: 19.7 s sooner. It is ordinary emulated
  input, so both machines stay identical (no state comes from outside). After a match the game
  itself returns to the VS character select with a win counter (NOTES.md), so rematches never need
  the title. `--boot-to-vs` applies the same script offline (reference runs of the tests).
- **Smoothness instrumentation** (`runtime/smooth.[ch]`, presentation-only): windowed runs record
  every presented frame's interval and the host work behind it, stalls (waiting for the peer),
  time-sync slowdowns, rollbacks (depth histogram, re-simulation time, frames shown before the
  correction) and the audio queue (underrun = empty, low = < 1 frame, repeat = the deterministic
  top-up re-queued samples). At exit (`netplay_finish`, also offline) a `smooth:` line and a
  `smooth-json: {...}` line: interval p50/p95/p99/max/sd, frames held > 1.5 vblank and what held
  each (the largest of rollback / stall / sync wait since the last present if it explains at least
  half the overrun, else "slow host"), host busy % of the 16.7 ms budget, stalls, rollbacks, audio.
  `--smooth-from F` starts counting at frame F. Headless runs present nothing (only the netplay
  part is counted). `tools/netplay_smooth.py [solo|solo_win|mm_lan|mm_wan|mw_lan|mw_wide]` runs
  windowed peers at 60 Hz on a VS replay (Mac<->Mac clean or 20 ms + 10 jitter + 2% loss each way,
  Mac<->Wine with the software renderer there, optionally widescreen; `solo` = one offline window)
  and prints one row per peer with the load average. Results depend heavily on machine load:
  compare runs made back-to-back.
- **Scale governor** (`smooth.c`, presentation-only; offline too): while View > Resolution is
  **Auto** in a window, a host whose average frame work stays above the budget lowers its hi-res
  render scale one step (4x6 -> 4x4 -> 3x4 -> 3x3 -> 2x3 -> 2x2 -> 1x2 -> 1x1), and raises it again
  one step at a time (never above what Auto picks) once the work stays well below the budget.
  Load = this host's own work per frame / 16.7 ms (waits for the peer excluded), EMA over 0.5 s.
  Down: above **90%** for 1 s during a netplay session (a late frame drags the opponent: it waits for
  every late input) or above **95%** for 2 s offline (conservative, so normal play does not flap).
  Up: below **60%** for 3 s and the next step's cost, estimated as the load times the step's pixel
  ratio (an upper bound: the hi-res share of the work cannot be timed apart), below **80%**.
  Warm-up: the first 3 s after the governor starts (program start, Auto chosen, a session starting
  or ending) and 1 s after any scale change are not measured (pipeline creation, the new target,
  Wine/D3D12 shader compilation: in a Mac <-> CrossOver session these used to step the Windows side
  down to 1x1 for the whole session, since nothing ever raised it). Back-off: a scale given up is not
  tried again for 10 s; failing again within 30 s of being raised to doubles the wait (20, 40 s ..
  5 min). Logs `smooth: this computer is too slow for render scale AxB (...): lowering it to CxD
  [for the netplay session] at frame F (AxB not tried again for N s)` and `smooth: render scale AxB
  uses N% of the frame budget (about M% expected at CxD): raising it to CxD at frame F`; the exit
  summary counts both. While it holds the scale down, SSAA is capped to the lowered scale's pixels
  too (else a default 2x SSAA on a lowered 2x3 would render 4x6 again). Never touches a fixed
  resolution (menu, settings, `--scale`); `--no-scale-governor` turns it off. **A fixed resolution
  the computer cannot keep up with slows a netplay opponent down too** (every late frame is a late
  input). Test hook `--fake-load F-G:PCT[@SxxSy],...`: the governor sees PCT% of the budget on
  frames F..G (only at that render scale with `@`); windowed it is added to the measured load,
  headless the governor runs only with it (`tools/test_governor.py`, in `run_tests.sh`: warm-up
  ignored, down to 1x1 and back up, oscillation with back-off 10/20/40 s, 92% offline unchanged and
  stepping down on a netplay host, fixed choices untouched, resolution menu actions with dump
  sizes and the settings round trip, every state hash equal to a plain run). Each input packet also
  carries the sender's load (% of its frame budget, one byte at the end; older peers ignore it), and
  the status line says "Opponent's computer is running slowly" (peer >= 95%) or "This computer is
  running slowly".
- Windows: `plat_sleep_us` uses a high-resolution waitable timer; `Sleep()` rounds to the 15.6 ms
  system tick, which turned every 0.5 ms netplay wait into a 16 ms one.
- **Final frames and desync detection**: the state at the start of frame F is final once every
  input before F is confirmed and was used. Each packet carries the newest final frame's gameplay
  hash, its 16 field hashes (replay.c fields) and every 30 frames the complete state hash
  (`--net-hash-every`). A mismatch is logged (`netplay: DESYNC at frame F`), counted (exit status 7)
  and the first one writes `desync-F-host|join.txt` (both hashes, every field DIFFERS / same with the
  local value, last agreeing frame, link stats) and `desync-F-host|join.dtr`, a replay file
  (M2 format) of this peer's timeline: `destrega --headless --replay desync-F-join.dtr` re-runs the
  confirmed inputs offline and, on the peer that went wrong, reports its first wrong frame.
  `--net-desync-dir DIR` (default: cwd), `--net-desync-stop`, `--net-inject-desync F` (test fault).
- **Readout**: once a second the window title shows ping (RTT), delay, fps, rollbacks/s, % of
  vblanks predicted, % of time waiting, DESYNC; every 10 s the same line goes to stderr, followed
  by the frame-to-frame ping. Exit summary: frames, predicted / mispredicted inputs, rollbacks (max
  depth, deferred, impossible), re-simulated frames and cost, stalls, time-sync waits, packets,
  final frames, hashes checked; then `netplay: ping A ms average network round trip (median, min, max, N
  samples), F ms frame-to-frame`.
- **How the ping is measured**: every input packet carries the sender's clock (us), the last clock
  it saw from the peer and how long it held that one (10 us units) **since that packet arrived**,
  plus (appended after the load byte) how long since its frame loop processed it. RTT = arrival of
  the reply - our clock echoed - the peer's hold. Arrival times come from `net.c`: a receiver thread
  per socket blocks in `select` and queues each datagram with its time (macOS / Linux: the kernel's
  `SO_TIMESTAMP`, so a late thread wakeup on a loaded machine does not count; Windows / Wine: the
  time the thread read it). Packets are still **processed** only at the frame boundary (`receive()`
  pops the queue: `net_recv_ts`), so rollback, prediction and time sync are unchanged; the ping is
  display only (nothing reads `s_rtt`). The old figure used the times each side *looked* at the
  socket (once per vblank), so up to a frame of waiting on each side counted as network: loopback
  showed 19-21 ms; now 0.1-0.3 ms Mac <-> Mac, median 0.3 ms Mac host <-> Wine joiner and ~2 ms
  Wine host <-> Mac joiner (Wine adds spikes on a loaded machine). Windows: the thread waits on the
  socket's `WSAEventSelect` FD_READ event plus a high-resolution timer, not `select()`: with a
  `select()` loop in the receiver thread a Mac <-> Wine session fell to ~1 fps with multi-second
  round trips (cause not pinned down). Status line and menu
  (`Connected to X, ping N ms`) and the title show the network figure (one decimal below 10 ms);
  the frame-to-frame one (what the old figure measured: network + waiting for the next frame on
  both sides) is in the 10 s line and the exit summary. Protocol version 3 (us timestamps).
- **Transport** (`net.c`): non-blocking UDP over IPv4 (the host listens on IPv4 only), the host
  answers whoever says hello first. Packets carry every unacknowledged input (up to 120 frames),
  so loss only costs latency. A receiver thread per socket queues arriving datagrams with their
  arrival time (above); without it (thread start failed) the main thread reads the socket as
  before. `--net-sim LAT:JITTER:LOSS:REORDER[:SEED]` (ms, ms, %, %) delays /
  drops / reorders this peer's sends (deterministic per seed) for tests; the receiver thread sends
  the delayed packets when due (it looks every 0.5 ms while a simulated link is in use), so the
  simulated latency no longer rounds up to the sender's next frame. `--net-pace HZ` paces
  headless runs; `--net-hash-log FILE` writes every final frame's hashes (also without netplay, for
  an offline reference run); `--net-lockstep` never predicts.
- Tests: `tools/test_netplay.py` (in `run_tests.sh`): eleven cases (two extra desync link timings that also assert the report file holds its frame) plus `ping` (the exit summary's average network ping over loopback < 3 ms and with `--net-sim 20:0:0:0` each way 38-47 ms), each two headless processes over
  loopback with seeded VS menu + random fight input (`replays.py` scripts), paced at 240 Hz (4x real
  time, so the simulated latencies cover 4x as many frames): lan, wan (20 ms + 8 jitter, 1% loss, 2%
  reorder), lossy (15+15 ms, 10% loss, 10% reorder), asym (35 / 5 ms), a full best-of-3 match with
  delay 3, an injected desync, refused menu actions, and the two menu-session cases below. Pass: both peers' final-frame hashes equal each other **and an
  offline run of the same inputs shifted by the delay**, >= 95% of frames final, no desync, no
  impossible rollback; the desync case must be detected and its two report replays must behave
  (clean peer replays cleanly, faulty one diverges at the injected frame naming `p1.hp`). The report is deferred
  until the in-memory recording holds its frame (`replay_last_frame`: the hash check runs in `netplay_vblank_begin`,
  before `replay_vblank_begin` records that frame, so an early peer packet used to give a report file one frame short).
  `--pace 60` runs in real time. ~90 s.
- **Menu actions during netplay**: `frontend_menu_action_allowed()` (main.c) refuses the ones that
  change emulated state: `load`, `save-import`, `save-export` (it flushes the card first) and
  `save-delete`. The Mac menu greys them out (`validateMenuItem`), `--menu-action` logs
  `menu: REFUSED NAME: ...`. Presentation-only actions (renderer, widescreen, full screen, reveal
  in Finder) still run. Test: the `menu` case of `test_netplay.py`.
- Tests for the menu path (`test_netplay.py`): `menu_session` (two processes start offline, host
  and join by `--menu-action` mid-run, both reboot through the boot shortcut (both log it, both are
  on the VS character select at frame 400: frame dump of the `--boot-to-vs` reference run, equal
  hashes), play synced with the replay's picks moved 1180 frames earlier: final-frame hashes equal
  on both and equal to an offline run with the agreed delay; the other cases pass
  `--net-full-boot` so their replay scripts still start from power-on; the joiner disconnects, the host gets the
  "opponent left" alert, both keep playing offline to `--frames`) and `menu_fail` (a host with a
  different rollback window refuses with a reason, an unanswered join times out with an alert, all
  three keep playing offline, nobody restarts; also Copy My Address refused before hosting and the
  home-network address copied while hosting without a router, six mistyped Join Game inputs each
  answered by its alert, and a join padded with spaces and a line break accepted; the lone joiner
  starts once the joiner is done, so at most two games run at once). The `desync` cases also check
  that `netplay-copy-report` gives the absolute path of each peer's report.
- **Mac <-> Windows** (`tools/test_netplay_wine.py`, in `run_tests.sh`; skipped with a message
  when Wine, `build/win/destrega.exe` + `SDL3.dll` or the data pack is missing): the native build
  and the Windows build under Wine (`WINE`, `WINE_ARGS`, `WIN_EXE`, `PAK`; default CrossOver's wine
  with `--bottle destrega-test --no-gui`) over loopback, started with the menu actions
  (`netplay-host` / `netplay-join`) in both directions: `mac_hosts`, `win_hosts`, and both again
  with a lossy link (`--net-sim 15:10:5:5`: 15 ms + 10 jitter, 5% loss, 5% reorder each way). Each
  boots through the boot shortcut (both peers log it), plays the VS fight of a replay script, disconnects and continues offline; every confirmed
  frame's hash must match on both peers **and** an offline native run. `ident`: a Windows host
  claiming other emulation sources refuses the Mac joiner with the alert. Pace 120 Hz (2x real
  time; headless Wine runs ~1.3 ms/frame, so Wine is not the bottleneck there). Offline frames are
  not paced, so the script starts the Wine peer first and the native one once Wine is hosting /
  joining (Wine needs 2-3 s to start). ~3.5 min. `--windowed` runs both peers windowed at 60 Hz
  with real audio output (slow under Wine, not in `run_tests.sh`; alerts are logged instead of
  shown with `DESTREGA_TEST_NO_ALERTS=1`): the check that a windowed session (menu-started too) stays in sync
  with real audio output (the audio top-up that used to need `ss_deterministic_audio()` no longer exists).
- Windows specifics: the menu restart is `plat_exec` = `CreateProcess` + wait + exit with the
  child's code, so the parent process stays alive meanwhile; its socket is closed before (and not
  inheritable), so the child can bind the same port.
- **Automatic port forwarding for the host** (`runtime/portmap.[ch]`, gateway lookup
  `plat_default_gateway_ipv4` in `platform_net.c`): when a player hosts (Host Game, and the
  `--host` of the restarted session), a worker thread asks the router to forward the UDP port, in
  this order: **PCP** (RFC 6887) and **NAT-PMP** (RFC 6886), UDP to the default gateway's port 5351
  (PCP first; a NAT-PMP-only router answers it with "unsupported version"), then **UPnP IGD**
  (SSDP M-SEARCH, the device description, SOAP `GetExternalIPAddress` / `AddPortMapping` /
  `DeletePortMapping` on WANIPConnection:1/:2 or WANPPPConnection:1). The UPnP client is our own
  (~300 lines in portmap.c), not miniupnpc: four actions of one service, no third-party code with
  its own blocking timeouts on four build targets, no download of outside sources. Handled: URLBase
  / relative control URLs, chunked replies, namespace prefixes, error 725 (only permanent leases:
  retried with lease 0, logged, deleted at the end), 718 (port taken: `AddAnyPortMapping` on IGD v2,
  else the next 4 ports). The joiner maps nothing (it sends first; its own router lets the replies
  in). Every wait is a few seconds at most (PCP / NAT-PMP 250 ms doubling, 3 tries; HTTP 2 s connect,
  3 s reply; SSDP 2 x 1.2 s) and checks for a cancel; the frame loop only reads the published result
  (`portmap_tick` per vblank) and never blocks, except `portmap_finish` at exit (delete, at most 3 s).
  Lease 3 h (`--net-portmap-lease S`), renewed at half the granted lifetime (a failed renewal is
  retried after 15 s), deleted when hosting ends: Cancel / Disconnect, the opponent leaving or the
  link dropping (`session_end`), the handshake failing, Load Game Image, exit (`netplay_finish`, plus
  an `atexit`). **Restart into the session**: the mapping is kept, not re-made: `lobby_restart` calls
  `portmap_keep_for_restart()`, which puts the mapping (protocol, ports, lease, PCP nonce, external
  address, UPnP control URL / service / internal client) in `DESTREGA_PORTMAP_KEEP`; the old worker
  stops without deleting (POSIX `execv` ends it anyway; on Windows the waiting parent's worker and
  `atexit` see `keep` and leave it); the new process reads and clears the variable and renews that
  same mapping first (PCP refuses a second mapping of the port with another nonce, so it must be the
  same nonce), falling back to a fresh request.
  What the host sees (panel, status line, log `portmap: ...`): "Asking your router to open port N
  for the internet...", then "Your address for the opponent: A.B.C.D:PORT" (router's external
  address + external port); an external address that is CGNAT (100.64/10), private or otherwise not
  public: "Your internet provider doesn't allow incoming connections, so players outside your home
  network probably can't join (...)"; explicit refusals: "Your router refused to open port N. Turn on
  automatic port opening (UPnP or NAT-PMP) in the router's settings, or forward UDP port N ... by
  hand."; nothing answered: "Your router didn't answer; you may need to forward UDP port N to this
  computer by hand ...". Hosting continues in every case. Option `--net-portmap on|off` (locks the
  setting), setting `[netplay] port_mapping` (default on; also the Host dialog's check box); off in
  a window shows a hint to forward the port by hand. Not part of the build identity
  (`tools/source_ident.py` excludes portmap.[ch]; this change altered only main.c / netplay.c among
  covered files: the hooks).
- **Port-mapping tests never touch a real router**: fail closed. Only a windowed game asks the real
  router (main.c calls `portmap_set_windowed(true)` after creating the window; the default is
  off). An earlier version derived "headless" from scanning argv on the first argument that reached
  `portmap_parse_arg`, which never ran when no argument got that far, so headless netplay tests
  mapped real ports: never infer this from argument parsing. Headless runs map only with
  `--net-portmap-gateway HOST:PORT` (PCP / NAT-PMP server) or `--net-portmap-upnp URL` (device
  description; replaces SSDP), and then only the overridden protocols are tried (no gateway
  lookup, no multicast). `--net-portmap on` alone does nothing headless. `test_netplay.py` fails if
  a headless log has a `portmap:` line; the windowed tools that host on loopback
  (`netplay_smooth.py`, `test_netplay_wine.py --windowed`) pass `--net-portmap off`.
  `tools/fake_router.py`: PCP + NAT-PMP (one UDP port) and UPnP (HTTP, chunked description, SOAP),
  127.0.0.1 only, modes ok / unsupported / refuse / silent / permanent, external address settable,
  enforces the PCP nonce, records every request. `tools/test_portmap.py` (last in `run_tests.sh`, ~20 s):
  `natpmp`, `pcp`, `upnp` (map, renewals at a 2 s lease, delete on Cancel or at exit; `upnp` also
  checks no UDP was sent), `upnp_permanent` (725 -> lease 0, no renewals, deleted at exit),
  `refused`, `cgnat` (100.72.1.9), `noanswer` (all three silent: message after ~6 s of timeouts;
  frame loop measured while the worker waits: 5219 frames, longest gap 38 ms with 11 games running
  at once; the limits are >= 100 frames and < 1 s gap, below any single timeout, because inside the
  full suite the machine is so loaded that gaps of ~400 ms occur without any router), `off`, `no_override` (headless `--net-portmap on` without overrides: nothing), `plain` (a headless
  host with no port-mapping argument at all: nothing), `restart`
  (menu session host + join: one PCP map, the restarted host renews it with the same nonce, one
  delete after the opponent leaves). Every basic case also runs `netplay-copy-address` before
  hosting (refused) and while hosting: the router's address in the four mapping cases, else the
  home-network address with the case's reason. `--win` runs them with the Windows build under Wine as the host.
  Unit test `runtime/tests/test_portmap.c` (CTest `portmap`): PCP / NAT-PMP packets, UPnP description
  / SOAP / fault parsing, chunked HTTP, URL joining, address classes; prints the local default
  gateway (routing table read only).
- Not done / not verified: a real router (only the fake one: no PCP / NAT-PMP / UPnP packet of this
  code has been sent to real hardware), the Host dialog check box and the panel's router line by hand
  (compiled on both platforms), the Linux gateway lookup (compiled in CI only), IPv6 (PCP can map
  IPv6, the host listens on IPv4 only); NAT traversal or a server-assisted lobby (direct UDP only: the
  host's port is forwarded, automatically or by hand), spectators, the Linux menus by hand (only driven through `menu-test`), a real
  Windows machine (only Wine), the Mac dialogs, panel and greyed-out items by hand (compiled; two
  windowed processes were driven through the same actions with `--menu-action`),
  windowed-vs-windowed play by two people.

## Application menus (`runtime/menu.h`, `menu_mac.m`, `menu_win.c`, `menu_overlay.[ch]`, `menu_stub.c`, `main.c`)

macOS gets a native **File** menu inserted after the app menu (SDL3 keeps building the standard
app/Window menus; Quit still works). `menu_mac.m` (Cocoa, ARC, Apple only) only builds items; each item calls `frontend_menu_action(NAME, ARG)` which **queues**. `main.c`
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
- **View > Renderer**: the graphics APIs this platform offers (Metal, Vulkan on the Mac; Direct3D 12,
  Vulkan on Windows), a separator, Software; radio check marks, unavailable APIs greyed out. Action
  `renderer[=software|gpu|metal|vulkan|d3d12|compare]`, see "Graphics APIs". Then a separator and
  Exact / Fast (Approximate): `gpu-precision[=exact|fast]`, see "Fast GPU precision".
- **View > Resolution** (Mac and Windows; radio check marks): **Auto** (default) / 4×6 (Sharpest) /
  2×3 (Balanced) / 1×1 (Original). Auto renders at 4x6 (2048x1440 for the usual 512x240 display, plus
  the guard band in widescreen) whatever the window size, more only if the window needs it and
  `--max-scale` allows, never more than `--max-scale`; the scale governor lowers and raises it when
  this computer cannot keep up (see "Rollback netplay", "Scale governor"). The fixed choices are
  never changed by the governor. Action `resolution[=auto|4x6|2x3|1x1]` (none = next), saved as
  `[display] resolution`; `--scale N|NxM|auto` overrides it for the run. SSAA: the default 2x (GPU)
  only applies while Auto, and only within the 24-pixel cap (so not at 4x6); with a fixed choice
  only an explicitly chosen Anti-aliasing level applies (capped to render Sx*Sy <= 24 as always).
  A switch reallocates the target and seeds it from the 1x VRAM, like a resize.
- **View > Texture Filtering** (Off (Sharp Pixels) / Bilinear / Smooth 2D Edges + Bilinear 3D),
  **View > Smooth Colours** (checkable) and **View > Anti-aliasing** (Off / 2x / 3x / 4x), Mac and
  Windows: actions `tex-filter[=nearest|bilinear|xbr]`, `smooth-colours[=on|off]`, `ssaa[=1..4]`;
  see "Render quality". Texture Filtering ends with a separator and the check item **Smooth Distant
  Textures (Fast Renderer)**: action `tex-cache[=on|off]` (none = toggle), the fast precision's
  texture cache, see "Texture cache".
- **Netplay** menu (after View): Host Game…, Join Game…, Copy My Address, Disconnect, status line, Copy
  Out-of-Sync Report Location; see "Rollback netplay".
- **View > Controls…** (Mac, Windows; Linux: the in-window menu's Controls form) and app menu **Settings… (Cmd+,)** on macOS: the Controls
  window, see "Input, controls and settings". View toggles are saved in `settings.ini`.
- **Full screen** on every platform from the SDL event loop in `main.c` (so not a Cocoa-only
  feature): **Alt+Enter** and **F11**, plus **Ctrl+Cmd+F** on macOS, queue the action
  `fullscreen` (`=on|off`, none = toggle; one toggle per frame, because Ctrl+Cmd+F can arrive both
  as the menu key equivalent and as an SDL key event). These keys never reach the pad (the Enter
  of Alt+Enter is not START). macOS: **View > Enter Full Screen / Exit Full Screen** (title
  follows the state, Ctrl+Cmd+F; replaces SDL's own "Toggle Full Screen" item, which called
  `-[NSWindow toggleFullScreen:]` behind SDL's back). `SDL_SetWindowFullscreen` uses the desktop
  mode (a native full-screen Space on macOS); SDL keeps and restores the windowed size and
  position. In full screen the mouse pointer hides after 2 s without movement and **Esc** leaves
  full screen (windowed, Esc still quits). macOS: `SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY=1`
  keeps the menu bar reachable by moving the mouse to the top (SDL's default hides it entirely
  when full screen is entered through `SDL_SetWindowFullscreen`, i.e. every path here). The hi-res auto scale follows the new pixel size in `present_frame` like any resize
  (widescreen and both renderers included); the log shows `fullscreen: entered/left, window WxH
  px` and the new `render scale`. **macOS: the 60 Hz pacing wait runs the Cocoa run loop**
  (`menu_wait_until`, `nextEventMatchingMask` without dequeueing, then `SDL_PumpEvents` for anything
  that arrives) instead of `nanosleep`: in a full-screen Space the menu bar is drawn inside this
  process, and with the main thread asleep ~95% of every frame revealing and using the menus lagged
  badly (windowed the system draws the menu bar, so it never showed there). The wait is an ordinary
  Cocoa loop (dequeue + `[NSApp sendEvent:]`, as SDL's own pump; NSApp is SDL's application
  object); peeking without dequeuing hid the full-screen menu bar entirely. `DESTREGA_MAC_WAIT=
  sleep|peek` selects the old variants for diagnosis. While a menu is open the game pauses (AppKit's
  menu tracking runs inside `sendEvent`), as in any Mac app. **Start-up full screen**
  (`[display] fullscreen = on`) is entered from `menu_script_and_poll` once the window has focus
  (frame >= 10; frame 180 at the latest): entered at frame 0, before the app had finished launching,
  the full-screen Space showed no menu bar at all. **No `LSApplicationCategoryType` games in the
  bundle** (`make_app.sh`): as a game in full screen the app gets macOS Game Mode, which deprioritises
  other processes, among them the menu bar agent that drives full-screen menus (macOS 26+:
  `MenuBarClientCore`): menus lagged visibly behind the mouse while the game's main thread sat idle
  in the menu tracking loop; the same build without the category was smooth. Other platforms sleep. Headless runs have no window and only log the request.
- Test hook: `--menu-action FRAME:NAME[=ARG]` (repeatable) runs the same handlers on that frame:
  `load=FILE`, `reveal-game`, `reveal-saves`, `save-import=FILE`, `save-export=FILE`, `save-delete`,
  `widescreen[=on|off]`, `renderer[=software|gpu|compare]`, `resolution[=auto|4x6|2x3|1x1]`, `fullscreen[=on|off]`,
  `tex-filter[=nearest|bilinear|xbr]`, `smooth-colours[=on|off]`, `ssaa[=1..4]`, `gpu-precision[=exact|fast]`,
  `tex-cache[=on|off]`, `netplay-host=PORT[,DELAY]`, `netplay-join=HOST:PORT[,DELAY]`, `netplay-disconnect`,
  `controls`, `settings`, `controls-*` (see "Input, controls and settings"),
  `menu-test=dump|click=PATH` (Windows, windowed). Headless: no dialogs (the file argument is
  required), confirmations are auto-accepted and logged, nothing opens Finder, and card actions refuse to run
  unless `--data-dir` or `--memcard1` says where the cards are (never the user's real folder by accident).
  Log lines start with `menu:`; errors with `menu: ERROR:`.
- Tests: `tools/test_menu.sh` (in `run_tests.sh`): load from `.cue` and raw `.iso` into a scratch data dir
  with state hash equal to a plain run on that pack, 9 rejection cases, save import with backup (the game
  really loads the imported save: ROUND COUNT byte), export, delete, invalid files. It rebuilds
  a disc image from the pack with `tools/pak_to_bin.py` when `Destrega/Destrega.cue` is absent.
- **Windows** (`menu_win.c`): a Win32 menu bar on SDL's window (`SDL_PROP_WINDOW_WIN32_HWND_POINTER`),
  same items and rules: **File** (Load Game Image…, Show Game Files in Explorer, Save Games > Show
  Save Games in Explorer / Import / Export / Delete, Exit), **View** (Widescreen (16:9) with check
  mark, Renderer > Direct3D 12 / Vulkan / Software, Resolution > Auto / 4×6 / 2×3 / 1×1, Texture
  Filtering, Smooth Colours, Anti-aliasing, Enter / Exit Full Screen, Controls…), **Netplay** (Host Game…, Join Game…,
  Copy My Address, Disconnect, status line, Copy Out-of-Sync Report Location). The window procedure is subclassed: `WM_COMMAND` queues the action,
  `WM_INITMENUPOPUP` updates check marks, titles, the status line and greys out refused items
  (`frontend_menu_action_allowed`, as `validateMenuItem` on the Mac). SDL sizes the window by its
  client area and accounts for the menu bar, so the picture keeps its size; full screen covers the
  bar. Host / Join are a small modal form built in code (no resource script; values in
  `settings.ini` `[netplay]`), the waiting panel a modeless window with Cancel (queues
  `netplay-disconnect`); file dialogs and alerts are SDL's (native). Explorer: `ShellExecuteW`
  (folder) / `explorer /select,`. No keyboard shortcuts besides Alt+Enter / F11 / F9 (SDL keeps
  Alt from opening the menu bar). The UI text never says "memory card" or console names, as on the
  Mac.
- Test hook `menu-test` (Windows): `--menu-action F:menu-test=dump` logs every item with
  `[greyed]` / `[checked]` after the same update a menu opening does; `menu-test=click=Netplay/Host
  Game…` posts that item's `WM_COMMAND` like a click. `DESTREGA_MENU_AUTOTEST=1` accepts the Host /
  Join form after 0.7 s with the remembered values, `=2` cancels it after 3 s. Verified under
  CrossOver (windowed): the dump before / during hosting / after Disconnect (Host and Join greyed
  and Disconnect enabled while waiting, status "Waiting for an opponent on port N"), widescreen
  check mark, full screen in and out (title follows), Exit, both forms (screenshots), the panel.
  Headless (every platform) `menu-test` drives the in-window menu below instead.
- Not done: saves other than the game's own file, the Windows menus clicked by a
  person (only driven through `menu-test` under Wine), a real Windows machine.

### In-window menu bar (Linux; `--overlay-menu on` elsewhere)

`runtime/menu_overlay.[ch]`; `menu_stub.c` (non-Apple, non-Windows) routes `menu_reveal` (SDL_OpenURL
`file://`), `controls_open`, `menu_netplay_progress` and `menu_test` to it. **Why drawn by the frontend**:
SDL3 has no menu API on Linux, a GTK menu bar inside an SDL window is fragile across X11 / Wayland and
absent in full screen or on a bare compositor (Steam Deck style); an overlay works the same in every
case and needs nothing beyond SDL (SDL_Renderer rectangles / lines and SDL's built-in 8x8 debug font
scaled by whole pixels; no fonts or toolkits from elsewhere). File pickers and alerts stay SDL's dialogs
(portal / zenity on Linux).
- **What the player sees**: a hint "Menu: F10, Alt, right-click or the mouse at the top" for 6 s at
  start; the bar (File, View, Netplay) slides in while the mouse is near the top. Opens with F10, Alt
  pressed and released alone (not when a player has Alt bound), a right-click (popup at the pointer) or
  a controller's Guide button. Arrows / Return / Escape, mouse hover and click (outside = close),
  controller D-pad or left stick, South = choose, East / Back = back, Start / Guide = close.
- **Same items as the native menus** from one table (`k_items`: path, action, shortcut label): File (Load
  Game Image, Show Game Files, Save Games submenu, Quit), View (Widescreen, Renderer with this platform's
  APIs + Software + Exact / Fast, Resolution, Texture Filtering, Smooth Colours, Anti-aliasing, Enter /
  Exit Full Screen, Controls), Netplay (Host Game, Join Game, Disconnect, status line). Greying, check
  marks and titles follow `frontend_menu_action_allowed` and the `frontend_*` getters as in
  `menu_win.c`. Every item queues `frontend_menu_action`; Host Game / Join Game are overlay forms (same
  text, `[netplay]` defaults, validation and resulting action as `menu_win.c`), Controls an overlay form
  (device, key / controller column per button, press-to-bind through `input_capture_begin`, stick,
  deadzone, Reset with SDL's confirm). The netplay waiting panel (non-modal, Cancel) shows at the bottom.
- **The game waits while a menu or form is open** (`overlay_pause` in `frontend_vblank`, right after
  the queued actions: presents the last picture with the overlay, every input event goes to the overlay,
  audio paused, pacing restarts; the pause does not count as host work for the scale governor). Keys
  held when it opens are released; controllers reach no pad after it closes until all their inputs are
  released (`input_hold_pads`). F10 is a reserved key. During netplay a long pause stalls the opponent
  (as the Mac's menu tracking does).
- `--overlay-menu on|off`: default on in a Linux window, off on macOS / Windows (native menus there).
  Headless never draws it. Not in the netplay build identity (`tools/source_ident.py`).
- **Test hook** (`--menu-action F:menu-test=CMD;CMD...`, headless on every platform, windowed when the
  overlay is on): `dump` (`menu: item PATH [greyed] [checked]`), `click=PATH`, `open=PATH`,
  `context=X,Y`, `bar`, `key=NAME` / `key-down=` / `key-up=` (scancode names), `pad=BUTTON`,
  `mouse=X,Y`, `mclick=X,Y`, `rclick=X,Y`, `type=TEXT`, `controls`, `panel=TEXT`, `state`, `close`,
  `shot=PATH` (the picture letterboxed into the window / `--window` size with the overlay, drawn by an
  SDL software renderer), `hold=MS` (windowed: stay paused MS ms). Synthetic events go through
  main.c's own event handler (`handle_event`: press-to-bind, overlay, window keys, pads). Headless a
  menu still open after the step is closed (headless runs never pause).
- Tests: `tools/test_overlay_menu.py` (menus group of `pr_check.sh`, `run_tests.sh`; ~15 s, two
  processes): the dump; clicks, greyed refusals, the Join form sending `netplay-join ADDR,DELAY`; a key
  pressed while a menu is open never reaches pad 1 (`--input-log`), the same key without the menu or
  after it closed does; a 2400-frame run that opens every menu, form and the context menu, navigates
  with keys / mouse / controller, chooses presentation-only items and takes 8 screenshots has the
  same per-frame `--state-hash` as a plain run.
- Not done: run on Linux (built and tested headless on macOS only; Docker was unavailable), a person
  using it, IME / non-ASCII text (the font is ASCII; forms take ASCII), live pressed-button marks in
  Controls (the game is paused, nothing polls).

## Input, controls and settings (`runtime/input.[ch]`, `settings.[ch]`, `controls_mac.m`, `controls_win.c`)

**One place turns host input into the two pad masks per frame**: `input_frame(out[2])` in `input.c`.
`main.c` only forwards SDL events (`poll_events`: frontend keys first, then `input_handle_event`) and
ORs in the `--press`/`--press2` script (`host_pads`). Netplay and replays are untouched: they still
read / override `g_pad_buttons` in `ss_vblank_end` after that, so the local player's input reaches
the netplay stream exactly as before. libpad only ever polls a **digital pad** (verified with a
temporary SIO0 probe: commands are only `01 42`, never config/analog `43/44/4D`; see NOTES.md), so
the left stick is mapped to the D-pad, never reported as analog.

- **Devices per player**: `auto` (P1: keyboard plus the first controller; P2: the next controller),
  `keyboard`, `gamepad:GUID` (a specific controller, remembered by SDL GUID + name; "(not connected)"
  while absent), `none`. `input_assign()` gives specific GUIDs first, then keeps each player's previous
  controller (sticky: unplugging P1's pad never moves P2's onto P1), then connection order. Hot-plug via
  `SDL_EVENT_GAMEPAD_ADDED/REMOVED`; keys and buttons are released on focus loss.
- **Defaults**: P1 keyboard arrows, Z Cross, X Circle, A Square, S Triangle, Q/W L1/R1, E/R L2/R2,
  Return Start, Backspace Select; P2 keyboard I/J/K/L, M Cross, Comma Circle, U Square, O Triangle,
  7/8 L1/R1, 9/0 L2/R2, P Start, Backslash Select. Controller: positional (south = Cross, east = Circle,
  west = Square, north = Triangle), shoulders L1/R1, triggers L2/R2, Start, Back/Share = Select, D-pad,
  left stick on with a 50 % radial deadzone and 8 sectors of 45 degrees.
- **Reserved** (never bindable, never reach a pad): Escape, Tab, F9, F10 (in-window menu), F11, F12, the Cmd/Win keys and
  anything pressed with them, and the Alt+Enter / Ctrl+Cmd+F combinations handled before input.c.
  Bindings are **scancodes** (layout-independent positions), names from `SDL_GetScancodeName` with a
  few aliases (Comma, Period, Backslash...) and `#N` for nameless ones; controller inputs are SDL
  names (`south`, `dpup`, `leftx-`, `lefttrigger`...). Binding an input clears it from the player's
  other buttons.
- **Labels follow the controller**: PlayStation (Cross / Circle / L1 / L2 / Options / Share), Xbox
  (A / B / LB / LT / Menu / View), Nintendo (B / A / ZL / Plus) from `SDL_GetGamepadTypeForID`; unknown
  or none: A / B / X / Y. The game's own buttons keep their PS names in the left column.
- **Controls window** (macOS View > Controls…, also app menu **Settings…** Cmd+,; Windows View >
  Controls…): Player 1 / Player 2, Device, one row per game button with a keyboard and a controller
  column (click, then press; Escape cancels; status line says what happened, including refused keys),
  "Left stick moves too", deadzone, Reset to Defaults (confirmed), Done. Pressed buttons light up
  (10 Hz refresh). While a binding is armed the pads read 0 and the next key / button goes to the
  window, not the game; background controller events are allowed while the window is open. Native
  Cocoa (`controls_mac.m`, key codes mapped kVK -> SDL scancode) and Win32 (`controls_win.c`, set-1
  scan codes with the E0 bit -> SDL scancode, built in code, no resource script). Every change is
  applied at once and saved.
- Menu / test actions (`--menu-action F:...`, all platforms): `controls` / `settings` (open the
  window; headless only logs), `controls-dump`, `controls-reset=P1|P2|all`,
  `controls-bind=P1:cross=key:Z` / `=P2:l2=pad:lefttrigger` / `=P1:start=none`,
  `controls-device=P2:keyboard|auto|none|gamepad:GUID`, `controls-stick=P1:on|off`,
  `controls-deadzone=P1:40`.
- Test hooks: `--input-event F:EVENT,...` injects host events in headless runs through the same
  `input_handle_event` path (SDL virtual controllers: `pad-add:N[:ps4|xbox]`, `pad-remove:N`,
  `pad-down:N:south`, `pad-up`, `pad-axis:N:leftx:-0.8` (triggers 0..1), `key-down:Z`, `key-up:Z`);
  `--input-log FILE` writes `frame pad1 pad2` (hex, active high) whenever a mask changes.
  Headless runs open only SDL virtual controllers (`input_set_virtual_only`): a physical controller
  plugged into the test machine is logged as ignored and never reaches a pad.

### Settings file (`settings.ini`)

INI-like (`[section]`, `key = value`, `#`/`;` comments, UTF-8 BOM skipped, bad lines logged as
`settings: line N ignored` and dropped, never fatal), in the data directory (SDL pref path, same
folder as `destrega.pak`; `--data-dir` moves it). Written atomically (temp file, fsync,
`plat_replace_file`), 0.5 s after the last change and at exit. Keys:
`[display]` resolution (auto|4x6|2x3|1x1, View > Resolution; absent = auto; the older `scale`
key, auto|off|NxM, is still read when `resolution` is absent), max_scale, widescreen (absent = on in
a window), renderer (software|gpu|metal|vulkan|d3d12; gpu = the
platform's default API, as written by older builds; an API this platform does not offer is logged and
ignored), gpu_precision (exact|fast), tex_cache (on|off; the fast precision's texture cache),
fullscreen, window (WxH points);
`[netplay]` port, delay, join_address, port_mapping (on|off); `[player1]` / `[player2]` device, device_name, stick,
deadzone, `key.<button>`, `pad.<button>` (buttons: up down left right cross circle square triangle
l1 r1 l2 r2 start select; empty = unbound; keys absent = default).

- **Command-line flags win for that run and are never written back**: each flag locks its key
  (`settings_lock`), so a View toggle cannot save it either. `--widescreen` / `--no-widescreen`,
  `--renderer`, `--gpu-precision`, `--tex-cache`, `--scale` (locks `scale` and `resolution`), `--max-scale`, `--window`.
- **Headless runs never read or write a settings file** unless given `--settings FILE`; tests stay
  hermetic. `--no-settings` makes a windowed run use defaults and write nothing (the Wine netplay
  test uses it). View toggles that persist: Widescreen, Renderer (not `compare`), GPU precision, Resolution, Full Screen, window
  size (not while full screen or maximised).
- Netplay defaults moved from NSUserDefaults (`NetplayPort`, `NetplayJoinAddress`, `NetplayDelay`) /
  `HKCU\Software\Destrega\Netplay` (`Port`, `JoinAddress`, `Delay`) to `[netplay]`: copied once when
  the file has no value; the old values are left in place.
- Not part of the netplay build identity: `input.[ch]`, `settings.[ch]` and the controls windows are
  excluded in `tools/source_ident.py` (they only produce the pad masks that the netplay stream
  already carries, and the equivalence test below proves they match `--press`).
- Tests: `runtime/tests/test_input.c` (defaults, mapping, stick deadzone/sectors, rebinding,
  reserved keys, name round trips, labels, device assignment incl. sticky hot-plug, settings round
  trip, malformed / binary / huge files, locks, atomic save, unwritable path) and
  `tools/test_input.py` (in `run_tests.sh`): a 6000-frame run driven by a custom settings file plus
  injected key and virtual-controller events (PS4-type pad for P1, Xbox-type for P2, plus events that
  must not reach a pad, plus a mid-run `controls-bind`) gives the **same masks and per-frame state
  hash** as the equivalent `--press` run; hot-plug; persistence (flags not written, F9 refused);
  a garbage file left untouched; a headless run touches no file.
- Not tested with hardware: no physical controller was attached (only SDL virtual controllers);
  the Windows window was checked under CrossOver only.

## High-resolution rendering (`runtime/hires.[ch]`)

Presentation-only. `gpu.c` stays the authoritative 1x GPU: every GP0 command still runs
on the 1x VRAM and all CPU-visible state (VRAM reads, GPUSTAT, copies, CLUTs) comes from it.
Each drawing command is *also* forwarded through `g_hires` (a `HiresBackend`: command in ->
target out; swap it for a GPU backend later) to a separate target covering VRAM
(0,0)-(512,480) at independent integer scales Sx (horizontal) x Sy (vertical)
(`hires_set_region` changes the region). Nothing in the target is ever read by the emulated
machine, so resolution cannot affect gameplay (`tools/test_hires_determinism.sh` proves it by
hashing all emulated state per frame).

- **1x skip rule** (`gpu.c` header, NOTES.md "Rendering: what the 1x VRAM holds"): polygons,
  rects and lines whose draw area lies inside the 512x480 framebuffer region are **not**
  rasterized into the emulated VRAM (the game never reads drawn framebuffer pixels back; the
  u/v and colour ranges the hi-res clamp needs are still tracked). Fills, uploads, copies and
  everything outside the region are emulated as before. The rule depends only on the guest's
  draw area, so every renderer, scale, window, widescreen setting and netplay peer computes the
  same VRAM; it changed the complete state hash (VRAM is hashed) but no gameplay field: the
  replays' `s` lines were re-recorded for that reason only, and `--gpu-1x draw` (rule off, never
  in netplay) reproduces the old per-frame `--state-hash` byte for byte. It roughly halves the
  CPU time of a plain 1x run (fight 0.94 -> 0.50 ms/frame with `--gpu-1x skip`) and cuts the
  user configuration's main-thread CPU by ~25 % (that run is GPU-bound, so its wall time is
  unchanged). The picture comes from the hi-res target; a presentation-only **display copy**
  (`gpu_display_vram()`, never hashed or saved) receives every write including the skipped
  pixels where the 1x picture is needed: `--gpu-1x copy`, headless runs without hi-res (dumps),
  `--hires-verify`, and lazily when a window has to present a 15-bit display from 1x VRAM.
  `--gpu-1x auto|copy|skip|draw` (default auto). A target reallocated between frames (resize,
  resolution / renderer / SSAA / colour change) is seeded from the old target sampled at each 1x
  pixel centre (`keep_picture` in hires.c), so it shows no black frames; a save-state load
  (`hires_reseed`) seeds from the display VRAM, i.e. without the copy the screen region is black
  until the game redraws it (2 game updates; netplay rollbacks do not reseed). A tile map of
  skipped pixels logs every guest observation of them (`gpu-1x:` summary line: must stay 0).
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
  is identical to serial (unit-tested, state hashes unchanged). The job counter word holds tag, chunk count and next
  index together (`run_chunks`): with the count in a plain field, a worker 64 jobs behind could claim
  a chunk of the slot's next job and over-count `done`, so `par_rows` spun forever (seen ~1 in 3
  governor runs under load). Workers spin ~50 us after the
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
- Auto scale (windowed default, View > Resolution > Auto, `--scale auto`): **4x6** whatever the
  window size (a small window is downsampled at present), capped per axis by `--max-scale N|NxM`
  (default 4x6, see measurements in the PR), and lowered / raised by the scale governor when this
  computer cannot keep up ("Rollback netplay", "Scale governor"). Larger only if the window needs
  it and `--max-scale` allows: the window-fit pick below, per axis. **Window fit** (headless
  default, so the `--resize` tests keep their window-driven scales; it was the windowed Auto
  before): the 4:3 letterbox viewport (vw x vh) inside the window's physical pixels
  (`SDL_GetWindowSizeInPixels`; the window is HiDPI) is covered without ever upsampling at
  present: `Sx = ceil(vw / display_w)`, `Sy = ceil(vh / display_h)` using the actual display mode
  (normally 512x240), capped by `--max-scale`. (On a 1x Windows display the default window used to
  give only 2x3; now Auto is 4x6 there too.) `--scale N|NxM` forces a scale (0 = off). On a
  resize the scales are recomputed; if they changed, the target is reallocated and seeded from
  the current 1x VRAM.
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
  tests, `tools/smoke_test.sh` and the determinism script.

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
- **Character mesh: dropped faces (`runtime/overrides/char_mesh.c`, NOTES.md "Character mesh and
  PGXP")**: the character renderer keeps a face only if NCLIP on its *integer* positions is > 0,
  so every face that is degenerate at 1x is never sent (17-59 % of the faces facing the camera,
  more with distance). At precise positions those faces have area: without them the mesh has
  holes that flicker every update (the "torn limbs / smear through the face" once blamed on
  missing welding, and the shimmer of distant fighters). `f_80036138` / `f_800363c8` (listed in
  `analysis/overrides.txt`) read the face list before running the original unchanged, rebuild
  each dropped face that faces the camera at precise positions (the game's packet words and OT
  slot) and hand it to the second linked-list DMA hook (`g_gpu_ll_hook2`), which draws it into
  the hi-res target only (`gpu_shadow_packet_in_area`) when the DMA reaches that slot. Their
  vertices get `PgxpVert.cull`: never welded, and their triangles that face away at precise
  positions are left out of the hi-res target (`gp0_polygon`). Summary line `pgxp-mesh:`.
- **Welding (other meshes; see NOTES.md "GTE projection call sites")**: within one batch (one GPU
  linked-list DMA, or one vblank) every use of an SXY word gets one position: the first precise
  value seen for it, or the integer if it was first sent without one (`pgxp_weld`); it keeps
  shared words of the widescreen extras and the real stage in one place. Vertices on different
  integer pixels stay independent. A precise value whose W differs by more than 1.25x is another
  mesh on the same pixel (stage vertex w=9073 vs character vertex w=3065 on one word) and is
  welded separately, keeping its W. Character vertices (`cull`) are not welded: on a distant
  fighter the first-seen value moved ~28 % of the vertices by up to ~1 px, a different way every
  frame.
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

A second `HiresBackend` on SDL3's SDL_GPU: **Metal** (MSL source embedded in the C file),
**Vulkan** (SPIR-V, Windows/Linux, and macOS through MoltenVK) or **Direct3D 12** (DXIL, Windows).
Same
contract as the software backend: commands in, target out, presentation only. It is
**bit-identical to the software backend** at every scale tested (1x1, 2x3, 4x6, 8x8 over a
7000-frame fight run) and to the 1x VRAM at 1x1 (with `--pgxp off`; for `on` see the PGXP
section). Keep it that way: a rule change in `hires.c`
needs the same change in **both** shader sources (MSL in `hires_gpu.c`, GLSL in
`runtime/shaders/hires.glsl`, which produces both SPIR-V and DXIL: rerun both generators).
The header comment of `hires_gpu.c` has the details.

- **Shaders / drivers.** One backend, two shader sources, three bytecode formats picked by the
  device's API: MSL (Metal) as before, and `runtime/shaders/hires.glsl` (Vulkan GLSL 450, the same
  functions line for line) compiled by `python3 tools/gen_spirv.py` (glslangValidator, `spirv-val`)
  into the checked-in `runtime/shaders/hires_spirv.h`, and by `python3 tools/gen_dxil.py` into
  `runtime/shaders/hires_dxil.h` (both generated bytecode; regenerate both after editing the GLSL).
  No shader compiler is needed to build.
- **DXIL** (`gen_dxil.py`): GLSL -> SPIR-V (glslang, with `-DPORTABLE_MUL`) -> HLSL (SPIRV-Cross
  `--hlsl --shader-model 60`, Homebrew `spirv-cross`) -> DXIL (Microsoft's DXC, `-Gis -O3`, debug
  and reflection stripped, **signed**: D3D12 refuses unsigned DXIL; no Homebrew formula, take the
  official release from github.com/microsoft/DirectXShaderCompiler: `DXC=` names the command, e.g.
  `dxc.exe` under Wine, or the Linux build with `LD_LIBRARY_PATH` at its `lib`). Registers follow
  SDL's D3D12 convention (GLSL `set` = HLSL space: vertex uniforms b0 space1, textures/samplers
  t0-2/s0-2 space2, the storage buffer t3 space2, fragment uniforms b0 space3). SPIRV-Cross has no
  HLSL form of `uaddCarry` / `usubBorrow` / `umulExtended` / `imulExtended`, so `PORTABLE_MUL`
  replaces them with exact 32-bit code (16-bit halves; signed product = unsigned product minus the
  sign corrections); the SPIR-V build keeps the builtins and its bytes did not change. The header
  stores the GLSL's sha256: `gen_dxil.py --check-fresh` (no tools, CI) fails when the GLSL changed
  without regenerating; `--check` compiles and verifies without writing (CI's Linux clang job,
  with the downloaded Linux DXC). SDL's D3D12 driver needs shader model 6.0 and resource binding
  tier 2 for DXIL. Int64 and signed division: the same emulation as Vulkan (`uvec2` pairs,
  magnitudes). Edge rule: D3D12 fixes the top-left rule like Metal, so the hardware rasterizer is
  used (not forced to the exact shader test as on Vulkan). Blend constants are the same
  fixed-function setup. B5G5R5A1 render target: required as on the other APIs (supported by WARP
  and vkd3d); if a D3D12 device lacked it the API is reported unavailable. A bit-exact fallback
  would be an R8G8B8A8 target holding each 5-bit channel as c<<3 with the blend done in the shader
  (not implemented).
- **64-bit integers on Vulkan**: `shaderInt64` is an optional Vulkan feature and SDL_GPU does not
  enable it, so the GLSL keeps every 64-bit value as a `uvec2` (lo, hi) with exact wrapping
  add/sub/multiply (`uaddCarry`, `usubBorrow`, `umulExtended`, `imulExtended`: core SPIR-V). The
  three 64-bit divisions of the MSL are exact without a divide: the plane-equation fraction is split
  off into a 0/1 carry test, span edges and the perspective division use a float estimate corrected
  by integer multiplies, `(U1-U0)/n` (n <= 8) is a 16-bit long division. Signed 32-bit `/` and `%`
  on negative values are avoided too (GLSL leaves them loose): `floordiv`/`tdiv` work on magnitudes.
- **Edge rule on Vulkan**: Vulkan does not fix the rasterizer's tie-break for pixel centres exactly
  on an edge (Metal/D3D use top-left, which the integer path relies on), so with SPIR-V every
  triangle takes the exact shader edge test (F_EDGE: the triangle grown by 1 px, or its bbox for
  slivers). `DESTREGA_GPU_EDGE=0|1` overrides it (experiments; on MoltenVK both give 0 diffs).
- `--gpu-driver auto|metal|vulkan|d3d12` (also for headless/compare): which API the GPU backend
  uses; auto = the platform order (below). An API the platform does not offer is an error listing
  the ones it does. On macOS `vulkan` loads MoltenVK through the Vulkan loader; SDL only searches the
  default library paths, so `hires_gpu.c` sets `SDL_VULKAN_LIBRARY` to Homebrew's
  `libvulkan.1.dylib` (`brew install vulkan-loader molten-vk`) unless already set. The windowed
  GPU renderer is created on the backend's own device (`hires_gpu_create_device` +
  `SDL_CreateGPURenderer(dev, win)`), so its driver is the one the shaders exist for.
- **Graphics APIs** (`hires_gpu_api_*`, main.c "renderer" section). Order per platform: macOS
  Metal, Vulkan (available only when MoltenVK loads); Windows Direct3D 12, Vulkan; Linux Vulkan;
  Software always. Each API is probed once (`SDL_GPUSupportsShaderFormats`, cached; a device that
  then fails to create or to init is marked failed with its reason in the log) and the menu greys
  out what is unavailable (`frontend_renderer_available`). The windowed default walks the order and
  skips software adapters (WARP, llvmpipe/lavapipe, SwiftShader: `hires_gpu_software_adapter`) in
  favour of the next API or the software renderer; an explicit choice (menu, `--renderer API`,
  settings) takes them. `--renderer software|gpu|metal|vulkan|d3d12|compare`: an API name is
  `gpu` + `--gpu-driver API`; `gpu` is the platform default. Both flags keep working and combine
  (`--renderer gpu --gpu-driver vulkan` = `--renderer vulkan`); the flag locks the setting.
  The log line `hires-gpu: SDL_GPU device ready (API, driver, FMT shaders; device name)` says
  what runs.
- **Live API switch** (View > Renderer or `renderer=API`, between frames, also during netplay: it
  is presentation-only): the hi-res backend is detached, the old target released, the windowed SDL
  renderer and its textures destroyed (it does not own the external device), the backend shut
  down (`hires_gpu_shutdown`: every pipeline, buffer, texture and fence released, GPU idle) and
  the device destroyed; then a new device is created, the SDL GPU renderer recreated on it
  (`SDL_Vulkan_CreateSurface` etc. do not recreate the window, so the Win32 menu bar stays), the
  backend initialised and the target re-seeded from the 1x VRAM. If the new API fails, the old one
  is restored, else software; the window always ends with a renderer. Log: `hires: graphics A -> B
  in N ms` (Metal -> Vulkan 17-40 ms, Vulkan -> Metal ~3 ms on an M1 Pro under load). Headless runs
  switch the backend's own device the same way.
- Vulkan results (MoltenVK on Apple Silicon, 7000-frame fight, every 10th frame compared,
  `tools/test_vulkan.sh`): `--pgxp off` 0 differing pixels at 2x2 and 4x6, 4:3 and 16:9; `affine`
  0 at 4x6; `on` 24 (4:3) and 31 (16:9) pixels over 331 fight frames at 4x6 (Metal: 24 / 28; limit
  `CMP_MAX_PGXP`). `runtime/tests/test_hires_gpu.c` (random GP0 streams through gpu.c in compare
  mode: all polygon/rect/line kinds, semi modes 1-3, texture windows, dither, fills, uploads,
  copies, plus PGXP packets; 1x1/2x2/3x2/4x6, with and without the guard band): 0 differing pixels
  on Metal and on MoltenVK; it found a deliberately broken edge rule at every scale. Not run on a
  native Vulkan driver (NVIDIA/AMD/Intel) or a native D3D12 driver: that, and the semi-transparency
  blend rounding on other hardware (UNORM5 blending precision is implementation-defined), are
  unverified.
- Direct3D 12 results: on the Windows CI runner (Microsoft Basic Render Driver = WARP, Microsoft's
  own D3D12 implementation, ~10 min) `test_hires_gpu` 114 cases and `switch` 19, 0 differing
  pixels. Under CrossOver 26 (vkd3d over MoltenVK): `test_hires_gpu d3d12`
  114 cases, 0 differing pixels (PGXP perspective and every render-quality mode included);
  `switch` 19 passed; full game `--renderer compare` 4x6 16:9 `--pgxp off` 0 differing pixels over
  500 frames (131 fight frames); `--renderer gpu` 4x6 state hashes and dumped frames identical to
  Metal on the Mac (`test_wine.sh`).

- `--renderer software|gpu|metal|vulkan|d3d12|compare`. Default: **gpu in a window** on every
  platform (if no API in the platform order gives a device with a B5G5R5A1 render target, or the SDL GPU renderer or the
  backend cannot be created: `hires-gpu: GPU renderer unavailable ...` and software),
  software headless (the tests' reference). `--renderer software` forces software and the
  plain SDL renderer (no live switch to GPU then). **View > Renderer** (API items + Software,
  radio check marks; menu action `renderer[=software|gpu|metal|vulkan|d3d12|compare]`, no argument
  toggles software / GPU) switches at any time between frames: `hires_set_backend` +
  `hires_set_scale` at the current scale, i.e. the old target is destroyed and the new one seeded
  from the 1x VRAM, as on a resize (an API change also replaces the device, see above). The window always uses SDL's GPU renderer, which presents either target. `compare` draws every command on both
  backends, presents the software one and every `--compare-every N` frames (default 30) reads
  both back: `hires-compare:` lines at exit (all / fight frames, pixels differing, by >1 step,
  mask-bit only, worst frame); `--compare-max PCT` exits 6 if a fight frame differs more.
  Headless works (own device, needs `SDL_InitSubSystem(VIDEO)` with the default driver; the
  `offscreen` driver has no GPU backend). Without a usable device it logs `GPU renderer
  unavailable` and uses software. Windowed it presents straight from the GPU texture (SDL GPU renderer +
  `SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`); readback only for `--dump`, `--hires-verify`
  and compare (`HiresBackend.read`).
- Exactness: target B5G5R5A1 (shader writes PS1 red into `.b` = the low 5 bits, so the download
  is raw PS1 u16, alpha = mask bit), or R8G8B8A8 in full-precision colour (see "Render quality";
  both pipeline sets are created at init). Attributes are the 1x plane equations evaluated as exact
  rationals in 64-bit integers per pixel (Metal `long`; emulated with 32-bit pairs on Vulkan). Semi modes are fixed-function
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
  emu 1.3 ms + present 6.6 ms per frame. (Older numbers: see the next point.)
- **No integer division in the MSL per-pixel path** (header comment of `hires_gpu.c`): Apple GPUs
  emulate integer division, 64-bit very slowly. Before, the MSL divided 64-bit values ~11 times
  per gouraud textured pixel and was slower than the same shader on MoltenVK (which already had
  divide-free forms): user configuration (GPU, 3834x2328 window = 4x6, widescreen, xbr, full
  colour, PGXP on; M1 Pro with the GPU partly busy) headless fight 17-18 ms/frame on Metal, 9.5 on
  MoltenVK; now 5.2-5.5 on Metal. Windowed at that size: 49-55 fps with 30% of presents late ->
  a steady 60 fps (present+convert 14.5 -> 3.7 ms per fight frame). Bit-exact: compare vs software
  0 pixels with `--pgxp off`, the PGXP-on difference counts unchanged, `test_hires_gpu` 0.
  Any new per-pixel `/` or `%` in the MSL costs real time: use the float-estimate + exact-correction
  helpers (`floordiv`, `lceil`, `div24`) or precompute per primitive.
  The perspective u/v path (PGXP on) was the largest remaining shader cost: sharing one
  reciprocal of q for u and v (`div24r`), skipping `span_end` unless the run is the span's last
  and a shift for the common n = 8 took the user configuration's fight from 5.7 to 4.85 ms/frame
  (Metal, frames byte-identical). `shaders/hires.glsl` has the same form (`div24r`,
  `span_end_beyond`, `ltdiv8` on `uvec2` pairs; SPIR-V and DXIL regenerated): Vulkan compare
  counts unchanged (`test_vulkan.sh`: 0 pixels with PGXP off / affine, 24 (4:3) and 31 (16:9)
  with PGXP on, as before), `test_hires_gpu` 114/114 0 pixels on MoltenVK and on D3D12 under
  CrossOver. On MoltenVK the user configuration's fight showed no measurable gain (11.5-13.0
  ms/frame before and after, alternated A/B at a load of ~100: the 64-bit emulation and exact
  edge test dominate there).
- Not implemented / approximate: mode 0 + mask check, mode 0 with F=0 over odd B (off by one;
  mode 0 unused by the game), VRAM->VRAM copies that overlap themselves inside the target are
  chunked by rows, `hires-uv:` sample counts are 0 on the GPU (no shader counters), no DXBC
  (D3D12 shader model 5.1: devices without SM 6.0 get Vulkan or software), no `--hires-threads` (n/a).
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
  recent-word writes journalled and undone), so the wide pass leaves no trace in the real one.
  Extra (guard-band) packets carry the precise vertices the wide pass produced: `collect()` copies
  the pool entry each packet word's tag points to (before the restore undoes it), and
  `gpu_shadow_packet(w, n, pv)` hands them to `gp0_polygon`, which welds them like any other
  packet. So a word shared by an extra and real geometry gets one position in the whole batch.
- **Pitfall (fixed):** `gpu.c` has a second hi-res path for triangles outside the 1x draw area
  (all extras, and real ones beyond the edge). It built `HiresVtx` from the integer vertices only
  (and left `w` unset), so those triangles were drawn at integer positions next to precise
  neighbours: black diagonal cracks along shared edges in the guard band on every stage. Both
  paths now apply the PGXP offsets. Regression check: `scenarios.py hires` also runs every stage
  scenario with `--widescreen` at 2x3, PGXP on and off (same state hash, `pgxp-check` clean,
  shadow triangles included) and `tools/check_ws_cracks.py` on the dumped fight frames: thin
  black runs with lit pixels at both ends in the PGXP frame, lit everywhere within one 1x pixel
  in the PGXP-off frame, grouped, at least 16 1x pixels long and not axis-aligned. Before the
  fix it found 7..36 cracks per stage (7 frames each); after, 0 on all 13.
- Tests: `tools/test_hires_determinism.sh` adds `g4x6` (state hash), `gverify1` (1x1 ==
  1x VRAM), `c1x1` and `c4x6` (compare vs software, `--pgxp off`, `CMP_MAX` default 0 %), and
  `cp4x6` (compare with `--pgxp on`, `CMP_MAX_PGXP` default 0.001 %). `g4x6` runs with PGXP on
  and must pass the `pgxp-check`.
  `tools/test_widescreen.sh` adds `ws46g` (16:9 on the GPU), `wsrender` (renderer and
  widescreen switched back and forth mid-fight, state hash = 4:3 software run) and `wsapi` (the
  same through every graphics API: Metal / Vulkan / Software / compare and back, 9 switches, 6 of
  them device changes; same state hash).
  `runtime/tests/test_hires_gpu.c` takes `auto|metal|vulkan|d3d12|switch` (an API this platform
  does not offer or cannot create = exit 77, skipped); `switch` cycles every available API six
  times (shutdown, new device, init, compare at 2x2 and 4x6 with and without the guard band), as
  CTest `hires_gpu_switch`; `run_tests.sh` runs `auto vulkan d3d12 switch`.

## Render quality (`--tex-filter`, `--hires-colour`, `--ssaa`; View menu)

Three presentation-only improvements of the hi-res target, implemented the same way in the software
backend (`hires.c`) and the GPU backend (`hires_gpu.c` MSL + `shaders/hires.glsl`). All integer
math, so the two backends stay **bit-identical in every mode** (`--renderer compare` reports 0
differing pixels with `--pgxp off`, 4:3 and 16:9). No emulated state changes (state hashes equal
with every mode on). Options are plain global setters/getters in `hires.h`
(`hires_set_tex_filter`, `hires_set_colour`, `hires_set_ssaa`), no persistence.

- **Defaults**: windowed = `xbr` filter, full-precision colour, and 2x SSAA when the GPU renderer
  is active and the resolution is Auto (1 on software or with a fixed resolution; switching the
  renderer moves the default along unless `--ssaa` or the menu chose a value). While the scale
  governor holds Auto down, the cap is the lowered scale's Sx*Sy instead of 24. In a window SSAA is always capped to a render target of at most 24 hi-res
  pixels per 1x pixel (`ssaa_cap()` in main.c -> `hires_set_ssaa_cap(24)`: render Sx*Sy <= 24, i.e.
  never beyond 4x6), whether it is the default, a menu choice or `display.ssaa` in settings.ini, so
  it only applies to small windows (base Sx*Sy <= 6 for 2x, e.g. 2x3) and is off at the usual 4x6.
  Only a headless `--ssaa N` is uncapped (tests). Reason: 2x SSAA on a 4x6 base (8x12) measured
  49-87 ms/frame windowed (14 fps): a menu choice used to be uncapped and was saved, so one click
  left every later launch a slideshow. Headless = nearest, 15-bit, no SSAA: every existing test and hash is
  unchanged. `--hires-verify` only runs where it is meaningful (1x1, 15-bit, nearest, no SSAA:
  `hires_verify_possible()`).
- **Menus / actions** (Mac and Windows View menu; also `--menu-action`): Texture Filtering > Off
  (Sharp Pixels) / Bilinear / Smooth 2D Edges + Bilinear 3D = `tex-filter=nearest|bilinear|xbr`
  (no argument: next); Smooth Colours = `smooth-colours[=on|off]`; Anti-aliasing > Off / 2x / 3x /
  4x = `ssaa=1..4` (no argument: off <-> 2x). Each switch runs `hires_reconfigure()` between frames:
  a format or SSAA change reallocates the target and seeds it from the 1x VRAM, like a resize. The
  log says `hires: texture filter F, C colour, ssaa N (in use N, render scale WxH) at frame F`.

### Texture smoothing (`--tex-filter nearest|bilinear|xbr`)
- The fast GPU precision adds mipmapped, anisotropic 3D textures on top of these filters (the
  texture cache, "Fast GPU precision" > "Texture cache"; View > Texture Filtering > Smooth Distant
  Textures (Fast Renderer)). Everything below describes the exact path and the uncached fast path.
- Filtering happens **after the CLUT lookup**, on the 15-bit texels, with integer weights summing
  to 65536 (`tex_combine` / `tcomb`), result in 5.8 fixed point; modulation, raw texture, dither and
  semi-transparency then work as before (a single texel `t << 8` gives exactly the old result).
- **Transparency**: texel 0000 contributes no weight; the opaque taps are renormalised, and the
  pixel is transparent (discarded) when the opaque weight is below half. So a sprite edge is the
  sprite colour or the background, never darkened towards black (no fringe). The STP bit
  (semi-transparency / mask) is the weighted majority of the opaque taps.
- **No bleed**: the whole footprint stays inside the 1x-sampled UV range (the `uv_track_span`
  clamp of "High-resolution rendering", now always on when filtering): taps outside it are clamped
  to its edge texel (clamp-to-edge inside the range). Rects use the u/v range of the columns/rows
  they draw (`hires_rect_uv_range`). Texture windows are applied to every tap. Pages that overlap
  the framebuffer are sampled from 1x VRAM like before (the game never does this).
- **Bilinear**: sample point at the hi-res pixel centre without the PS1's +0.5 rounding bias,
  `i = floor(u - 0.5)`, fraction in 1/256.
- **xbr (default)**: a Scale2x/xBR corner rule for **2D** primitives, bilinear for **3D** ones.
  2D = textured rects, and triangles without PGXP depth whose u/v advance exactly ±1 texel per 1x
  pixel along x/y (`hires_tri_is_2d`: HUD, fonts, portraits, sprites). For the nearest texel E
  and the quadrant the sample lies in, if the two neighbours H (horizontal) and V (vertical) are
  equal, differ from E, and differ from their own outer neighbours, the corner beyond the diagonal
  |dx|+|dy| > 1/2 texel takes H's colour, with a one-hi-res-pixel ramp; otherwise E (nearest).
  Exact colour equality means noisy textures are left alone while fonts and HUD outlines get
  smooth diagonals and keep sharp straight edges. Bilinear blurs pixel art, so it is the 3D-only
  half. 3D textures (stage, characters) are photographic/noisy at 1x: bilinear removes the
  blockiness at 4x6 without inventing edges.
- No texture cache: every tap reads the 1x VRAM (software) / VRAM texture (GPU) directly, so
  there is nothing to invalidate. The software rect path precomputes per-column taps.

### Full-precision colour (`--hires-colour 15|full`)
- Target RGBA8 (u32 R | G << 8 | B << 16 | mask << 24 in software, R8G8B8A8_UNORM on the GPU)
  instead of 15-bit. Channels are on the PS1's 8-bit scale: a 15-bit value c is c << 3 (VRAM
  uploads, fills, copies and textures stay 15-bit sources), an untextured colour is the 8-bit
  vertex colour, a modulated texel `min((t5.8 * c) >> 12, 255)`, gouraud interpolation keeps all
  8 bits. Never dithered (dither only exists to hide 15-bit banding).
- Semi-transparency in 8-bit units, clamped like the hardware: B/2+F/2 = (B+F) >> 1,
  B+F = min(B+F, 255), B-F = max(B-F, 0), B+F/4 = min(B + F/4, 255). The GPU uses the same
  fixed-function blends on the UNORM8 target (exact for integers; the mode-0 F = 0 tie case of the
  15-bit target applies here too; mode 0 is unused by the game).
- The mask bit is kept in the alpha byte (set-mask, check-mask work as before).
- For opaque primitives `full >> 3 == 15-bit` exactly (unit test), so full precision only adds the
  bits gouraud shading, modulation and blending produce: smooth gradients and fog instead of
  15-bit banding.
- The 15-bit mode stays (default headless, `--hires-verify`, every bit-exact test).

### Anti-aliasing (`--ssaa N`, 1..4)
- Rendering at N x the scale and box-downsampling N x N blocks at present (rounded average of the
  8-bit channels, identical in `hires_view_to_rgba` and the GPU present pass). It is ordinary
  rasterization at a higher integer scale, so it is exact by construction (same rules, same
  compare) and also smooths texture/edge aliasing; MSAA was not used because its coverage samples
  cannot be reproduced bit-exactly in software.
- The base scale (auto from the window, capped by `--max-scale`, default 4x6) is multiplied:
  `hires_get_scale` returns the base, `hires_get_render_scale` the target's. N is lowered until each
  axis <= `HIRES_MAX_RENDER_SCALE` (16), each side <= `HIRES_MAX_RENDER_DIM` (8192, the GPU texture
  limit everywhere) and the target <= `HIRES_MAX_RENDER_PX` (34 M px, 136 MB in RGBA8); 4x6 with
  `--ssaa 2` = 8x12 = 4096 x 5760 (+ guard band in widescreen). If the allocation still fails the
  frontend logs it and turns SSAA off. Dumps (`--dump`) are written after the box filter.

### Tests and measurements
- `runtime/tests/test_hires.c`: filter footprint vs the 1x-sampled range (taps outside show up as
  foreign colours; 3D quad, 1:1 sprite quad, rects, x-flipped rect), transparent texels (no fringe,
  bilinear and xbr, 15-bit and full), the four blend modes' clamping in full precision and the mask
  bit, full >> 3 == 15-bit for opaque primitives, SSAA scales / limits / box average, 2D detection.
- `runtime/tests/test_hires_gpu.c`: every random command stream also in bilinear, xbr, full,
  2x SSAA and combinations, 1x1 .. 4x6, with and without the guard band: 0 differing pixels on
  Metal and on Vulkan (MoltenVK). It added a 2D sprite-quad generator (the xbr 2D path).
- `tools/test_render_quality.sh` (in `run_tests.sh`): compare runs per mode and combined, 4:3 and
  16:9, `--compare-max 0` and 0 differing pixels overall; state hashes with every mode on (software
  and GPU) and with the modes switched live through the menu actions.
- Measurements (Apple Silicon Mac, machine heavily loaded by other jobs: 1-min load 18-124, so
  treat as rough; A/B runs back to back, fight frames 3700-4500, `tools/fight_script.py`).
  Headless 4x6, ms/frame emulation + hi-res: GPU base 12.4, bilinear 13.1, xbr 13.1, full 12.4,
  2x SSAA (8x12) 46.0, all three 48.4; software (3 threads) base 11.6-12.6, bilinear 47, xbr 46,
  full 24. The GPU filter cost was ~4x higher before two fast paths (single opaque tap:
  `(s + 128) >> 8` without the division; bilinear taps with weight 0 are not fetched).
  Windowed GPU emu + present: 960x720 window (4x6) default 14.1 vs all off 12.4, `--ssaa 2`
  49.1; 640x480 (3x4) default 10.3, `--ssaa 2` (6x8) 24.6; 1280x720 16:9 (4x6) default 17.6.
  So explicit 2x SSAA does not hold 60 Hz on this machine at these window sizes, hence the cap.

## Fast GPU precision (`--gpu-precision exact|fast`; View > Renderer > Exact / Fast)

A second precision for the GPU renderer, next to the bit-exact one. **Exact stays the default.**
Fast draws triangles and rects with the hardware rasterizer and interpolators instead of exact
plane equations: ~4x faster on Metal and ~7x on MoltenVK in the fight (below), approximate
(not bit-identical to software), and presentation only like the rest of hi-res: per-frame
`--state-hash` equal to a software run (Metal, Vulkan, and Direct3D 12 under CrossOver, Hills
replay 4500 frames, 4x6 widescreen xbr full colour).

- **Where**: `runtime/hires_gpu.c` (header comment "Fast precision"; `fast_tri`, the `s_fast`
  branches of `be_tri` / `be_rect`; MSL `vs_fast` / `fs_fast`) and `runtime/shaders/hires_fast.glsl`
  (Vulkan + D3D12; `python3 tools/gen_spirv.py` and `tools/gen_dxil.py` now build both GLSL files,
  `--src hires_fast` one of them; `gen_dxil.py --check-fresh` checks every header, `hlr_dxil.h` included).
- **What is approximate**: coverage (the GPU's fill rule and sub-pixel snapping on float vertices:
  polygon edges, shared-edge ties), u/v (float interpolation: texel choice at exact ties; vertex
  u/v are shifted to hires.c's sample point and the perspective case uses clip w = 1 / q there, so
  it is close; hires.c itself computes perspective exactly only every 8 pixels), gouraud colour
  (float, screen-linear, no clamp to the 1x colour range). **Exact as before**: fills, CPU->VRAM
  uploads, VRAM copies, lines (the game draws none), the seed, 24-bit FMV (1x fallback), the
  guard band and shadow packets (same paths, only the triangles inside them are fast).
- **Mask bit**: as in exact, the target's alpha: set-mask writes alpha 1, check-mask is the blend
  factor ONE_MINUS_DST_ALPHA (the same 10 blend pipelines, keys NKEYS..2*NKEYS-1). The game never
  sets or checks the mask.
- **Semi-transparency**: the same fixed-function blends and the same two-pass split of textured
  primitives (opaque texels, then STP texels), so blending is exact given the same input colour.
- **Texture decode**: in the fragment shader from the R16_UINT 1x VRAM texture (the same dirty-block
  upload): texture window, 4/8-bit CLUT or 15-bit, texel 0 transparent, then the exact backend's
  integer modulation / raw / dither (only when hires.c dithers) / filters (bilinear, xbr with the
  same weights), sample point = interpolated u/v - 0.5, clamped to the 1x-sampled UV range.
- **Batching**: 28-byte vertices and 32-byte records (instead of 272-byte GPrim records); the same
  group / tile-mask ordering as exact (exact and fast keys share it, so command order holds).
- **Switching**: `--gpu-precision` (locks the setting), `[display] gpu_precision = exact|fast` in
  `settings.ini`, menu action `gpu-precision[=exact|fast]` (none = toggle; between frames, re-seeds
  the target with `hires_reseed`; allowed during netplay). Mac and Windows: View > Renderer, after
  Software, a radio pair "Exact" / "Fast (Approximate)" (applies whenever a GPU API runs). The log
  line `hires-gpu: SDL_GPU device ready (...; fast precision)` and the exit line `... N primitives
  in fast precision` say what ran.
- **Compare mode** (works with either precision): a `hires-compare-steps:` line per range (after `hires-compare:`, separate prefix so its parsers are untouched) gives
  the pixels off by > 1 / > 2 / > 4 15-bit steps (1 step = 8 of 255), the mean absolute error per
  channel and PSNR, and the worst frame for the tolerance. `--compare-tol PCT[:STEPS]` (STEPS 1, 2
  or 4, default 4): exit 6 if a fight frame has more than PCT% of its pixels off by more than STEPS.
  `--compare-dump DIR` (+ `--compare-dump-max N`, default 4) writes `frame_F_software.png`,
  `_gpu.png` and `_diff.png` (|a - b| x 4) of the first compared fight frames; tile them with
  `tools/contact_sheet.py`.
- **Results** (Hills replay, frames 3700-4500, 4x6 widescreen 3834x2328, xbr, full colour, PGXP on,
  every 10th frame): fight 14.8% of pixels differ at all, **0.135% by > 1 step, 0.057% > 2, 0.019%
  > 4**, mean abs error 0.13 of 255, PSNR 49.2 dB; worst frame 0.05% > 4 steps. 15-bit nearest
  (headless defaults, PGXP on): 0.081 / 0.041 / 0.016%, PSNR 46.0 dB. The differences are thin
  lines along polygon edges (the diff images look like a faint wireframe) and single texels.
  Metal, MoltenVK and D3D12 (CrossOver) give the same statistics within noise.
- **Unit test**: `test_hires_gpu fast [API]` (CTest `hires_gpu_fast`): the random GP0 streams in four
  modes (nearest; bilinear; xbr + full; full + 2x SSAA) at 1x1 / 2x2 / 4x6 with and without guard
  band, plus PGXP affine and perspective packets, against a generous tolerance (<= 3% of pixels
  > 4 steps and mean abs error <= 3 of 255 per compare; worst seen 1.8% / 1.9 on Metal, 2.1% / 2.2
  on D3D12). Random streams are much harsher than the game (huge random u/v gradients, noise
  textures). An off-by-one-texel u/v fails 24 of 48 checks; a missing transparent-texel discard
  is not caught (random textures almost never hold texel 0). Its PGXP pass now uses
  `gpu_shadow_packet_in_area`: plain shadow packets were not drawn at all without a guard band, so
  the exact test's PGXP cases had compared empty targets (still 0 differing pixels now that they draw).
- **Speed** (headless, `tools/quick_check.sh`'s bench configuration, M1 Pro, load 4.5-8, three
  A/B rounds back to back, fight segment 3700-4500 ms/frame emulation + hi-res): Metal exact 4.9,
  **fast 1.16**; Vulkan (MoltenVK) exact 9.8, **fast 1.3**. Windowed Metal (3834x2328 px, 4x6, widescreen, xbr, full colour): exact emu 1.22 + present 2.55 = 3.77 ms/frame, fast 1.11 + 0.15 = 1.27 (host busy 16% -> 7%).
- Not done: no fast lines (rare), no fast path for the exact backend's 1x1 `--hires-verify` (fast is
  never bit-exact), not run on a native Vulkan / D3D12 driver (only MoltenVK and vkd3d) or by a
  person in the menus.

### Texture cache (`--tex-cache off|on|verify[=N]`; fast precision only)

3D textured triangles of the fast path sample a cache of decoded textures with mipmaps and
anisotropic filtering instead of decoding 1x VRAM texels in the shader. The exact path never uses
it (stays bit-identical). Header comment "Texture cache" in `hires_gpu.c` has the details.

- **Key** = (texture page, CLUT position, depth 4/8/15-bit, texture window); for 15-bit pages the
  CLUT is ignored. Each entry is one layer of a 256x256 2D-array texture: premultiplied RGBA8
  (texel 0 = alpha 0, so transparent texels never bleed into filtering) plus an R8 array with the
  STP-bit coverage (the two-pass semi-transparency split reads it: STP where coverage >= half the
  alpha). Decoded on the CPU from the 1x VRAM with the texture window applied, mips are 2x2 box
  averages of the premultiplied texels (alpha-correct), made on the CPU too (identical on every
  API; `SDL_GenerateMipmapsForGPUTexture` exists on Metal / Vulkan / D3D12 but always does every
  layer of the texture and filters straight colour). The fragment shader un-premultiplies, discards
  below alpha 0.5 and rescales to the exact path's 5.8 texel (`rgb / a * 8160`), so modulation,
  raw texture, dither and blending run unchanged.
- **Filtering**: trilinear + anisotropic sampler (`DESTREGA_TC_LEVELS` 1-9, default 4;
  `DESTREGA_TC_ANISO` 1-16, default 16; anisotropy is a sampler field everywhere, on Vulkan it also
  needs SDL's anisotropy device feature, on by default), gradients from the un-clamped u/v, then the
  u/v clamp to the 1x-sampled range as before. `--tex-filter nearest` reads level 0 when magnified
  and uses the mips when minified. **2D primitives** (rects, `hires_tri_is_2d`) never use the cache:
  xbr's 2D rule and the HUD stay as they were.
- **Page edges / bleed**: layers are whole 256x256 pages, mips are built over the page and clamped
  at its border (no wrap). Within a page, sub-texture u/v rectangles are packed at 1-texel
  granularity (66 % of triangles have an odd edge, 87 % not a multiple of 4; NOTES.md), so a level
  >= 1 at a sub-texture border mixes in its neighbour. Measured cost: none visible at 4x6, where
  > 99 % of textured pixels are magnified and levels >= 3 are used by < 0.001 % of pixels; that is
  why levels stop at 4 (2 and 9 levels give the same picture). Per-sub-texture mips would need the
  game's rectangles, which only a high-level renderer has (docs/HIGH_LEVEL_RENDERER.md).
- **Validity / invalidation**: every 1x VRAM write (upload, fill, copy, and anything `mark_vram`
  sees) stamps a 16x16 block and a 64x256 tile with a write clock; an entry whose page or CLUT
  blocks are newer than its decode is re-hashed (FNV of the page rows / CLUT) and re-decoded only
  if the texels really changed. Texture windows are part of the key. Bounded: 128 layers (~55 MB
  with mips, `DESTREGA_TC_LAYERS`), LRU eviction of entries not used in the current frame; a full
  cache sends the triangle down the uncached path. `be_configure` (reseed / scale change) clears it.
- **Verify** (`--tex-cache verify[=N]`, default every 60 frames): re-hashes every entry the stamps
  call clean and logs `tex-cache: STALE ...`, and reads one layer back from the GPU per round,
  comparing colour and STP with a fresh decode. Exit lines `hires-texcache:` (lookups, decodes,
  re-decodes, evictions, entries) and `hires-texcache-verify:`. `DESTREGA_TC_DIAG=1` adds the key
  depth / window counts, u/v alignment and a texels-per-pixel histogram.
- **Switching**: `--tex-cache` (locks the setting), `[display] tex_cache = on|off`, menu action
  `tex-cache[=on|off]` (none = toggle, between frames, allowed in netplay). Mac and Windows: View >
  Texture Filtering > "Smooth Distant Textures (Fast Renderer)" (check mark). Default **on**.
- **Results** (Hills match replay, 11530 frames, 4x6 widescreen xbr full colour, verify every 10 /
  30 frames): replay state hashes match (Metal and Vulkan, so presentation only); 76 entries, 76
  decodes (33 ms in all), 43 re-decodes between rounds, 0 evictions, 0 STALE, read-backs 0
  differing. Compare vs software (fight 3700-3950, every 10th frame): cache off 0.084 % of pixels
  > 1 step / 0.0173 % > 4, PSNR 50.3 dB; on 0.114 % / 0.0243 %, 48.9 dB (no mips 0.084 % /
  0.0173 %; mips without anisotropy 0.142 % / 0.0288 %): mips move distant texels towards their
  average, which the point-sampling software reference does not do. Shimmer (frame-to-frame
  change not explained by a supersampled software reference, 8-bit units, fight rows): at 4x6
  cache on = off within noise (Hills 0.413 / 0.409, canyon 0.036 / 0.037) because almost nothing
  is minified; with `--tex-filter nearest` 3.34 vs 4.14 (software nearest). It pays off at the
  governor's lower scales (reference: software xbr at 4x the scale with `--ssaa 4`; shimmer S /
  error E, off -> on): canyon 2x3 S 0.053 -> 0.046, E 0.842 -> 0.720 (software xbr 0.054 / 0.773);
  Hills 2x3 unchanged (0.090 / 0.264 -> 0.090 / 0.268); Hills 1x1 S 0.121 -> 0.096, E 0.735 ->
  0.538 (software xbr 0.124 / 0.737). Mips do most of it (1x1 without mips 0.114 / 0.554),
  anisotropy keeps oblique floors sharp (1x1 without it E 0.760).
- **Speed** (quick_check bench configuration, fast precision, headless Hills replay frames
  3000-4000, three A/B rounds back to back, load 15-20 from other work): Metal 1.17 -> 1.11
  ms/frame, Vulkan (MoltenVK) 1.34 -> 1.22 (a second set at load 32-52: Metal 1.19 -> 1.10, Vulkan
  1.32 -> 1.16). The texel decode moves out of the shader; decoding is ~0.4 ms per new page,
  a few dozen pages per stage. Default on for that reason and for the lower scales.
- **Unit test**: `test_hires_gpu fast` runs with `--tex-cache verify` every frame (CTest
  `hires_gpu_fast`) and requires cached triangles, re-decodes, checks and read-backs, 0 stale and 0
  differing; the PGXP packets run with the cache off (random full-page u/v are minified noise, where
  mips are legitimately far from point sampling).
- Not done: per-sub-texture mips, 2D primitives, 15-bit pages are cached but the game draws none
  in 3D, GPU-side decode (the CPU decode is 0.4 ms per page), a native Vulkan / D3D12 driver.

## Widescreen (`--widescreen`; default on in a window, off headless)

Windowed runs start in 16:9 (default window 1280x720) unless `settings.ini` says `widescreen = off`,
`--no-widescreen` is given or View > Widescreen / F9 turns it off (saved). Headless runs stay 4:3
unless `--widescreen` is given, so every test's hashes and dumps are unchanged.

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
  override (`runtime/overrides/widescreen.c`): save scratchpad/CPU/GTE and open the RAM write
  journal, run the renderer with `g_widescreen_cull` on, interrupts held off (`next_event = ~0`)
  and hardware access blocked (`g_bus_sandbox` in bus.c, BIOS calls too; any access rejects the
  pass), record the 2048 OT slots, undo the journal and restore the rest, run it again normally (the run the game sees), record again; per slot the packets the
  wide pass has and the real one lacks (compared by payload) are the extras. The GPU linked-list
  DMA calls `g_gpu_ll_hook` per node; on reaching a slot the extras are drawn into the hi-res
  target only (`gpu_shadow_packet`: no 1x drawing, no IRQ, GPU state restored).
- **RAM write journal** (`mem_journal_begin` / `mem_journal_undo`, bus.c): the inline RAM store
  fast path tests `p < g_mem_wlimit` (psx.h; normally 0x800000, 0 while the journal is open), so
  during the wide pass every RAM store goes through `bus_write*`, which saves each 256-byte block
  before its first write; the undo copies those blocks back (~38 KB per pass in a fight instead
  of 2 MB in and out). PGXP RAM tag writes take the same detour (`pgxp_wslot`, 64-word blocks).
  `DESTREGA_WS_JOURNAL_CHECK=1` also keeps full copies and fails (`rt_fatal`) if the undo left
  any RAM byte or tag different. User configuration, Hills fight: 0.39 -> 0.19 ms per pass.
  Netplay re-simulation already runs without the wide pass except in its last 3 vblanks
  (`hires_for_resim` detaches `g_hires`, and `stage_render` only widens with a hi-res target).
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

## Launcher and game module (`-DDESTREGA_GAME_MODULE=ON`; `runtime/launcher/`, `runtime/game_*`)

Release form without game code (docs/RELEASES.md option A). The static build stays the default.

- **Layout.** The executable holds the runtime only; `gen/` + `runtime/game_module.c` form a
  shared library `destrega_game.<dylib|dll|so>`. Interface `runtime/game_abi.h`: the module
  imports every runtime global/function the generated code touches through `GameImports` (the
  module side compiles with `-include game_module_side.h`, which turns `g_cpu` etc. into
  pointers; hidden visibility) and exports the table, overlays and the generated functions the
  runtime calls (`GAME_ABI_GEN`, reached as `g_game.fn_*` via `game_syms.h`). One exported symbol
  (`destrega_game_module_init`); the dylib links nothing but libc's memcpy. `game_load.c`
  refuses a module whose ABI version, struct sizes, layout fingerprint (`game_abi_layout()`) or
  SDK id (sha256 of the headers it was compiled with, `tools/embed_files.py`) differ, with a
  plain message. **Changing a list in game_abi.h, psx.h/recomp.h/pgxp.h inlines or a struct in
  the layout list = bump `GAME_ABI_VERSION` (the SDK id changes by itself).** A new runtime
  global or function used by generated code must be added to the lists or the module fails to
  link (undefined symbol) in the module build.
- `-DDESTREGA_DEV_GAME_MODULE=ON` (default with the module layout) also builds the module from
  `gen/` next to the executable; `OFF` = the release layout (no disc, no gen/; CI builds it).
- **Lookup at start** (`launcher_prepare`, after `disc_verify`): `--game-module PATH` -> the
  dev module next to the executable (skipped with `--rebuild-game` / `--analysis-dir`) -> `<data
  dir>/game/destrega_game.<ext>` if `game/game.key` matches -> build. Key = sha256 over version
  tag, SDK id, recompiler id, every analysis input (embedded, or `--analysis-dir DIR`),
  SLUS_007.32 and all overlay files, compiler argv + full `--version` output, flags, target.
  Without any compiler an existing module is still tried (the ABI check protects).
- **Build** (worker thread; the main thread polls the UI): C recompiler (`recompiler.c`, port of
  `recomp/*.py`; **change both**, `tools/test_recomp_c.sh` must stay byte-identical) into
  `game/build/gen/`, the SDK headers into `game/build/sdk/`, units compiled largest first in
  parallel (`--game-jobs N`, default the performance cores) with `-O1 -w -fno-strict-aliasing
  -fvisibility=hidden [-fPIC]`, linked `-shared`, renamed into place, key written last.
  Compiler: `--game-cc "CMD ..."`, `DESTREGA_ZIG`, `<exe dir>/zig/zig`, `../Resources/zig/zig`
  (.app), then system `cc`/`clang`/`gcc`. Zig's caches stay in `game/build/zig-cache`.
  Log `game/build/build.log`; stderr lines `launcher: ...` with the time of each step.
- **UI**: Cocoa (`launcher_ui_mac.m`), Win32 (`launcher_ui_win.c`), SDL text (`launcher_ui_sdl.c`,
  Linux): "Preparing the game for this computer...", determinate bar (5% recompile, compile
  units, link), Cancel / Esc / closing the window; errors in an SDL message box with Copy Details.
  Headless prints progress every 10%.
- **Other options**: `--recompile OUTDIR` (C recompiler only, then exit).
- **Determinism** (verified): a module build gives per-frame `--state-hash` identical to the
  static build on vs_hills, `vs_tieme_rohzen_villa` and `1p_reyus_cpu` with `--no-interp
  --verify-overrides`, both for the CMake/clang dev module and for a module the packaged app built
  with its bundled Zig 0.17.1. `test_netplay.py lan` with a static host and a module joiner: ok. Netplay identity is unchanged by the
  layout: `replay_build_id` hashes the table only, and `source_ident.py` covers
  `runtime/launcher/recompiler.[ch]` but not the rest of `runtime/launcher/`.
- **Tests**: `tools/test_launcher.sh [--cc CMD] [--jobs N]` (scratch data dir under
  `build/launcher-test/`: import, first launch builds, second is cached, a changed analysis
  input rebuilds, the original inputs rebuild again; prints step times). `tools/test_recomp_c.sh`.
  `DESTREGA_BIN_JOIN=... tools/test_netplay.py lan` plays a launcher peer against a static one.
- **Packaging**: `tools/package_release.sh launcher mac|win|linux --zig DIR`, `tools/make_appimage.sh`,
  `tools/check_no_game_code.py PATH...` (also run by CI on the Linux launcher build); see
  "Releases".
- Measured (M1 Pro, shared machine, 4 jobs): system clang: recompile 0.3 s, compile 50 units
  39-46 s, link 0.3 s; bundled Zig (first use, load ~60): compile 93 s, link 16 s (Zig builds its
  compiler-rt into the cache once); cached launch: key check 0.04 s. `--recompile` output is
  identical to `gen/`. A Windows DLL cross-builds with `zig cc -target x86_64-windows-gnu`
  (exports only the entry point; not run). Under Wine / CrossOver the bundled Windows Zig cannot
  compile at all (`manifest_create Unexpected`: Wine's `NtLockFile` is incomplete; docs/RELEASES.md),
  so the Windows first launch with the game is untested until a real Windows machine runs it.
- **`--game-selftest DIR`** (needs `--data-dir`): the launcher's module build without a disc. The
  files of DIR (C sources + `files.cmake` listing `${GEN_DIR}/...`) replace the recompiler's output
  and the key hashes them instead of the analysis inputs and disc; then the normal compile / link /
  load runs and the process exits (0 = built or current and loaded, `launcher: self-test passed:
  ... built with VERSION [bundled zig|--game-cc|system]`). CI feeds it the stub sources
  (`tools/ci/stub_gen`, which in a module build has one table entry, since an empty module is
  refused, and traps instead of using stdio): the Windows job downloads Zig 0.17.1 (sha256 checked)
  next to `build/launcher/destrega.exe` and requires the bundled Zig to build and load the DLL,
  then "is current" on a second run; the Linux jobs run it with their gcc / clang.

## Native high-level renderer (`runtime/hlr/`, `--hlr`; docs/HLR_PLAN.md)

Presentation only, in phases (plan and results: `docs/HLR_PLAN.md`; game facts: NOTES.md "Stage
builders in full", "HLR phase 2a", "HLR phase 2b"). Phase 2a: the stage is captured and redrawn
exactly (`--hlr exact`). Phase 2b: `--hlr on` draws the stage in real 3D (below).

- **Hooks** (`runtime/hlr_hook.h`, table `g_hlr` in bus.c; all NULL = off): `stage_pre(mode)` /
  `stage_post()` around the real pass of `stage_render` (`overrides/widescreen.c real_pass`),
  `ll_filter(node)` per node of the GPU linked-list DMA (`bus.c`; `0xffffffff` = end of the walk),
  `on_restore()` from `ss_restore`. Hooks only read guest RAM / scratchpad; GTE work runs on a copy
  (`gte_exec_on`); they never call guest code or touch cycles, the scheduler, RAM or GPU state.
- **Capture** (`hlr_stage.c`): predicts every stage packet (all builders incl. the 0x2000
  subdivision ones), audits the prediction against RAM and the scene OT, and a clean frame becomes
  an `HlrFrame` (copies of the packet words, keyed by the OT it was linked into; two slots). A
  frame whose audit fails is not captured (`frames not captured`).
- **Drawing** (`hlr.c hlr_ll_filter`): when the walk enters a captured table the frame becomes
  active (and an empty `HiresBackend.external` pass is inserted: GPU backend only; phase 2b draws
  the native stage there); each node that is a captured packet whose RAM words still equal the
  capture is drawn into the hi-res target by `gpu_hires_replay` and sent to the 1x GPU only
  (`gpu_set_node_no_hires`); a changed packet ("stale") goes the normal way. The end of the walk
  consumes the frame; a save-state load or a mode / parts change drops every frame.
- **Modes**: `off`; `audit` (prediction + checks, draws nothing differently); `exact` (any hi-res
  backend; byte-identical to off); `on` (native 3D stage, needs the GPU renderer: Metal (MSL),
  Vulkan (SPIR-V) or Direct3D 12 (DXIL, `hlr_dxil.h` from `gen_dxil.py --src hlr`)). Default: on in a window, off headless. `--hlr-parts
  stage|chars|shadows|fx|story|all|none` (only stage exists). Settings `[display] hlr` (on|off),
  `hlr_parts`; menu actions `hlr[=on|off]`, `hlr-part=stage:on|off`; View > 3D Scene > Original /
  Native, Stage (Mac, Windows; greyed unless a GPU API renders). `runtime/hlr/` is outside the
  netplay build identity (`source_ident.py`), `hlr_hook.h` and the call sites are inside.
- **Native 3D stage (`on`, phase 2b)**: the capture also records every stage polygon (model
  vertices = world coordinates, normals / colours, its packet for UV / CLUT / page) of every object
  the flag rules let through, also those the PS1 culled; an object the stage renderer's own 4-point
  test culls at 4:3 only if the same test widened by the guard band passes (widescreen; `hlr_set_guard`
  from main.c), because that test culls some covering objects on purpose (Throne's VS camera is
  behind a wall). `hlr_native.c` schedules them into the GPU linked-list walk (header comment:
  positions, overlap intervals against the non-stage packets' GP0 boxes, fewest passes) and
  `hlr_gpu.c` draws each pass as a `HiresBackend.external` on the hi-res device: float camera from the
  GTE state (`hlr_cam.h`; `hlr_cam_clip` gives the clip-space form for any pass rectangle, guard band
  included), reverse-Z depth (D32, else D24) cleared per table, MSAA with resolve
  (`SDL_GPUTextureSupportsSampleCount`, lowered to what the target format takes), GTE fog and
  lighting per pixel (enhanced) or per vertex (ps1 look), textures from the texture cache's layers
  (`material_texture(key)`, keyed by content fingerprint for a later replacement pack; trilinear +
  anisotropic, point-sampled in the ps1 look), opaque then semi-transparent (blend from the tpage).
  The background gradient strips are drawn natively, stretched over the pass. The stage packets go
  to the 1x GPU only. `stage_render` skips the widescreen shadow pass while the stage is drawn
  natively (`g_hlr.stage_native`; still counts `g_ws_cull_hits`, so the presentation stays 16:9);
  software renderer / HLR off keep it. Characters, shadows, effects and the HUD stay on the
  original path and keep the PS1's order relative to each stage polygon; known limit: a character
  the PS1 drew between two overlapping stage polygons gets the order of its pass.
- **2b switches**: `--hlr-msaa 1|2|4|8` (default 4), `--hlr-near clip|ps1` (default clip: polygons
  crossing the near plane are clipped by the GPU; ps1 drops what the PS1 dropped as too near),
  `--hlr-look enhanced|ps1` (default enhanced); settings `[display] hlr_msaa`, `hlr_near`, `hlr_look`
  (flags lock them); menu actions `hlr-msaa=N`, `hlr-near=clip|ps1`, `hlr-look=enhanced|ps1`; View >
  3D Scene items (Enhanced / PS1 Look, Clip / Drop Near Polygons, Edge Smoothing Off / 2x / 4x / 8x).
- Exit lines: `hlr-audit:` (+ `-kinds`, `-alt`), `hlr-cost:` (host ms per stage pass), `hlr:` (frames
  captured / walked / dropped, packets drawn by the HLR, stale, external passes), `hlr-native:`
  (polygons recorded), `hlr-native-draw:` (passes and polygons per frame, culled, near-dropped,
  stale, without texture, suppressed, walks given up, GPU passes / draws / failed).
  `DESTREGA_HLR_DIAG=N` logs the first N native polygons that cover more than a quarter of a pass.
  `DESTREGA_HLR_TEST_DROP=N` leaves out every Nth packet (tests prove they notice).
- **Tests**: CTest `hlr` (no disc; also the float camera against the GTE's RTPS: within 0.75 px
  for z >= 4H, where the GTE's own integer view position moves SX by at most H / z) and `hlr_gpu` /
  `hlr_gpu_vulkan` (`test_hires_gpu external [API]`: empty external passes leave GPU == software, and
  a cube through `hlr_gpu_external`: face colours, depth test, MSAA resolve, 15-bit channel order;
  `external d3d12` passes under CrossOver (vkd3d) with the same cube pixel counts as Metal and MoltenVK);
  `tools/test_hlr.py [audit hash exact rewind]` (env `STAGES`, `API`, `AUDIT_MODES`, `HASH`; `hash`
  and `rewind` require the native draw line); `test_netplay.py lan_hlr`; pr_check group `hlr`.
  `tools/hlr_diff.py` (not a test): every VS stage key frame off / on at 4x6 16:9, contact sheets
  and a difference table in `build/hlr2b/diff/`. Must hold: audit 0 everywhere, state hashes on ==
  off, exact dumps byte-identical to off.
- **Direct3D 12** (checked under CrossOver 26, vkd3d over MoltenVK; Windows build, vs_hills 4x6 16:9,
  `--no-interp --verify-overrides`): `off` / `on` / `exact` per-frame state hashes equal to each other
  and to Metal on the Mac; `exact` and `off` dumps byte-identical to each other and to Metal `off`;
  `on` vs Metal `on`: 0.107 % of the pixels differ, by at most 1 15-bit step (8 of 255), none by
  more; same polygon / pass counts (114 polygons, 1.40 passes per frame, 0 failed). Not run on a
  native D3D12 driver or WARP.

## Releases (`docs/RELEASES.md`)

`gen/` is translated game code and is **never** published: CI has no disc, and no workflow,
artifact or GitHub Release may contain a playable build. Design (options A/B/C, recommendation,
notarization steps) is in `docs/RELEASES.md`.

- `tools/package_release.sh mac|win` zips a finished build on the machine that built it (Mac:
  `Destrega.app` via `ditto`; Windows: exe + DLLs found next to it / `--dll-dir` / `$MINGW_PREFIX/bin`,
  zip made with Python). A game build needs `--personal` and is named
  `...-PERSONAL-DO-NOT-REDISTRIBUTE.zip`; a stub build (marker string `DESTREGA-CI-STUB-BUILD` in
  `tools/ci/stub_gen/stub.c`) becomes `Destrega-STUB-NOT-PLAYABLE-...zip`. Output in `dist/`.
- CI packages and uploads only the stub (`STUB-NOT-PLAYABLE-windows|macos`, 3 days); a macOS job also
  runs `make_app.sh`. CI robustness: curl with `--retry-all-errors` and a libsdl.org fallback,
  SDL3 cache restore/save split with `restore-keys` (bump `SDL_CACHE_REV` to invalidate), MSYS2
  `cache: true` and a second setup attempt if the first fails.
- Versions: `VERSION` (base, e.g. 0.1.0) + git, via `tools/version.py` (`short|build|describe|rc-header`).
  A tag `vX.Y.Z` on the commit wins over `VERSION`. `make_app.sh` writes `CFBundleShortVersionString`,
  `CFBundleVersion` (commit count), `DestregaBuild` and `NSHumanReadableCopyright` (no personal
  names/emails). Windows: `runtime/win/destrega.rc` (manifest with Common Controls 6, icon
  `destrega.ico` from `tools/make_ico.py`, VERSIONINFO from the generated `destrega_version.h`).
  The Win32 dialogs in `menu_win.c` already use the system message font; only the manifest
  dependency was missing for the modern look. Checked under CrossOver (`--bottle destrega-test`).
- `THIRD_PARTY_LICENSES.txt` goes into every package; update it when linking anything new.
- **Launcher packages** (no game code; "Launcher and game module"): `package_release.sh launcher
  mac|win|linux --zig DIR [--bin PATH]` -> `Destrega-<version>-<os>-<arch>.zip` (mac: app made by
  `make_app.sh --zig DIR --entitlements tools/launcher.entitlements`, Zig in
  `Contents/Resources/zig`; win: exe + DLLs + `zig/`; linux: `make_appimage.sh`, needs
  `appimagetool` on PATH, never downloaded by the script). Refused unless the binary is a launcher
  build and `check_no_game_code.py` passes. Zig is never committed; the dev copy lives in
  `build/zig/`.

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
- Hi-res lines run Sa - 1 steps past their end (to cover whole 1x pixels); the gouraud colour
  used to keep stepping there and wrapped below 0 (software wrote a garbage u32, the GPU `& 255`),
  so the two backends disagreed on the last hi-res pixels of a fading line. Both now clamp the
  colour to the end colours' range (a no-op at 1x1). Found by checking the random compare test
  after every primitive (later primitives had hidden it); the game draws no lines.
- SPU speed-ups that rely on invariants (`spu.c`): `spu_render` ticks only the voices in its
  `live` set (on, sweeping, output != 0, or the SPU IRQ on); a new per-voice effect that can act
  on an off voice must widen that test. The reverb output filter relies on `rv_out_hist` holding
  0 at every even `rv_pos` (only `reverb_tick` writes it). Checks: `--wav` and `--state-hash`
  byte-identical to the previous build on a replay.
- CD GetlocL must answer the last sector *read*, XA-ADPCM sectors included (`cdrom.c read_tick`):
  the game polls it every 2 vblanks to find a BGM track's end and re-seek. Updated only for data
  sectors, it stayed "past the end" after the re-seek and the game re-seeked every 9 vblanks forever
  (one note of the BGM repeated). `DESTREGA_XA_TRACE=1` logs sectors and GetlocL answers; NOTES.md
  "CD: GetlocL and the BGM loop".
- GTE `mvmva_core` fast path assumes 16-bit matrix elements and vector components (the registers
  are stored sign-extended / saturated); a change that lets wider values in must drop it.
- Fights run at 30 updates/s by design (`f_80027218(1)` -> `VSync(2)`); not a bug.
- Windowed pacing (`frontend_vblank` end, main.c): `s_next_tick_ns` is when the current frame
  should end. A frame more than 4 frames late restarts the clock at "now" *without* sleeping; it
  used to reset and then add a frame, so every very slow frame slept another 16 ms on top.
