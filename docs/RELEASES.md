# Releases and distribution

## The policy (owner decision 2026-11-16, from 0.1.2-alpha on)

A release is the program as built on a machine that has the disc: the runtime plus `gen/*.c`
(the code `recomp/recomp.py` translated from the game's executable, `SLUS_007.32`), compiled for
each platform. This follows the model of other static-recompilation ports such as
[Takedawn](https://github.com/xenophobentx/takedawn): the download contains the game's
translated code, which is a grey area the project accepts, and **never any game data**.

- **Never in a release, the repository, CI logs or artifacts:** disc images, the data pack,
  any file from the disc (stages, characters, textures, models, sound, music, films, overlays),
  memory-card images, or anything decoded from them (pictures, shader caches, dumps). The
  README screenshots in `docs/screenshots/` are the one exception, in the repository only.
- **Never in the repository, CI logs or artifacts:** `gen/` (the translated C). It exists only on
  the release machine and reaches users only compiled into the executable.
- CI has no disc and must never get one; it keeps building the stub and the launcher.
- Every release package is checked by `tools/check_no_game_data.py` against the extracted disc
  (`package_release.sh release` runs it and refuses to package otherwise).
- The README's notices ("Destrega Redux is an unofficial, non-commercial fan project...", the
  FAQ "Does Destrega Redux include the game?", "How it works" and the Legal notice) say plainly
  what the download contains. Keep them true when a release changes.

Until 0.1.1 the policy was stricter (no game code in any published file); 0.1.0 and 0.1.1 shipped
the launcher (option A below) for that reason.

## Options considered

| | Idea | Verdict |
|---|---|---|
| **A** | Release = runtime + recompiler; first launch generates and compiles the game code from the user's disc | Kept no game code in the download, but bundles a C compiler (Zig, ~50-150 MB per OS) and takes minutes on first launch; the Windows first-launch build was never run on real Windows. Shipped in 0.1.0 / 0.1.1; the code is kept. |
| **B** | User builds locally; CI builds only the stub to prove everything compiles and packages | Works; players must build. Still how development works. |
| **C** | Runtime loads/translates the game code from the disc at run time (an interpreter or dynarec) | Big redesign: the project is deliberately native C, netplay determinism depends on it. Not planned. |
| **D** | Release = the prebuilt game build (translated code compiled in), no game data; first launch imports the disc | **The release form since 0.1.2.** Small download, no compiler, starts at once, everyone runs the tested binary. Built on the release machine for all three platforms. |

## Release packages (`tools/package_release.sh release mac|win|linux`)

Built from the merge commit with the release tag checked out (the version comes from
`tools/version.py describe`, so tag first, locally at least):

1. `cmake --build build/cmake` (macOS app), `cmake --build build/win` (Windows cross build, see
   AGENTS.md), `tools/build_linux_docker.sh` (Linux x86-64 in an Ubuntu 22.04 container, glibc
   2.35; SDL3 built once into `build/linux-deps/`).
2. `tools/package_release.sh release mac --sign "Developer ID Application: ..." --notarize PROFILE`
   -> `Destrega-<ver>-macos-arm64.dmg` (signed, notarized, stapled; no extra entitlements).
3. `tools/package_release.sh release win --dll-dir build/win-deps/prefix/bin`
   -> `Destrega-<ver>-windows-x86_64.zip` (exe + SDL3.dll; MinGW links the rest statically).
4. `tools/package_release.sh release linux --bin build/linux/destrega --lib
   build/linux-deps/sdl3/lib/libSDL3.so.0` -> `Destrega-<ver>-linux-x86_64.tar.gz` (RUNPATH
   `$ORIGIN/lib`, README from `docs/README-LINUX.txt`).
5. Each run refuses a stub or launcher binary and a binary with too few recompiled functions,
   checks the staged files (and the Windows zip / Linux tarball) with `check_no_game_data.py`
   and writes a `.sha256`. Then `SHA256SUMS.txt` and `gh release create`.

`check_no_game_data.py` fails on game-data file names (`*.pak`, `*.bin`, `*.cue`, `*.iso`,
`*.mcd`, `*.BNS`, `*.XA`, `SLUS_*`, `SYSTEM.CNF`, anything under `disc/`, every name in the disc
listing) and on any of ~3300 64-byte windows sampled from every file of the extracted disc
appearing in a packaged file (whole files and large pieces; it is a sample, not a proof). The
executable contains no disc bytes: it holds the code compiled for the host, and the runtime loads
the game's own executable and data from the player's data pack.

## Option A: the launcher (`-DDESTREGA_GAME_MODULE=ON`; 0.1.0 / 0.1.1, kept)

A launcher build is the runtime alone; the game code is a **game module** (a shared library) that
the launcher builds on first launch from the player's own disc and loads at start-up. AGENTS.md
"Launcher and game module" has the details; in short:

- Configure with `-DDESTREGA_GAME_MODULE=ON -DDESTREGA_DEV_GAME_MODULE=OFF` (no disc, no `gen/`
  needed: CI builds it). With `DEV_GAME_MODULE=ON` (default in that layout) CMake also builds
  `destrega_game.<dylib|dll|so>` from `gen/` next to the executable, for development.
- First launch: import the disc (the existing flow), then a window "Preparing the game for this
  computer..." with a progress bar and Cancel: the C port of the recompiler
  (`runtime/launcher/recompiler.c`, byte-identical output to `recomp/recomp.py`,
  `tools/test_recomp_c.sh`) writes the C files, the bundled Zig (`zig cc`; a system clang/gcc with
  `--game-cc`) compiles them in parallel and links `<data dir>/game/destrega_game.<ext>`. Later
  launches load it directly; a changed key (recompiler, analysis inputs, module SDK, compiler,
  flags, target, the disc's game files) rebuilds it. Errors show a plain message with "Copy
  Details" (the build log's tail).
- The module interface is explicit and versioned (`runtime/game_abi.h`): a module built for
  other headers, another ABI version or another target is refused with a plain message.
- Packages: `tools/package_release.sh launcher mac|win|linux --zig DIR` ->
  `Destrega-<version>-<os>-<arch>.zip` (macOS: a signed, notarized `.dmg` with an Applications
  shortcut, `--sign IDENTITY --notarize PROFILE`; Linux: an AppImage, `tools/make_appimage.sh`, needs
  `appimagetool` installed by hand). Every package is checked by `tools/check_no_game_code.py`
  (no generated-code files, no recompiled-function names beyond the interface, no bytes of the
  game executable) and refused if anything is found.
- Determinism: a module build gives the same per-frame `--state-hash` as the static build, and
  the netplay identity (recompiled-code table + emulation sources) is the same, so launcher and
  static builds of one commit play together.

### What remained open for the launcher (not pursued since 0.1.2)

- A licence review of the bundled Zig (MIT; it
  contains LLVM and C library headers/sources under their own terms, `THIRD_PARTY_LICENSES.txt`).
- macOS: Developer ID signing and notarization of the app **including** the bundled Zig
  (`make_app.sh` signs `Resources/zig/zig` separately; notarization may want it under
  `Contents/Helpers` or `MacOS`). The hardened runtime needs `tools/launcher.entitlements`
  (`com.apple.security.cs.disable-library-validation`), since the game module is signed only by
  the player's linker. Not tried with a real Developer ID.
- Windows: run the launcher on a real Windows machine, Authenticode signing. The package
  (`package_release.sh launcher win`, Zig 0.17.1 x86_64-windows, 106 MB zip, no game code) was run
  under CrossOver 26 (`--bottle destrega-test`): the launcher starts, finds the bundled Zig, checks
  it and recompiles (1 s), but every `zig cc` fails with `checking cache failed: manifest_create
  Unexpected`, also for a one-line file on the bottle's own C: drive. Wine trace: Zig locks its
  cache manifest with `NtLockFile`, which Wine logs as `fixme:file:NtLockFile Unimplemented yet
  parameter` and fails. A Wine limitation (real Windows implements it), so the Windows first-launch
  build cannot be tested under Wine; the launcher reports the failure cleanly (exit 1, no crash).
  CI's Windows job now runs the bundled Zig on a real Windows runner (`--game-selftest`: compiles,
  links and loads a module from the stub sources, no game code), so the compiler, the DLL and its
  loading are covered; the real 50-unit game build still needs a Windows machine with the disc.
  The zip with Zig is large (~100 MB download, Zig's Windows release alone).
- Linux: build and run the AppImage on Linux (not done: no Linux machine with the game here).
- Size: Zig's `lib/` holds libc headers and sources for every target (~400 MB unpacked on macOS);
  pruning it to the host target would roughly halve the download.
- First-launch UX by a person: the Cocoa, Win32 and SDL progress windows were only exercised by
  scripted runs.

## What is implemented

- `tools/package_release.sh release mac|win|linux`: the published packages (above).
- `tools/package_release.sh mac|win` zips a finished build without the release checks, for your
  own machines: a game build needs `--personal` and is named `...-PERSONAL-DO-NOT-REDISTRIBUTE.zip`.
  A stub build (detected by a marker string in `tools/ci/stub_gen/stub.c`) is named
  `Destrega-STUB-NOT-PLAYABLE-...zip` and contains a `NOT-PLAYABLE.txt`.
- CI (`.github/workflows/build.yml`) builds and packages the **stub** on Windows and macOS and
  uploads `STUB-NOT-PLAYABLE-windows` / `-macos` (3 days) so packaging is exercised on every push.
  Those zips cannot run the game.
- `THIRD_PARTY_LICENSES.txt` (SDL3 and zlib: zlib licence; MinGW runtime notes) is in every package
  and inside `Destrega.app/Contents/Resources`.
- Versions come from `VERSION` plus git (`tools/version.py`): Info.plist (short version = base,
  `CFBundleVersion` = commit count, `DestregaBuild` = full string) and the Windows version resource.
- The repository is licensed under the GPL-3.0 (`LICENSE`, owner decision 2026-11-12).

## macOS signing and notarization (needs an Apple Developer account; not done here)

`make_app.sh` adds `--options runtime` (hardened runtime) to the app signature whenever the
identity is not ad-hoc. A launcher build then needs `--entitlements tools/launcher.entitlements`
(`package_release.sh launcher mac` passes it).

Ad-hoc signing (default) runs on the Mac that built it. Others get Gatekeeper warnings.

1. Join the Apple Developer Program, create a "Developer ID Application" certificate, install it
   in the login keychain.
2. Build signed: `cmake -S . -B build/cmake -DDESTREGA_APP_SIGN="Developer ID Application: Name (TEAMID)"`.
   Notarization also needs the hardened runtime (`make_app.sh` adds it for a non-ad-hoc
   identity); test that the bundled SDL3 still loads (it is signed with the same identity).
3. `ditto -c -k --keepParent build/cmake/Destrega.app Destrega.zip`
4. `xcrun notarytool store-credentials destrega --apple-id ... --team-id ... --password <app-specific>`
   then `xcrun notarytool submit Destrega.zip --keychain-profile destrega --wait`
5. `xcrun stapler staple build/cmake/Destrega.app`, then `tools/package_release.sh mac --personal`.

`LSMinimumSystemVersion` is taken from the build's deployment target (`minos` of the binary);
set `MACOSX_DEPLOYMENT_TARGET` when configuring if the build is meant to run on older systems.

## Windows

The exe embeds an icon, version info and a manifest with UTF-8 code page, long paths and
Common Controls 6 (`runtime/win/`). It is not Authenticode-signed; SmartScreen warns on
downloaded copies. `runtime/win/destrega.ico` is generated by `tools/make_ico.py` from
`assets/icon/destrega.png` (original procedural art).
