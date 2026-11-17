#!/bin/bash
# Build the macOS game executable for a release package: arm64, runs on macOS 13 and later.
#
#   tools/build_mac_release.sh [--min-os 13.0]
#
# Why not build/cmake: Homebrew's SDL3 and a build without a deployment target are made for the
# macOS of the build machine (minos = that version; 0.1.0 and 0.1.1 shipped DMGs that needed
# macOS 27 while their notes said 13+). Here SDL3 is built from source with the same deployment
# target as the game.
#
# Output (all under build/, gitignored):
#   build/mac-deps/sdl3/lib/libSDL3.0.dylib   SDL3 (version from .github/workflows/build.yml);
#                                             its install name is its absolute path, so
#                                             tools/make_app.sh bundles it like a Homebrew dylib
#   build/rel-mac/destrega                    RelWithDebInfo (the configuration the tests use)
#   build/rel-mac/Destrega.app                ad-hoc signed, for a quick try
# Then: tools/package_release.sh release mac (its default --bin is build/rel-mac/destrega).
# The SDL3 source tarball is taken from build/mac-deps/ or build/linux-deps/ when present,
# otherwise downloaded once from SDL's GitHub releases.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MINOS=13.0
while [ $# -gt 0 ]; do
    case "$1" in
        --min-os) MINOS="$2"; shift 2 ;;
        -h|--help) sed -n 2,19p "$0"; exit 0 ;;
        *) echo "build_mac_release: unknown option $1" >&2; exit 2 ;;
    esac
done
[ "$(uname -s)" = Darwin ] || { echo "build_mac_release: needs macOS" >&2; exit 1; }
SDL_VERSION="$(sed -n 's/^ *SDL_VERSION: *//p' "$ROOT/.github/workflows/build.yml" | head -1)"
[ -n "$SDL_VERSION" ] || { echo "build_mac_release: no SDL_VERSION in build.yml" >&2; exit 1; }
D="$ROOT/build/mac-deps"
LIB="$D/sdl3/lib/libSDL3.0.dylib"
TAR="SDL3-$SDL_VERSION.tar.gz"
JOBS="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo 4)"
mkdir -p "$D"

minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'; }

if [ ! -f "$LIB" ] || [ "$(minos_of "$LIB")" != "$MINOS" ] || [ ! -f "$D/sdl3/.version-$SDL_VERSION" ]; then
    cd "$D"
    if [ ! -f "$TAR" ]; then
        if [ -f "$ROOT/build/linux-deps/$TAR" ]; then cp "$ROOT/build/linux-deps/$TAR" .
        else curl -fsSL --retry 5 -o "$TAR" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/$TAR"; fi
    fi
    rm -rf "SDL3-$SDL_VERSION" sdlbuild sdl3 && tar xzf "$TAR"
    nice -n 10 cmake -S "SDL3-$SDL_VERSION" -B sdlbuild -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINOS" -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_INSTALL_PREFIX="$D/sdl3" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > /dev/null
    nice -n 10 cmake --build sdlbuild -j"$JOBS" > /dev/null
    cmake --install sdlbuild > /dev/null
    rm -rf "SDL3-$SDL_VERSION" sdlbuild
    # absolute install name: make_app.sh copies absolute-path dylibs into the bundle
    install_name_tool -id "$LIB" "$LIB" 2>/dev/null
    codesign -f -s - "$LIB" 2>/dev/null
    touch "$D/sdl3/.version-$SDL_VERSION"
    cd "$ROOT"
fi

B="$ROOT/build/rel-mac"
nice -n 10 cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINOS" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DSDL3_DIR="$D/sdl3/lib/cmake/SDL3" -DCMAKE_PREFIX_PATH="$D/sdl3" -DDESTREGA_APP_VERSION= > /dev/null
nice -n 10 cmake --build "$B" -j"$JOBS"

for f in "$B/destrega" "$B/Destrega.app/Contents/Frameworks/"*.dylib; do
    m="$(minos_of "$f")"
    [ "$m" = "$MINOS" ] || { echo "build_mac_release: $f needs macOS $m, not $MINOS" >&2; exit 1; }
done
echo "build_mac_release: $B/destrega (macOS $MINOS+, $(python3 "$ROOT/tools/version.py" describe))"
