#!/bin/bash
# Build a Linux AppImage of a launcher build (no game code; docs/RELEASES.md option A).
#
#   tools/make_appimage.sh --zig DIR [--bin PATH] [--out FILE.AppImage] [--appdir DIR]
#
#   --bin     launcher executable (default build/launcher/destrega; configure with
#             -DDESTREGA_GAME_MODULE=ON -DDESTREGA_DEV_GAME_MODULE=OFF)
#   --zig     unpacked Zig release for this architecture (zig + lib/); goes to usr/bin/zig/,
#             where the launcher looks for it (<exe dir>/zig/zig)
#   --out     the AppImage (default dist/Destrega-<version>-linux-<arch>.AppImage)
#   --appdir  keep the AppDir there instead of a temporary folder
#
# Needs `appimagetool` on PATH (not downloaded by this script; get it from the AppImage project
# and verify it yourself). Shared libraries the executable links that are not part of a normal
# desktop system (SDL3; glibc, libm, libdl, libpthread, libz, X11/Wayland/GL/Vulkan/audio stay on
# the system, as the AppImage guidelines ask) are copied to usr/lib and found through the
# executable's RUNPATH ($ORIGIN/../lib, set with patchelf when available, else LD_LIBRARY_PATH in
# AppRun). The AppDir is checked with tools/check_no_game_code.py before it is packed.
# The game module is built into the data directory (~/.local/share/Destrega/game/), never into
# the read-only AppImage.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/launcher/destrega"; ZIGDIR=""; OUTF=""; APPDIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --zig) ZIGDIR="$2"; shift 2 ;;
        --out) OUTF="$2"; shift 2 ;;
        --appdir) APPDIR="$2"; shift 2 ;;
        -h|--help) sed -n 2,21p "$0"; exit 0 ;;
        *) echo "make_appimage: unknown option: $1" >&2; exit 1 ;;
    esac
done
die() { echo "make_appimage: $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "Linux only (ldd, appimagetool)"
[ -x "$BIN" ] || die "no executable at $BIN"
grep -aq destrega_game_module_init "$BIN" || die "$BIN is not a launcher build"
[ -n "$ZIGDIR" ] && [ -x "$ZIGDIR/zig" ] || die "--zig DIR must hold the zig executable"
command -v appimagetool >/dev/null || die "appimagetool not found on PATH (install it yourself)"
ARCH="$(uname -m)"
VER="$(python3 "$ROOT/tools/version.py" describe | tr + -)"
[ -n "$OUTF" ] || OUTF="$ROOT/dist/Destrega-$VER-linux-$ARCH.AppImage"
mkdir -p "$(dirname "$OUTF")"

if [ -z "$APPDIR" ]; then
    TMPD="$(mktemp -d "${TMPDIR:-/tmp}/destrega-appimage.XXXXXX")"
    trap 'rm -rf "$TMPD"' EXIT
    APPDIR="$TMPD/Destrega.AppDir"
fi
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$BIN" "$APPDIR/usr/bin/destrega"
cp -R "$ZIGDIR" "$APPDIR/usr/bin/zig"
cp "$ROOT/THIRD_PARTY_LICENSES.txt" "$APPDIR/"

# libraries to bundle: everything ldd resolves except the base system
skip='^(linux-vdso|ld-linux|libc\.|libm\.|libdl\.|libpthread\.|librt\.|libz\.|libgcc_s|libstdc\+\+|libX|libxcb|libwayland|libGL|libEGL|libvulkan|libasound|libpulse|libdrm|libgbm|libudev|libdbus)'
ldd "$BIN" | awk '/=> \//{print $1, $3}' | while read -r name path; do
    echo "$name" | grep -Eq "$skip" && continue
    cp -L "$path" "$APPDIR/usr/lib/"
done
RPATH_OK=0
if command -v patchelf >/dev/null; then
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/destrega" && RPATH_OK=1
fi

cat > "$APPDIR/AppRun" <<EOF
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
$( [ $RPATH_OK = 1 ] || echo 'export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"' )
exec "\$HERE/usr/bin/destrega" "\$@"
EOF
chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/destrega.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Destrega
Comment=Destrega for this computer (bring your own disc)
Exec=destrega
Icon=destrega
Categories=Game;
Terminal=false
EOF
cp "$ROOT/assets/icon/destrega.png" "$APPDIR/destrega.png"

python3 "$ROOT/tools/check_no_game_code.py" "$APPDIR" || die "the AppDir holds game code"
ARCH="$ARCH" appimagetool --no-appstream "$APPDIR" "$OUTF"
echo "make_appimage: built $OUTF"
