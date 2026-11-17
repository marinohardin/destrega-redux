#!/bin/bash
# Package a finished build into a zip, on the machine that built it.
#
#   tools/package_release.sh release mac|win|linux [--bin PATH] [--lib FILE] [--dll-dir DIR]...
#                            [--sign IDENTITY] [--notarize PROFILE] [--disc DIR] [--out DIR]
#   tools/package_release.sh launcher mac|win|linux [--bin PATH] [--zig DIR] [--dll-dir DIR]...
#                            [--out DIR]
#   tools/package_release.sh mac [--app PATH/Destrega.app] [--personal] [--out DIR]
#   tools/package_release.sh win [--exe PATH/destrega.exe] [--dll-dir DIR]... [--personal] [--out DIR]
#
# RELEASE PACKAGES (docs/RELEASES.md, the published downloads since 0.1.2) hold the program as
#   it was built here: the runtime plus the code translated from the game's executable, compiled
#   for this platform. They hold NO GAME DATA: no disc image, no data pack, no file from the disc,
#   no memory card. The player's own disc is imported on first launch. A package is refused
#   unless the executable is a full game build (not the CI stub, not a launcher) and
#   tools/check_no_game_data.py finds nothing in the staged files (it compares them with the
#   extracted disc: --disc DIR, default ./disc, required). Named Destrega-<version>-<os>-<arch>.
#     mac    --bin build/cmake/destrega (default); a disk image with Destrega.app (made by
#            tools/make_app.sh), an Applications shortcut, README and licences. --sign IDENTITY
#            (Developer ID Application: ...) signs app and image; --notarize PROFILE (an
#            `xcrun notarytool store-credentials` keychain profile) notarizes and staples both.
#     win    --bin build/win/destrega.exe (default) + the DLLs it imports (next to it, --dll-dir,
#            $MINGW_PREFIX/bin); a .zip.
#     linux  --bin PATH (built with RUNPATH $ORIGIN/lib, see tools/build_linux_docker.sh) and
#            --lib PATH/libSDL3.so.0; a .tar.gz with lib/, README (docs/README-LINUX.txt, @VERSION@ filled in), licences.
#
# LAUNCHER PACKAGES (docs/RELEASES.md option A, 0.1.1) hold no game code at all: an executable built with
#   -DDESTREGA_GAME_MODULE=ON -DDESTREGA_DEV_GAME_MODULE=OFF plus an unpacked Zig release (the C
#   compiler the launcher uses to build the game module from the player's disc on first launch).
#   Named Destrega-<version>-<os>-<arch>.zip (macOS: .dmg; Linux: .AppImage via tools/make_appimage.sh). The
#   package is refused unless the executable is a launcher build (it has the module entry point
#   name) and tools/check_no_game_code.py finds nothing in the staged files.
#     mac    --bin build/launcher/destrega (default); the app is made by tools/make_app.sh --zig.
#            Signing and notarization as for release packages.
#     win    --bin build/win-launcher/destrega.exe (default) + DLLs as for `win`, zig/ next to it
#     linux  --bin build/launcher/destrega; needs appimagetool on PATH
#   --zig DIR is required (the folder that holds the zig executable and its lib/).
#
# PERSONAL AND STUB ZIPS (`mac` / `win`)
#   `mac` / `win` zip a game build for your own machines without the release checks: they need
#   --personal and are named ...-PERSONAL-DO-NOT-REDISTRIBUTE.zip. Publish only release packages.
#
#   A STUB build (-DDESTREGA_STUB_GEN=ON, what CI builds, no game code, cannot play anything)
#   is packaged without --personal, as Destrega-STUB-NOT-PLAYABLE-...zip with a NOT-PLAYABLE.txt
#   inside. CI uses this to prove that packaging works. The script tells the two apart by a
#   marker string that only tools/ci/stub_gen/stub.c contains.
#
# These zips hold the runtime, a README, THIRD_PARTY_LICENSES.txt and (Windows) the DLLs the
# exe needs. Nothing from the disc is ever included. Output goes to dist/ (a .sha256 next to
# each package).
#
#   mac   Destrega.app built by the normal macOS build (build/cmake/Destrega.app, ad-hoc signed).
#         Zipped with ditto so the code signature and symlinks survive.
#   win   destrega.exe (default build/win/destrega.exe, the MinGW cross build, or build/cmake/
#         destrega.exe under MSYS2) plus every DLL it imports that is found next to it, in
#         --dll-dir DIR (repeatable) or in $MINGW_PREFIX/bin (MSYS2). Windows' own DLLs are
#         skipped. For the cross build SDL3.dll sits next to the exe.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 1 ] || { sed -n 2,9p "$0"; exit 1; }
KIND="$1"; shift
LKIND=""
if [ "$KIND" = launcher ] || [ "$KIND" = release ]; then
    [ $# -ge 1 ] || { echo "package_release: $KIND needs mac, win or linux" >&2; exit 1; }
    LKIND="$1"; shift
fi
OUT="$ROOT/dist"; APP=""; EXE=""; PERSONAL=0; DLLDIRS=(); LBIN=""; ZIGDIR=""; SIGNID="-"; NOTARY=""
LIB=""; DISC=""
while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --bin) LBIN="$2"; shift 2 ;;
        --zig) ZIGDIR="$2"; shift 2 ;;
        --sign) SIGNID="$2"; shift 2 ;;
        --notarize) NOTARY="$2"; shift 2 ;;
        --dll-dir) DLLDIRS+=("$2"); shift 2 ;;
        --lib) LIB="$2"; shift 2 ;;
        --disc) DISC="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --personal) PERSONAL=1; shift ;;
        -h|--help) sed -n 2,57p "$0"; exit 0 ;;
        *) echo "package_release: unknown option: $1" >&2; exit 1 ;;
    esac
done

die() { echo "package_release: $*" >&2; exit 1; }
VER="$(python3 "$ROOT/tools/version.py" describe | tr + -)"
STUB_MARK="DESTREGA-CI-STUB-BUILD"

# copy every DLL that $1 imports (recursively) from DLLDIRS into $2
copy_dlls() {
    local OBJDUMP=objdump o
    for o in x86_64-w64-mingw32-objdump llvm-objdump; do
        if command -v "$o" >/dev/null; then OBJDUMP=$o; break; fi
    done
    command -v "$OBJDUMP" >/dev/null || die "need objdump (MinGW binutils) to list the exe's DLLs"
    DLLDIRS+=("$(dirname "$1")")
    [ -n "${MINGW_PREFIX:-}" ] && DLLDIRS+=("$MINGW_PREFIX/bin")
    local queue=("$1") seen=" " obj dll key d f
    while [ ${#queue[@]} -gt 0 ]; do
        obj="${queue[0]}"; queue=("${queue[@]:1}")
        for dll in $("$OBJDUMP" -p "$obj" | awk '/DLL Name:/{print $3}'); do
            key="$(echo "$dll" | tr 'A-Z' 'a-z')"
            case "$seen" in *" $key "*) continue ;; esac
            seen="$seen$key "
            for d in "${DLLDIRS[@]}"; do
                f="$(find "$d" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null || true)"
                if [ -n "$f" ]; then cp "$f" "$2/"; queue+=("$f"); break; fi
            done
        done
    done
    ls "$2" | grep -qi '^SDL3\.dll$' || die "SDL3.dll not found (use --dll-dir)"
}

# zip STAGE/NAME into ZIP, keeping the executable bits (zig)
zip_dir() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import os, sys, zipfile
stage, name, out = sys.argv[1:4]
if os.path.exists(out):
    os.remove(out)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dp, dns, files in os.walk(os.path.join(stage, name)):
        dns.sort()
        for f in sorted(files):
            p = os.path.join(dp, f)
            info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, stage))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as fh:
                z.writestr(info, fh.read())
PY
}

if [ "$KIND" = launcher ] || [ "$KIND" = release ]; then
    if [ "$KIND" = launcher ]; then
        [ -n "$ZIGDIR" ] || die "launcher packages need --zig DIR (an unpacked Zig release)"
        [ -x "$ZIGDIR/zig" ] || [ -f "$ZIGDIR/zig.exe" ] || die "no zig executable in $ZIGDIR"
        DEFMAC="$ROOT/build/launcher/destrega"; DEFWIN="$ROOT/build/win-launcher/destrega.exe"
    else
        DEFMAC="$ROOT/build/cmake/destrega"; DEFWIN="$ROOT/build/win/destrega.exe"
    fi
    case "$LKIND" in
        mac)   [ -n "$LBIN" ] || LBIN="$DEFMAC"; OS=macos; ARCH="$(uname -m)" ;;
        win)   [ -n "$LBIN" ] || LBIN="$DEFWIN"; OS=windows; ARCH=x86_64 ;;
        linux) [ -n "$LBIN" ] || LBIN="$ROOT/build/launcher/destrega"; OS=linux; ARCH=x86_64 ;;
        *) die "$KIND kind must be mac, win or linux" ;;
    esac
    [ -f "$LBIN" ] || die "no executable at $LBIN"
    if [ "$KIND" = launcher ]; then
        grep -aq destrega_game_module_init "$LBIN" ||
            die "$LBIN is not a launcher build (configure with -DDESTREGA_GAME_MODULE=ON)"
    else
        ! grep -aq "$STUB_MARK" "$LBIN" || die "$LBIN is the CI stub build: it cannot play the game"
        ! grep -aq destrega_game_module_init "$LBIN" ||
            die "$LBIN is a launcher build; release packages hold the full game build"
        [ "$(grep -aoE 'f_8[0-9a-f]{7}' "$LBIN" | sort -u | wc -l)" -gt 1000 ] ||
            die "$LBIN does not look like a full game build (too few recompiled functions)"
        [ -n "$DISC" ] || DISC="$ROOT/disc"
        [ -f "$DISC/SLUS_007.32" ] ||
            die "release packages are checked against the extracted disc: run tools/extract_disc.py or pass --disc DIR"
        if [ "$LKIND" = linux ]; then
            [ -n "$LIB" ] && [ -f "$LIB" ] || die "linux release packages need --lib PATH/libSDL3.so.0"
            grep -aq '$ORIGIN/lib' "$LBIN" || die "$LBIN has no RUNPATH \$ORIGIN/lib (tools/build_linux_docker.sh sets it)"
        fi
    fi
    NAME="Destrega-$VER-$OS-$ARCH"
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/destrega-pkg.XXXXXX")"
    trap 'rm -rf "$STAGE"' EXIT
    PKG="$STAGE/$NAME"; mkdir -p "$PKG" "$OUT"
    cp "$ROOT/THIRD_PARTY_LICENSES.txt" "$ROOT/LICENSE" "$PKG/"
    # the release check: no game data in the staged package; the launcher check: no game code
    check_pkg() {
        if [ "$KIND" = release ]; then
            python3 "$ROOT/tools/check_no_game_data.py" --disc "$DISC" "$@" || die "the package holds game data"
        else
            python3 "$ROOT/tools/check_no_game_code.py" "$@" || die "the package holds game code"
        fi
    }
    if [ "$KIND" = release ] && [ "$LKIND" = linux ]; then
        sed "s/@VERSION@/$VER/" "$ROOT/docs/README-LINUX.txt" > "$PKG/README.txt"
    elif [ "$KIND" = release ]; then
        cat > "$PKG/README.txt" <<EOF
Destrega Redux $VER ($OS, $ARCH)

ALPHA RELEASE. This is an early test build for people comfortable with rough edges: it is not
user-friendly yet, things will break, and settings or save formats may change between versions.
Report problems at https://github.com/marinohardin/destrega-redux/issues

No game data is included. The program contains code translated from the game's executable and
none of the game's data. On first launch Destrega asks for your own disc image of the game (the
North American release, SLUS-00732: .cue/.bin), checks it and imports it. Then you play.

Destrega Redux is an unofficial fan project, not affiliated with, endorsed by or sponsored by
Koei Tecmo or Sony Interactive Entertainment. Legal notice: see the README on the project page.

Destrega Redux is free software under the GPL-3.0 (LICENSE). Third-party licences:
THIRD_PARTY_LICENSES.txt
EOF
    else
        cat > "$PKG/README.txt" <<EOF
Destrega $VER ($OS, $ARCH)

ALPHA RELEASE. This is an early test build for people comfortable with rough edges: it is not
user-friendly yet, things will break, and settings or save formats may change between versions.
Report problems at https://github.com/marinohardin/destrega-redux/issues

No game is included. On first launch Destrega asks for your own disc image of the game (the
North American release, SLUS-00732: .cue/.bin), imports it, and then prepares the game for this
computer: it translates the game program on your disc and compiles it with the build tools that
come with it (Zig). That takes a minute or two, once; later launches start straight away.

Destrega is free software under the GPL-3.0 (LICENSE). Third-party licences:
THIRD_PARTY_LICENSES.txt (Zig: zig/LICENSE)
EOF
    fi
    case "$LKIND" in
    mac)
        [ "$(uname -s)" = Darwin ] || die "mac packaging needs macOS"
        MAKEAPP=(--bin "$LBIN" --out "$PKG/Destrega.app" --sign "$SIGNID")
        [ "$KIND" = launcher ] && MAKEAPP+=(--zig "$ZIGDIR" --entitlements "$ROOT/tools/launcher.entitlements")
        "$ROOT/tools/make_app.sh" "${MAKEAPP[@]}" >/dev/null
        codesign --verify --strict "$PKG/Destrega.app" || die "app bundle signature is invalid"
        check_pkg "$PKG"
        notarize() {    # $1 = file to submit (zip or dmg)
            xcrun notarytool submit "$1" --keychain-profile "$NOTARY" --wait --timeout 60m \
                > "$STAGE/notary.log" 2>&1 || { cat "$STAGE/notary.log" >&2; die "notarization failed"; }
            cat "$STAGE/notary.log"
            grep -q "status: Accepted" "$STAGE/notary.log" || die "notarization was not accepted"
        }
        if [ -n "$NOTARY" ]; then
            [ "$SIGNID" != "-" ] || die "--notarize needs --sign with a Developer ID identity"
            ditto -c -k --keepParent "$PKG/Destrega.app" "$STAGE/app.zip"
            notarize "$STAGE/app.zip"
            xcrun stapler staple "$PKG/Destrega.app" || die "stapling the app failed"
        fi
        ln -s /Applications "$PKG/Applications"
        ZIP="$OUT/$NAME.dmg"; rm -f "$ZIP"
        hdiutil create -volname "Destrega $VER" -srcfolder "$PKG" -fs HFS+ -format UDZO \
            -imagekey zlib-level=9 -ov "$ZIP" >/dev/null || die "hdiutil create failed"
        if [ "$SIGNID" != "-" ]; then
            codesign --force --timestamp -s "$SIGNID" "$ZIP" || die "signing the disk image failed"
        fi
        if [ -n "$NOTARY" ]; then
            notarize "$ZIP"
            xcrun stapler staple "$ZIP" || die "stapling the disk image failed"
            spctl -a -t open --context context:primary-signature -v "$ZIP" || die "Gatekeeper rejects the image"
        fi ;;
    win)
        cp "$LBIN" "$PKG/destrega.exe"
        copy_dlls "$LBIN" "$PKG"
        [ "$KIND" = launcher ] && cp -R "$ZIGDIR" "$PKG/zig"
        check_pkg "$PKG"
        ZIP="$OUT/$NAME.zip"
        zip_dir "$STAGE" "$NAME" "$ZIP" ;;
    linux)
        if [ "$KIND" = release ]; then
            mkdir -p "$PKG/lib"
            cp "$LBIN" "$PKG/destrega"; chmod +x "$PKG/destrega"
            cp -L "$LIB" "$PKG/lib/libSDL3.so.0"
            check_pkg "$PKG"
            ZIP="$OUT/$NAME.tar.gz"; rm -f "$ZIP"
            tar -C "$STAGE" -czf "$ZIP" "$NAME"
            check_pkg "$ZIP"
        else
            ZIP="$OUT/$NAME.AppImage"
            "$ROOT/tools/make_appimage.sh" --bin "$LBIN" --zig "$ZIGDIR" --out "$ZIP"
        fi ;;
    esac
    if command -v shasum >/dev/null; then SUM="shasum -a 256"; else SUM=sha256sum; fi
    ( cd "$OUT" && $SUM "$(basename "$ZIP")" > "$(basename "$ZIP").sha256" )
    [ "$KIND" = release ] && [ "$LKIND" = win ] && check_pkg "$ZIP"
    if [ "$KIND" = launcher ]; then
        echo "package_release: $ZIP ($(du -h "$ZIP" | cut -f1)) launcher, no game code"
    else
        echo "package_release: $ZIP ($(du -h "$ZIP" | cut -f1)) release, no game data"
    fi
    exit 0
fi

case "$KIND" in
    mac)
        [ "$(uname -s)" = Darwin ] || die "mac packaging needs macOS (ditto, codesign)"
        [ -n "$APP" ] || APP="$ROOT/build/cmake/Destrega.app"
        [ -d "$APP" ] || die "no app bundle at $APP (build first: cmake --build build/cmake)"
        BIN="$APP/Contents/MacOS/destrega"
        ARCH="$(uname -m)"; OS=macos ;;
    win)
        [ -n "$EXE" ] || EXE="$ROOT/build/win/destrega.exe"
        [ -f "$EXE" ] || die "no exe at $EXE (cross-build first, see AGENTS.md)"
        BIN="$EXE"; OS=windows; ARCH=x86_64 ;;
    *) die "first argument must be release, launcher, mac or win" ;;
esac

if grep -aq "$STUB_MARK" "$BIN"; then
    STUB=1
    NAME="Destrega-STUB-NOT-PLAYABLE-$VER-$OS-$ARCH"
else
    STUB=0
    [ "$PERSONAL" = 1 ] || die "$BIN contains code translated from a game disc. Pass --personal to
  package it for your own machines (never publish it). CI builds use the stub instead."
    NAME="Destrega-$VER-$OS-$ARCH-PERSONAL-DO-NOT-REDISTRIBUTE"
fi

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/destrega-pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"
mkdir -p "$PKG" "$OUT"
cp "$ROOT/THIRD_PARTY_LICENSES.txt" "$PKG/"

if [ "$STUB" = 1 ]; then
    cat > "$PKG/NOT-PLAYABLE.txt" <<EOF
THIS IS A STUB BUILD. IT CANNOT RUN THE GAME.

It was built without the game's code (the code is generated from a disc image, which a public
build server does not have) to prove that the runtime compiles, links and packages. It starts,
prints its options and then reports that there is no data pack.

To play: build Destrega yourself from your own disc image, see the README in the repository.
Version: $VER
EOF
else
    cat > "$PKG/README.txt" <<EOF
Destrega $VER ($OS, $ARCH) - PERSONAL BUILD

This build contains code that was translated from the game executable on the disc it was
built from. Keep it to yourself: do not upload it, attach it to a release or share it
publicly. Game data is not included. On first launch the game asks for your disc image
(.cue/.bin of the North American release, SLUS-00732) and imports it once.

Third-party licences: THIRD_PARTY_LICENSES.txt
EOF
fi

case "$KIND" in
mac)
    ditto "$APP" "$PKG/Destrega.app"
    codesign --verify --strict "$PKG/Destrega.app" || die "app bundle signature is invalid"
    ZIP="$OUT/$NAME.zip"; rm -f "$ZIP"
    # ditto keeps the code signature, symlinks and extended attributes; --keepParent puts the
    # package folder (README, licences, app) at the top of the archive.
    ditto -c -k --norsrc --noextattr --noqtn --keepParent "$PKG" "$ZIP"
    ;;
win)
    OBJDUMP=objdump
    for o in x86_64-w64-mingw32-objdump llvm-objdump; do
        if command -v "$o" >/dev/null; then OBJDUMP=$o; break; fi
    done
    command -v "$OBJDUMP" >/dev/null || die "need objdump (MinGW binutils) to list the exe's DLLs"
    DLLDIRS+=("$(dirname "$EXE")")
    [ -n "${MINGW_PREFIX:-}" ] && DLLDIRS+=("$MINGW_PREFIX/bin")
    cp "$EXE" "$PKG/destrega.exe"
    queue=("$EXE"); seen=" "
    while [ ${#queue[@]} -gt 0 ]; do
        obj="${queue[0]}"; queue=("${queue[@]:1}")
        for dll in $("$OBJDUMP" -p "$obj" | awk '/DLL Name:/{print $3}'); do
            key="$(echo "$dll" | tr 'A-Z' 'a-z')"
            case "$seen" in *" $key "*) continue ;; esac
            seen="$seen$key "
            for d in "${DLLDIRS[@]}"; do
                f="$(find "$d" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null || true)"
                if [ -n "$f" ]; then cp "$f" "$PKG/"; queue+=("$f"); break; fi
            done
        done
    done
    ls "$PKG" | grep -qi '^SDL3\.dll$' || die "SDL3.dll not found (use --dll-dir)"
    ZIP="$OUT/$NAME.zip"; rm -f "$ZIP"
    python3 -I - "$STAGE" "$NAME" "$ZIP" <<'PY'
import os, sys, zipfile
stage, name, out = sys.argv[1:4]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dp, _, files in os.walk(os.path.join(stage, name)):
        for f in sorted(files):
            p = os.path.join(dp, f)
            z.write(p, os.path.relpath(p, stage))
PY
    ;;
esac

if command -v shasum >/dev/null; then SUM="shasum -a 256"; else SUM=sha256sum; fi
( cd "$OUT" && $SUM "$(basename "$ZIP")" > "$(basename "$ZIP").sha256" )
echo "package_release: $ZIP ($(du -h "$ZIP" | cut -f1)) stub=$STUB"
