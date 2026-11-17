Destrega Redux 0.1.0-alpha (Linux, x86-64)

ALPHA RELEASE. This is an early test build for people comfortable with rough edges. It is not
user-friendly yet: things will break, messages can be technical, and settings or save formats may
change between versions. The Linux build has not been tested with the game yet (only in CI,
without a disc). Report problems at https://github.com/marinohardin/destrega-redux/issues

This package holds no game code or data. You need your own copy of the North American disc
(SLUS-00732) as a MODE2/2352 .cue/.bin image.

Needs: an x86-64 distribution with glibc, a C compiler (gcc or clang: the game is translated into
C and compiled on this computer the first time it starts, about a minute), zlib, and a Vulkan
driver for the GPU renderer (otherwise it falls back to software). SDL3 is included in lib/.

    ./destrega                       # asks for the disc image, then prepares the game once
    ./destrega --import Destrega.cue # or import the image from the command line

Game data, saves and settings live in ~/.local/share/Destrega/. Menus: F10, Alt, right-click or
move the mouse to the top of the window. Alt+Enter / F11 full screen, F9 widescreen.

Destrega Redux is free software under the GPL-3.0 (LICENSE). Third-party licences:
THIRD_PARTY_LICENSES.txt.
