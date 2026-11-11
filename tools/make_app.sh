#!/bin/bash
# Wrap the built `destrega` binary in a self-contained macOS .app bundle.
#
# CMake runs this automatically on macOS (target `destrega_app`, part of the
# default build), producing build/cmake/Destrega.app. It can also be run by hand
# after a build:
#
#   tools/make_app.sh [--bin PATH] [--out PATH.app] [--icon PNG|ICNS]
#                     [--version X.Y] [--sign IDENTITY] [--dmg PATH.dmg]
#
#   --bin      built executable           (default: build/cmake/destrega)
#   --out      bundle to create           (default: next to --bin, Destrega.app)
#   --icon     app icon, PNG >= 512px or .icns (default: assets/icon/destrega.png)
#   --version  CFBundleShortVersionString (default: tools/version.py short = the VERSION file;
#              CFBundleVersion is the git commit count, and the full "describe" string is
#              also stored under the DestregaBuild key)
#   --sign     codesign identity          (default: "-" = ad-hoc, runs on this Mac;
#              use a "Developer ID Application: ..." identity to distribute)
#   --dmg      also write a compressed disk image
#
# Every non-system dylib the binary links (e.g. Homebrew's libSDL3) is copied
# into Contents/Frameworks and relinked through @rpath, build-machine rpaths are
# removed, and the bundle is re-signed. Game data is never bundled: on first
# launch the app asks for the disc image and imports it to
# ~/Library/Application Support/Destrega/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/cmake/destrega"
APP=""
ICON="$ROOT/assets/icon/destrega.png"
VERSION=""
SIGN="-"
DMG=""
BUNDLE_ID="io.github.destrega-recomp"

while [ $# -gt 0 ]; do
    case "$1" in
        --bin)     BIN="$2"; shift 2 ;;
        --out)     APP="$2"; shift 2 ;;
        --icon)    ICON="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --sign)    SIGN="$2"; shift 2 ;;
        --dmg)     DMG="$2"; shift 2 ;;
        -h|--help) sed -n 2,26p "$0"; exit 0 ;;
        *) echo "make_app: unknown option: $1" >&2; exit 1 ;;
    esac
done

[ "$(uname -s)" = Darwin ] || { echo "make_app: macOS only" >&2; exit 1; }
[ -x "$BIN" ] || { echo "make_app: no executable at $BIN (build first: cmake --build build/cmake)" >&2; exit 1; }
[ -n "$VERSION" ] || VERSION="$(python3 "$ROOT/tools/version.py" short)"
BUILD_NUM="$(python3 "$ROOT/tools/version.py" build)"
BUILD_DESC="$(python3 "$ROOT/tools/version.py" describe)"
YEAR="$(date +%Y)"
[ -n "$APP" ] || APP="$(dirname "$BIN")/Destrega.app"

# 1. Bundle skeleton. Build in a temp dir and swap in at the end so a failed run
#    never leaves a half-made bundle behind.
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/destrega-app.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
TMPAPP="$STAGE/Destrega.app"
mkdir -p "$TMPAPP/Contents/MacOS" "$TMPAPP/Contents/Frameworks" "$TMPAPP/Contents/Resources"
EXE="$TMPAPP/Contents/MacOS/destrega"
cp "$BIN" "$EXE"
chmod u+w "$EXE"

ICON_KEY=""
if [ -n "$ICON" ]; then
    [ -f "$ICON" ] || { echo "make_app: icon not found: $ICON" >&2; exit 1; }
    case "$ICON" in
        *.icns) cp "$ICON" "$TMPAPP/Contents/Resources/destrega.icns" ;;
        *)
            SET="$STAGE/destrega.iconset"; mkdir -p "$SET"
            for s in 16 32 128 256 512; do
                sips -z $s $s "$ICON" --out "$SET/icon_${s}x${s}.png" >/dev/null
                sips -z $((s*2)) $((s*2)) "$ICON" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
            done
            iconutil -c icns "$SET" -o "$TMPAPP/Contents/Resources/destrega.icns" ;;
    esac
    ICON_KEY="<key>CFBundleIconFile</key><string>destrega</string>"
fi

MINOS="$(otool -l "$BIN" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
cat > "$TMPAPP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Destrega</string>
    <key>CFBundleDisplayName</key><string>Destrega</string>
    <key>CFBundleExecutable</key><string>destrega</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$BUILD_NUM</string>
    <key>DestregaBuild</key><string>$BUILD_DESC</string>
    <key>NSHumanReadableCopyright</key><string>Runtime (c) $YEAR the Destrega Redux contributors. Destrega is (c) 1998 KOEI Co., Ltd. No game data is included; you supply your own disc image.</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>${MINOS:-11.0}</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    $ICON_KEY
</dict>
</plist>
EOF
plutil -lint -s "$TMPAPP/Contents/Info.plist"

cp "$ROOT/THIRD_PARTY_LICENSES.txt" "$TMPAPP/Contents/Resources/"

# 2. Copy every non-system dylib (recursively) into Frameworks; relink via @rpath.
is_external() { case "$1" in /usr/lib/*|/System/*|@*) return 1 ;; *) return 0 ;; esac; }
deps_of() { otool -L "$1" | tail -n +2 | awk '{print $1}'; }
rpaths_of() { otool -l "$1" | awk '/LC_RPATH/{f=1} f&&/ path /{print $2; f=0}'; }

queue=("$EXE")
while [ ${#queue[@]} -gt 0 ]; do
    obj="${queue[0]}"; queue=("${queue[@]:1}")
    for dep in $(deps_of "$obj"); do
        is_external "$dep" || continue
        name="$(basename "$dep")"
        dest="$TMPAPP/Contents/Frameworks/$name"
        if [ ! -f "$dest" ]; then
            cp -L "$dep" "$dest"; chmod u+w "$dest"
            install_name_tool -id "@rpath/$name" "$dest" 2>/dev/null
            queue+=("$dest")
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$obj" 2>/dev/null
    done
done

# Drop build-machine rpaths (e.g. /opt/homebrew/lib) so only the bundle is searched.
for obj in "$EXE" "$TMPAPP"/Contents/Frameworks/*; do
    [ -f "$obj" ] || continue
    for rp in $(rpaths_of "$obj"); do install_name_tool -delete_rpath "$rp" "$obj" 2>/dev/null; done
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE" 2>/dev/null

# 3. Nothing may still point outside the bundle or the OS.
leftover="$(for o in "$EXE" "$TMPAPP"/Contents/Frameworks/*; do [ -f "$o" ] && deps_of "$o"; done \
            | grep -vE '^(/usr/lib/|/System/|@rpath/|@executable_path/|@loader_path/)' || true)"
if [ -n "$leftover" ]; then echo "make_app: unbundled dependencies remain:" >&2; echo "$leftover" >&2; exit 1; fi

# 4. Sign (relinking invalidated the original signatures): libraries, then the bundle.
for lib in "$TMPAPP"/Contents/Frameworks/*; do
    [ -f "$lib" ] && codesign --force --timestamp=none -s "$SIGN" "$lib" 2>/dev/null
done
codesign --force --timestamp=none -s "$SIGN" "$TMPAPP" 2>/dev/null
codesign --verify --strict "$TMPAPP"

rm -rf "$APP"
mkdir -p "$(dirname "$APP")"
mv "$TMPAPP" "$APP"
touch "$APP"   # nudge Finder/Dock to pick up a changed icon
echo "make_app: built $APP"

if [ -n "$DMG" ]; then
    hdiutil create -volname Destrega -srcfolder "$APP" -ov -format UDZO "$DMG" >/dev/null
    echo "make_app: built $DMG"
fi
