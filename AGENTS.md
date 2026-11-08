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
  Native menus on macOS (`menu_mac.m`) and Windows (`menu_win.c`, see "Application menus"); Linux
  has none (`menu_stub.c`). Alt+Enter/F11 full screen and F9 widescreen work everywhere.
- GPU renderer: Vulkan (SPIR-V) on both, the windowed default when a device exists, else software
  (see "GPU renderer"). `--renderer` works everywhere; View > Renderer on macOS and Windows.
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
  with `--hires-threads 6` (Win32 worker pool), frames dumped at 4x6 byte-identical too. Windowed under CrossOver works with the software renderer; its SDL_GPU
  Vulkan device (MoltenVK 1.2.10 via winevulkan) reports conformance version 0.0.0.0, and SDL
  rejects non-conformant Vulkan drivers, so the GPU renderer falls back to software there.
  `--state-hash`/`--gameplay-trace` files are written in binary mode (LF) on every platform.
- Not verified on a real Windows/Linux machine with the game: there is no disc in CI. CI
  (`.github/workflows/build.yml`) builds everything except the generated code: configure with
  `-DDESTREGA_STUB_GEN=ON` to link the runtime against `tools/ci/stub_gen` (empty function table,
  stubs that exit) and `-DDESTREGA_TESTS=ON` for the CTest unit tests; the executable then starts,
  parses options and fails with "no data pack". Windows CI: MSYS2 CLANG64, no Vulkan driver (the
  GPU test checks it is refused cleanly). Linux CI: gcc and clang, the GPU test on Mesa lavapipe
  under Xvfb, and `tools/gen_spirv.py --check`.

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
subset (every stage; plus widescreen with PGXP on/off and the crack check `tools/check_ws_cracks.py`)
concurrently with the rest; `SCENARIOS=0 tools/run_tests.sh` skips them.
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
  Game… (port, default 7000, and input delay, default 2, both remembered: NSUserDefaults on the
  Mac, `HKCU\Software\Destrega\Netplay` on Windows; the dialog lists this computer's IPv4
  addresses, `plat_local_ipv4`), Join Game… (address:port, remembered, and delay),
  Disconnect (enabled while a session or an attempt runs), and a disabled status line
  (`netplay_status_text`: Not connected / Waiting for an opponent on port N / Connecting to X /
  Connected to X, ping N ms / Desync at frame F). While hosting or connecting a small non-modal
  panel says so with Cancel; the game keeps running offline. UI words: Netplay, Host Game, Join
  Game, opponent (no console jargon). Menu actions (also `--menu-action`, every platform):
  `netplay-host=PORT[,DELAY]`, `netplay-join=HOST:PORT[,DELAY]`, `netplay-disconnect`.
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
  resources), plus tools / tests / docs. No compiler, OS or build type: the Mac (clang) and Windows
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
  ahead by >= 1 frame on average waits (at most 8 frames, once a second), like GGPO.
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
  vblanks predicted, % of time waiting, DESYNC; every 10 s the same line goes to stderr. Exit
  summary: frames, predicted / mispredicted inputs, rollbacks (max depth, deferred, impossible),
  re-simulated frames and cost, stalls, time-sync waits, packets, final frames, hashes checked.
- **Transport** (`net.c`): non-blocking UDP over IPv4 (the host listens on IPv4 only), the host
  answers whoever says hello first. Packets carry every unacknowledged input (up to 120 frames),
  so loss only costs latency. `--net-sim LAT:JITTER:LOSS:REORDER[:SEED]` (ms, ms, %, %) delays /
  drops / reorders this peer's sends (deterministic per seed) for tests. `--net-pace HZ` paces
  headless runs; `--net-hash-log FILE` writes every final frame's hashes (also without netplay, for
  an offline reference run); `--net-lockstep` never predicts.
- Tests: `tools/test_netplay.py` (in `run_tests.sh`): eleven cases (two extra desync link timings that also assert the report file holds its frame), each two headless processes over
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
  and join by `--menu-action` mid-run, both reboot, play synced: final-frame hashes equal on both
  and equal to an offline run with the agreed delay; the joiner disconnects, the host gets the
  "opponent left" alert, both keep playing offline to `--frames`) and `menu_fail` (a host with a
  different rollback window refuses with a reason, an unanswered join times out with an alert, all
  three keep playing offline, nobody restarts).
- **Mac <-> Windows** (`tools/test_netplay_wine.py`, in `run_tests.sh`; skipped with a message
  when Wine, `build/win/destrega.exe` + `SDL3.dll` or the data pack is missing): the native build
  and the Windows build under Wine (`WINE`, `WINE_ARGS`, `WIN_EXE`, `PAK`; default CrossOver's wine
  with `--bottle destrega-test --no-gui`) over loopback, started with the menu actions
  (`netplay-host` / `netplay-join`) in both directions: `mac_hosts`, `win_hosts`, and both again
  with a lossy link (`--net-sim 15:10:5:5`: 15 ms + 10 jitter, 5% loss, 5% reorder each way). Each
  plays the VS fight of a replay script, disconnects and continues offline; every confirmed
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
- Not done / not verified: NAT traversal or a server-assisted lobby (direct UDP only: the host
  forwards the port over the internet), spectators, Linux menus (the actions work there), a real
  Windows machine (only Wine), the Mac dialogs, panel and greyed-out items by hand (compiled; two
  windowed processes were driven through the same actions with `--menu-action`),
  windowed-vs-windowed play by two people.

## Application menus (`runtime/menu.h`, `menu_mac.m`, `menu_win.c`, `menu_stub.c`, `main.c`)

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
- **Netplay** menu (after View): Host Game…, Join Game…, Disconnect, status line; see "Rollback netplay".
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
  `widescreen[=on|off]`, `renderer[=software|gpu|compare]`, `fullscreen[=on|off]`,
  `netplay-host=PORT[,DELAY]`, `netplay-join=HOST:PORT[,DELAY]`, `netplay-disconnect`,
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
  mark, Renderer > Software / GPU, Enter / Exit Full Screen), **Netplay** (Host Game…, Join Game…,
  Disconnect, status line). The window procedure is subclassed: `WM_COMMAND` queues the action,
  `WM_INITMENUPOPUP` updates check marks, titles, the status line and greys out refused items
  (`frontend_menu_action_allowed`, as `validateMenuItem` on the Mac). SDL sizes the window by its
  client area and accounts for the menu bar, so the picture keeps its size; full screen covers the
  bar. Host / Join are a small modal form built in code (no resource script; values in
  `HKCU\Software\Destrega\Netplay`), the waiting panel a modeless window with Cancel (queues
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
  On macOS and Linux `menu-test` logs that it is not available.
- Not done: saves other than the game's own file, Linux menus, the Windows menus clicked by a
  person (only driven through `menu-test` under Wine), a real Windows machine.

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

A second `HiresBackend` on SDL3's SDL_GPU: **Metal** (MSL source embedded in the C file) or
**Vulkan** (SPIR-V, Windows/Linux, and macOS through MoltenVK). Same
contract as the software backend: commands in, target out, presentation only. It is
**bit-identical to the software backend** at every scale tested (1x1, 2x3, 4x6, 8x8 over a
7000-frame fight run) and to the 1x VRAM at 1x1 (with `--pgxp off`; for `on` see the PGXP
section). Keep it that way: a rule change in `hires.c`
needs the same change in **both** shaders (MSL in `hires_gpu.c`, GLSL in `runtime/shaders/hires.glsl`).
The header comment of `hires_gpu.c` has the details.

- **Shaders / drivers.** One backend, two shader sources selected by `SDL_GetGPUShaderFormats`:
  MSL (Metal) as before, and `runtime/shaders/hires.glsl` (Vulkan GLSL 450, the same functions
  line for line) compiled by `python3 tools/gen_spirv.py` (glslangValidator, `spirv-val`) into
  the checked-in `runtime/shaders/hires_spirv.h` (generated bytecode, regenerate after editing
  the GLSL; `--check` only compiles and validates, as CI does). No shader compiler is needed to
  build. Not done: DXIL/D3D12 (the device is created with MSL|SPIR-V only, so SDL never picks
  D3D12; a Windows machine without a Vulkan driver gets software).
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
- `--gpu-driver auto|metal|vulkan` (also for headless/compare): auto = Metal on macOS, Vulkan
  elsewhere. On macOS `vulkan` loads MoltenVK through the Vulkan loader; SDL only searches the
  default library paths, so `hires_gpu.c` sets `SDL_VULKAN_LIBRARY` to Homebrew's
  `libvulkan.1.dylib` (`brew install vulkan-loader molten-vk`) unless already set. The windowed
  GPU renderer is created on the backend's own device (`hires_gpu_create_device` +
  `SDL_CreateGPURenderer(dev, win)`), so its driver is the one the shaders exist for.
- Vulkan results (MoltenVK on Apple Silicon, 7000-frame fight, every 10th frame compared,
  `tools/test_vulkan.sh`): `--pgxp off` 0 differing pixels at 2x2 and 4x6, 4:3 and 16:9; `affine`
  0 at 4x6; `on` 24 (4:3) and 31 (16:9) pixels over 331 fight frames at 4x6 (Metal: 24 / 28; limit
  `CMP_MAX_PGXP`). `runtime/tests/test_hires_gpu.c` (random GP0 streams through gpu.c in compare
  mode: all polygon/rect/line kinds, semi modes 1-3, texture windows, dither, fills, uploads,
  copies, plus PGXP packets; 1x1/2x2/3x2/4x6, with and without the guard band): 0 differing pixels
  on Metal and on MoltenVK; it found a deliberately broken edge rule at every scale. Not run on a
  native Vulkan driver (NVIDIA/AMD/Intel) or D3D12: that, and the semi-transparency blend rounding
  on other hardware (UNORM5 blending precision is implementation-defined), are unverified.

- `--renderer software|gpu|compare`. Default: **gpu in a window** on every platform (if no device
  with MSL or SPIR-V shaders and a B5G5R5A1 render target exists, or the SDL GPU renderer or the
  backend cannot be created: `hires-gpu: GPU renderer unavailable ...` and software),
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
  `offscreen` driver has no GPU backend). Without a usable device it logs `GPU renderer
  unavailable` and uses software. Windowed it presents straight from the GPU texture (SDL GPU renderer +
  `SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`); readback only for `--dump`, `--hires-verify`
  and compare (`HiresBackend.read`).
- Exactness: target B5G5R5A1 (shader writes PS1 red into `.b` = the low 5 bits, so the download
  is raw PS1 u16, alpha = mask bit). Attributes are the 1x plane equations evaluated as exact
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
  emu 1.3 ms + present 6.6 ms per frame.
- Not implemented / approximate: mode 0 + mask check, mode 0 with F=0 over odd B (off by one;
  mode 0 unused by the game), VRAM->VRAM copies that overlap themselves inside the target are
  chunked by rows, `hires-uv:` sample counts are 0 on the GPU (no shader counters), no DXIL
  (D3D12), no `--hires-threads` (n/a).
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
