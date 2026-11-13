# Destrega Redux 0.1.0-alpha

> **Alpha release.** This is an early test build for people comfortable with rough edges. It is
> **not user-friendly yet**: expect bugs, technical messages and missing polish, and expect
> settings, save states or netplay compatibility to change between versions (both netplay players
> need the same version). Please report problems in the issue tracker.

**No game included.** These downloads contain no game code or data. You need your own copy of
*Destrega* for the PlayStation, North American release (SLUS-00732), as a MODE2/2352 `.cue`/`.bin`
image. On first launch the game's code is translated from your disc and compiled on your computer
(about a minute; the Mac and Windows packages bring their own compiler, Linux uses your gcc or
clang).

## Downloads

| File | For |
|---|---|
| `Destrega-0.1.0-alpha-macos-arm64.dmg` | macOS 13+ on Apple Silicon (signed and notarized): open it and drag Destrega to Applications |
| `Destrega-0.1.0-alpha-windows-x86_64.zip` | Windows 10/11, x86-64 (not code-signed: SmartScreen will warn) |
| `Destrega-0.1.0-alpha-linux-x86_64.tar.gz` | Linux x86-64 (needs gcc or clang; untested with the game) |

`SHA256SUMS.txt` holds the checksums.

## What works

- Every mode: story, 1P battle, VS, team battle, time attack, endurance, practice, options;
  memory-card saves.
- High-resolution rendering (up to 4x6) on Metal, Vulkan or Direct3D 12, 16:9 widescreen fights,
  sub-pixel vertices and perspective-correct textures, texture smoothing, full-precision colour.
- The native 3D renderer for stages and fighters (depth buffer, edge smoothing; View > 3D Scene).
- Rollback netplay (Netplay > Host Game / Join Game), including Mac <-> Windows, with automatic
  router port opening.
- Rebindable keyboard and controller controls, full screen, settings that persist.

## Known limitations

- Tested mainly on macOS (Apple Silicon). Windows was tested under Wine/CrossOver only; Linux only
  built in CI, never run with the game.
- Automatic port opening has only been tested against a simulated router.
- Shadows, effects and story-scene characters still use the original 2D path of the 3D renderer.
- The Windows build is not code-signed.

Destrega Redux is free software under the GPL-3.0. *Destrega* is © Koei; this project is not
affiliated with or endorsed by Koei Tecmo.
