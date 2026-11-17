#!/bin/bash
# Build the Linux x86-64 game executable for a release package, in an Ubuntu 22.04 container.
#
#   tools/build_linux_docker.sh [--shell]
#
# Needs Docker (on Apple Silicon the amd64 container runs under emulation: slow, ~20-40 min the
# first time) and gen/ from a normal build (the recompiler output; it never leaves this machine
# except compiled into the executable). Ubuntu 22.04 = glibc 2.35, so the executable runs on
# Ubuntu 22.04, Debian 12, Fedora 36 and later.
#
# Output (all under build/, gitignored):
#   build/linux/destrega            RUNPATH $ORIGIN/lib, ready for
#                                   tools/package_release.sh release linux --bin ... --lib ...
#   build/linux-deps/sdl3/lib/libSDL3.so.0
# SDL3 (version from .github/workflows/build.yml) is downloaded once into build/linux-deps/ and
# built there; the apt packages come from Ubuntu's mirrors on each run (the container is
# thrown away). --shell opens a shell in the container instead of building.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDL_VERSION="$(sed -n 's/^ *SDL_VERSION: *//p' "$ROOT/.github/workflows/build.yml" | head -1)"
[ -n "$SDL_VERSION" ] || { echo "build_linux_docker: no SDL_VERSION in build.yml" >&2; exit 1; }
[ -f "$ROOT/gen/files.cmake" ] || { echo "build_linux_docker: no gen/ (run a normal build first)" >&2; exit 1; }
mkdir -p "$ROOT/build/linux-deps" "$ROOT/build/linux"
MODE=build
[ "${1:-}" = --shell ] && MODE=shell

docker run --rm -i $([ "$MODE" = shell ] && echo -t) --platform linux/amd64 \
    -v "$ROOT:/src" -w /src -e SDL_VERSION="$SDL_VERSION" -e MODE="$MODE" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    ubuntu:22.04 bash -s <<'SH'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl cmake ninja-build clang \
    python3 git patchelf zlib1g-dev pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
    libxtst-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libasound2-dev \
    libpulse-dev libvulkan-dev > /dev/null
git config --global --add safe.directory /src
if [ "$MODE" = shell ]; then exec bash; fi
D=/src/build/linux-deps
if [ ! -f $D/sdl3/lib/libSDL3.so.0 ]; then
    cd $D
    [ -f SDL3-$SDL_VERSION.tar.gz ] || curl -fsSL --retry 5 -o SDL3-$SDL_VERSION.tar.gz \
        https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz
    rm -rf SDL3-$SDL_VERSION sdlbuild && tar xzf SDL3-$SDL_VERSION.tar.gz
    cmake -S SDL3-$SDL_VERSION -B sdlbuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_INSTALL_PREFIX=$D/sdl3 -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > /dev/null
    cmake --build sdlbuild > /dev/null
    cmake --install sdlbuild > /dev/null
    rm -rf SDL3-$SDL_VERSION sdlbuild
    cd /src
fi
cmake -S /src -B /src/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DCMAKE_PREFIX_PATH=$D/sdl3
cmake --build /src/build/linux --target destrega
patchelf --set-rpath '$ORIGIN/lib' /src/build/linux/destrega
ldd /src/build/linux/destrega | grep -E 'SDL3|not found' || true
/src/build/linux/destrega --version 2>&1 | head -2 || true
chown -R "$HOST_UID:$HOST_GID" /src/build/linux /src/build/linux-deps /src/gen
SH
echo "build_linux_docker: build/linux/destrega"
