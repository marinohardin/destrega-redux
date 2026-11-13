# Destrega Redux 0.1.0-alpha

> **Alpha release: incomplete as a user experience.** The game itself is fully playable, but
> everything around it is still rough. Setting it up asks for technical steps, error messages can be
> technical, some menus and options are developer-facing, and there is no in-app help. Expect bugs.
> Settings, save states and netplay compatibility may change between versions (both netplay players
> need the same version). This build is for people comfortable with rough edges; please report
> problems in the issue tracker.

**No game included.** These downloads contain no game code or data. You need your own copy of
*Destrega* for the PlayStation, North American release (SLUS-00732), as a MODE2/2352 `.cue`/`.bin`
image. On first launch Destrega Redux reads your disc, translates the game's code and compiles it
on your computer (about a minute; the Mac and Windows packages bring their own compiler, which is
why they are large; Linux uses your gcc or clang).

## Downloads

| File | For |
|---|---|
| `Destrega-0.1.0-alpha-macos-arm64.dmg` | macOS 13+ on Apple Silicon (signed and notarized): open it and drag Destrega to Applications |
| `Destrega-0.1.0-alpha-windows-x86_64.zip` | Windows 10/11, x86-64 (not code-signed: SmartScreen will warn) |
| `Destrega-0.1.0-alpha-linux-x86_64.tar.gz` | Linux x86-64 (needs gcc or clang; untested with the game) |

`SHA256SUMS.txt` holds the checksums.

## What this is

Destrega Redux is a **static recompilation** of *Destrega* (Koei, 1998): the game's MIPS code is
translated function by function into C and compiled into a native program, running on a
re-implementation of the PlayStation's hardware. It is not an emulator running the original code
instruction by instruction; in normal play no PlayStation code is interpreted at all.

## Key work in this release

**The recompiled game**
- Recompiler for the whole game: the main program and every overlay (one per mode and scene), jump
  tables, delay slots, interrupt entry points; a MIPS interpreter only as a safety net.
- Native implementations of the PlayStation hardware the game uses: graphics chip, geometry
  coprocessor (GTE), sound chip with reverb, CD drive including streamed XA music and FMV movies,
  movie decoder, controllers, memory cards, timers, DMA and the BIOS calls the game makes.
- Every mode plays: story, 1P battle, VS, team battle, time attack, endurance, practice and the
  options, with saves on a standard memory-card image (`.mcd`, the same format emulators use).
- **Bring your own disc**: the first launch imports your disc image into a compressed data pack,
  checks it is the US release, and builds the game module for your computer. Load Game Image in the
  File menu replaces it later.

**Graphics**
- High-resolution rendering, up to 4x6 the original resolution (2048x1440 and more), on Metal,
  Vulkan or Direct3D 12, with a software renderer as fallback. The exact GPU renderer gives the same
  picture as the software one, pixel for pixel; a faster approximate mode is in View > Renderer.
- 16:9 widescreen fights: the stage is drawn wider without changing anything the game sees (proved
  frame by frame against a 4:3 run). Menus and movies stay 4:3.
- Sub-pixel vertex precision and perspective-correct textures (no wobbling or warping polygons),
  including faces the original drops at low resolution.
- Texture smoothing (smooth 2D edges, bilinear 3D), mipmapped distant textures, full-precision
  colour without banding, optional anti-aliasing.
- A native 3D renderer for stages and fighters (View > 3D Scene): the scene is redrawn with a depth
  buffer and edge smoothing instead of the PlayStation's polygon sorting, and fighter animation can
  be smoothed between the game's 30 updates per second.
- An automatic resolution governor lowers the render scale on computers that cannot keep up.

**Rollback netplay**
- Netplay > Host Game / Join Game: rollback netcode over the internet or a home network, Mac and
  Windows players together, input delay and a rollback window, ping and connection quality shown,
  desync detection with a saved report.
- Both players start from a fresh boot straight into VS character select, and rematches stay there.
- The host's router is asked to open the port automatically (PCP, NAT-PMP or UPnP).

**Around the game**
- Native menus on macOS and Windows (an in-window menu on Linux): game files, save games
  (import, export, delete), renderer, resolution, widescreen, full screen, controls, netplay.
- Rebindable keyboard and controller controls for both players, with button names that follow the
  controller (PlayStation, Xbox, Nintendo); settings remembered between runs.
- Complete save states at frame boundaries (used by rollback; not yet exposed as a menu feature).

**Testing behind it**
- Recorded replays and scripted scenarios check every frame of play for any change in gameplay;
  every renderer, resolution and widescreen setting is checked to leave the game's state untouched;
  netplay is tested between two Macs and between a Mac and Windows (under Wine), over clean and
  lossy links.

## Known limitations

- Tested mainly on macOS (Apple Silicon). Windows was tested under Wine/CrossOver only (the
  first-launch compile there is untested on a real Windows machine); Linux was only built in CI,
  never run with the game.
- Automatic port opening has only been tested against a simulated router.
- In the native 3D renderer, shadows, effects and story-scene characters still use the original
  2D path.
- The downloads are large because they include a C compiler for the first-launch build.
- The Windows build is not code-signed.

Destrega Redux is free software under the GPL-3.0. *Destrega* is © Koei; this project is not
affiliated with or endorsed by Koei Tecmo.
