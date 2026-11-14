# Destrega Redux 0.1.1-alpha

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
| `Destrega-0.1.1-alpha-macos-arm64.dmg` | macOS 13+ on Apple Silicon (signed and notarized): open it and drag Destrega to Applications |
| `Destrega-0.1.1-alpha-windows-x86_64.zip` | Windows 10/11, x86-64 (not code-signed: SmartScreen will warn) |
| `Destrega-0.1.1-alpha-linux-x86_64.tar.gz` | Linux x86-64 (needs gcc or clang; untested with the game) |

`SHA256SUMS.txt` holds the checksums.

## What's new in 0.1.1-alpha

This release finishes the native 3D renderer (View > 3D Scene). Everything else is unchanged from
0.1.0-alpha, so see its notes for the whole feature list. Netplay needs both players on the same
version: 0.1.1 and 0.1.0 cannot play each other.

**Native 3D renderer**
- **Fighter shadows** are drawn in 3D on the floor (View > 3D Scene > Shadows). They follow sloped
  and stepped floors and sit under the fighters' feet.
- **Effects** (fire, sparks, trails, dust) are drawn in the 3D scene with the stage and fighters
  (View > 3D Scene > Effects).
- **60 Hz motion in fights** (View > 3D Scene > 60 Hz Motion, on by default). Fights update 30
  times a second; an in-between picture is now shown for every other frame. It adds no input
  delay, and the game itself is unaffected.
- Story scenes use the native renderer too.

**Fixes**
- Fallen fighters no longer sink into sloped floors, and their shadow is no longer drawn on top of
  them. This was fixed for native shadows and for shadows drawn the original way.
- Shadows no longer flicker or lose pieces on sloped and stepped floors (Relics, Castle, Shrine).
- Torn fighters in the in-between frames during hits and knockdowns.
- Effects that the original draws behind a fighter were drawn over it.
- Settings carried over from 0.1.0 now turn on the 3D renderer parts added since: shadows and
  effects. You can still switch each one off in View > 3D Scene.

## Known limitations

- Tested mainly on macOS (Apple Silicon). Windows was tested under Wine/CrossOver only (the
  first-launch compile there is untested on a real Windows machine); Linux was only built in CI,
  never run with the game.
- Automatic port opening has only been tested against a simulated router.
- The downloads are large because they include a C compiler for the first-launch build.
- The Windows build is not code-signed.

Destrega Redux is free software under the GPL-3.0. *Destrega* is © Koei; this project is not
affiliated with or endorsed by Koei Tecmo.
