# Releases and distribution

## The constraint

`gen/*.c` is a translation of the game's executable (`SLUS_007.32`), produced by
`recomp/recomp.py` from the user's own disc. It is derived from copyrighted code, so a binary
that contains it is too. CI has no disc, and must not have one. Therefore:

- no workflow, release page or artifact may contain a normal (playable) build;
- the repository, CI logs and artifacts never contain `gen/`, `disc/` or any game data;
- everything the project publishes is the runtime, the recompiler, tools and docs.

Today a playable build only exists on a machine that has the disc: the user builds it
(`README.md`), and may package it for their own machines with `tools/package_release.sh --personal`.

## Options considered

| | Idea | Verdict |
|---|---|---|
| **A** | Release = runtime + recompiler; first launch generates and compiles the game code from the user's disc | Legally clean, but needs a C compiler on every player's machine (Xcode tools, MinGW/clang). Unrealistic unless a toolchain is bundled (a Zig or clang distribution, 50-150 MB per OS) and first launch takes minutes. |
| **B** | User builds locally; CI builds only the stub to prove everything compiles and packages | Clean, works today. Cost: players must build. |
| **C** | Runtime loads/translates the game code from the disc at run time (an interpreter or dynarec for the MIPS executable, or generated code kept as data) | The only design that gives a true "download, point at your disc, play" release. Big redesign: the project is deliberately native C, netplay determinism depends on it, and the MIPS interpreter is being removed. |

**Status:** B is implemented (below), and so is the bundled-toolchain variant of **A** (next
section): a downloadable package with no game code that builds the game on the player's
machine. C stays a separate project.

## Option A: the launcher (`-DDESTREGA_GAME_MODULE=ON`)

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
  `Destrega-<version>-<os>-<arch>.zip` (Linux: an AppImage, `tools/make_appimage.sh`, needs
  `appimagetool` installed by hand). Every package is checked by `tools/check_no_game_code.py`
  (no generated-code files, no recompiled-function names beyond the interface, no bytes of the
  game executable) and refused if anything is found.
- Determinism: a module build gives the same per-frame `--state-hash` as the static build, and
  the netplay identity (recompiled-code table + emulation sources) is the same, so launcher and
  static builds of one commit play together.

### What remains before a launcher release

- A licence for the repository (none yet) and a licence review of the bundled Zig (MIT; it
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
  Needs a real Windows machine (or a Windows CI job that runs the bundled `zig cc`); the zip with Zig is large (~100 MB download, Zig's Windows release alone).
- Linux: build and run the AppImage on Linux (not done: no Linux machine with the game here).
- Size: Zig's `lib/` holds libc headers and sources for every target (~400 MB unpacked on macOS);
  pruning it to the host target would roughly halve the download.
- First-launch UX by a person: the Cocoa, Win32 and SDL progress windows were only exercised by
  scripted runs.

## What is implemented

- `tools/package_release.sh mac|win` packages a finished build on the machine that built it.
  A game build needs `--personal` and is named `...-PERSONAL-DO-NOT-REDISTRIBUTE.zip`.
  A stub build (detected by a marker string in `tools/ci/stub_gen/stub.c`) is named
  `Destrega-STUB-NOT-PLAYABLE-...zip` and contains a `NOT-PLAYABLE.txt`.
- CI (`.github/workflows/build.yml`) builds and packages the **stub** on Windows and macOS and
  uploads `STUB-NOT-PLAYABLE-windows` / `-macos` (3 days) so packaging is exercised on every push.
  Those zips cannot run the game.
- `THIRD_PARTY_LICENSES.txt` (SDL3 and zlib: zlib licence; MinGW runtime notes) is in every package
  and inside `Destrega.app/Contents/Resources`.
- Versions come from `VERSION` plus git (`tools/version.py`): Info.plist (short version = base,
  `CFBundleVersion` = commit count, `DestregaBuild` = full string) and the Windows version resource.
- The repository has **no licence file yet**. The owner has to pick one before anything is
  published as a release.

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
