# Destrega — static recompilation (PS1 → native C)

The PlayStation game *Destrega* (Koei, 1998) recompiled from MIPS R3000 machine
code into C, with a native runtime emulating the PS1 hardware (GPU software
renderer, GTE, SPU + reverb, XA audio, MDEC FMV, CD-ROM, pads). Analysis is
done with [REA](https://github.com/morluto/rea) (Ghidra provider).

You need your own copy of the game: the North American release (SLUS-00732)
as a MODE2/2352 `.cue`/`.bin`. Nothing from the game is in this repository.

## Requirements (all platforms)

- CMake 3.20+ and Ninja (or another CMake generator)
- A C11 compiler: **Clang or GCC** (MSVC's `cl` is not supported; on Windows use MinGW/MSYS2 or `clang-cl`)
- Python 3 (runs the recompiler during the build)
- SDL3 and zlib development packages

## Build

The build turns the game's executable into C, so it needs the extracted disc
files in `disc/` once:

```sh
python3 tools/extract_disc.py /path/to/Destrega.bin disc
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
```

### macOS

```sh
brew install sdl3 cmake ninja python
```

Then run the build steps above. On macOS the build also produces
`build/cmake/Destrega.app`: a self-contained app (SDL3 bundled, with an icon)
that you can drag to `/Applications` and launch from Finder. The plain
`./build/cmake/destrega` binary is still there for the command-line options.

The app is ad-hoc signed, which is enough to run it on the Mac that built it.
To give it to someone else without Gatekeeper warnings, sign it with a
Developer ID and notarize it:

```sh
cmake -S . -B build/cmake -DDESTREGA_APP_SIGN="Developer ID Application: Your Name (TEAMID)"
cmake --build build/cmake
tools/make_app.sh --help    # bundling options, including --dmg
```

Turn the bundle off with `-DDESTREGA_MACOS_APP=OFF`. The icon
(`assets/icon/destrega.png`) is original artwork drawn by `tools/make_icon.py`;
it uses no game assets.

### Linux

Install the dependencies for your distribution:

```sh
# Ubuntu 25.04+ / Debian 13+
sudo apt install build-essential cmake ninja-build python3 libsdl3-dev zlib1g-dev
# Fedora 42+
sudo dnf install gcc cmake ninja-build python3 SDL3-devel zlib-devel
# Arch
sudo pacman -S base-devel cmake ninja python sdl3 zlib
```

If your distribution doesn't package SDL3 yet, build it from
[source](https://github.com/libsdl-org/SDL) and point CMake at it with
`-DCMAKE_PREFIX_PATH=/path/to/sdl3/install`.

Then run the build steps above and start the game with `./build/cmake/destrega`.

### Windows

Use [MSYS2](https://www.msys2.org/) and its **UCRT64** shell:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-zlib
```

Then run the build steps above (from the UCRT64 shell) and start the game with
`./build/cmake/destrega.exe`. To launch it outside the MSYS2 shell, copy
`SDL3.dll`, `zlib1.dll` and the GCC runtime DLLs from `C:\msys64\ucrt64\bin`
next to `destrega.exe`.

## First launch

The game asks for your disc image (`.cue` or `.bin`), checks it is the
supported version, and converts it once into a private data pack (~330 MB).
After that it runs from the pack, and the original image can be deleted or
moved. The pack is stored in your per-user data folder:

| OS | Data pack location |
|---|---|
| macOS | `~/Library/Application Support/Destrega/destrega.pak` |
| Linux | `~/.local/share/Destrega/destrega.pak` |
| Windows | `%APPDATA%\Destrega\destrega.pak` |

Command-line import: `destrega --import /path/to/Destrega.cue`. A different
location can be chosen with `--data-dir DIR`. To recreate `disc/` later
without the original image, pass the pack to the extractor instead:
`python3 tools/extract_disc.py <data pack path> disc`.

## Controls

| Keyboard | PS1 button | In a fight |
|---|---|---|
| Arrow keys | D-pad | Move |
| A | Square | Quick attack |
| S | Triangle | Strong attack |
| X | Circle | Sidestep |
| Z | Cross | Jump / confirm in menus |
| Q / W | L1 / R1 | Guard / dash |
| E / R | L2 / R2 | |
| Enter | Start | Pause / confirm |
| Backspace | Select | |

Tab = fast-forward, F12 = screenshot, Esc = quit. Gamepads work (the second
pad is player 2). Step far back until the charge bar turns blue to use magic.

## Status

Boots, plays the intro FMV with audio, title, menus, all modes reachable,
fights are playable (60 Hz output; fights update at 30 fps, as on the
original). Not yet: memory-card saves (the card is reported as absent), PAL,
GPU texture-cache quirks. Developed and tested on macOS (Apple Silicon); the
Linux and Windows builds use the same portable C11 + SDL3 code but have not
been tested yet. See `AGENTS.md` for internals and `tools/run_tests.sh` for
the test suite.
