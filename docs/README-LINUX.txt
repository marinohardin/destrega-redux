Destrega Redux @VERSION@ (Linux, x86-64)

ALPHA RELEASE. This is an early test build for people comfortable with rough edges. It is not
user-friendly yet: things will break, messages can be technical, and settings or save formats may
change between versions. The Linux build has not been tested on a Linux desktop by a person yet.
Report problems at https://github.com/marinohardin/destrega-redux/issues

No game data is included. The program contains code translated from the game's executable and
none of the game's data. You need your own copy of the North American disc (SLUS-00732) as a
MODE2/2352 .cue/.bin image.

Needs: an x86-64 distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or
later), zlib, and a Vulkan driver for the GPU renderer (otherwise it falls back to software).
SDL3 is included in lib/.

    ./destrega                       # asks for the disc image, imports it, then plays
    ./destrega --import Destrega.cue # or import the image from the command line

Game data, saves and settings live in ~/.local/share/Destrega/. Menus: F10, Alt, right-click or
move the mouse to the top of the window. Alt+Enter / F11 full screen, F9 widescreen.

Destrega Redux is an unofficial fan project, not affiliated with, endorsed by or sponsored by
Koei Tecmo or Sony Interactive Entertainment. Legal notice: see the README on the project page.

Destrega Redux is free software under the GPL-3.0 (LICENSE). Third-party licences:
THIRD_PARTY_LICENSES.txt.
