# Destrega Redux 0.1.2-alpha

> **Alpha release: incomplete as a user experience.** The game itself is fully playable, but
> everything around it is still rough. Error messages can be technical, some menus and options are
> developer-facing, and there is no in-app help. Expect bugs. Settings, save states and netplay
> compatibility may change between versions (both netplay players need compatible versions). This
> build is for people comfortable with rough edges; please report problems in the issue tracker.

> [!IMPORTANT]
> Destrega Redux is an unofficial, non-commercial fan project. It is not affiliated with,
> endorsed by or sponsored by Koei Tecmo or Sony Interactive Entertainment. All trademarks
> belong to their owners.
>
> No game data is included or distributed: no disc images, no textures, models, sound, music or
> films, and no memory-card saves. To play you need your own, legally obtained disc of the
> supported retail release for the PlayStation: North America (NTSC-U, SLUS-00732), as a
> MODE2/2352 `.cue`/`.bin` image. Destrega Redux checks a copy of it and takes the game data from
> there. Please do not ask for game files or disc images, and do not share them.

The downloads contain the program, which holds code translated from the game's executable, and no
game data. See the legal notice in the README.

## Downloads

| File | For |
|---|---|
| `Destrega-0.1.2-alpha-macos-arm64.dmg` | macOS 13+ on Apple Silicon (signed and notarized): open it and drag Destrega to Applications |
| `Destrega-0.1.2-alpha-windows-x86_64.zip` | Windows 10/11, x86-64 (not code-signed: SmartScreen will warn) |
| `Destrega-0.1.2-alpha-linux-x86_64.tar.gz` | Linux x86-64 with glibc 2.35+ (Ubuntu 22.04, Debian 12, Fedora 36 or later) |

`SHA256SUMS.txt` holds the checksums.

## What's new in 0.1.2-alpha

**Start playing right away.** The downloads now contain the finished program. 0.1.0 and 0.1.1
translated and compiled the game on your computer on first launch, with a bundled C compiler.
Now the first launch only asks for your disc image, checks it and imports it (as before), and the
game starts. There's no "Preparing the game for this computer..." step, and no compiler is needed,
on Linux too.

- Smaller downloads (no bundled compiler).
- Everyone runs the same build that was tested, on every platform.
- Your saves, settings and imported disc stay where they were. An existing installation of 0.1.1
  keeps working; the game module 0.1.1 built in its data folder (`game/`) is no longer used and
  can be deleted.

The game, the renderer and netplay are unchanged from 0.1.1-alpha; see its notes and 0.1.0's for
the feature list. 0.1.2 and 0.1.1 run the same emulation and can play each other online.

## Known limitations

- Tested mainly on macOS (Apple Silicon). Windows was tested under Wine/CrossOver only, not on a
  real Windows machine. The Linux build is new: it was built in an Ubuntu 22.04 container and
  checked there, but not played on a Linux desktop.
- Automatic port opening has only been tested against a simulated router.
- The Windows build is not code-signed.

Destrega Redux is free software under the GPL-3.0. The software is provided "as is", without
warranty of any kind.
