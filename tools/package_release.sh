#!/bin/bash
# Package a finished build into a zip, on the machine that built it.
#
#   tools/package_release.sh mac [--app PATH/Destrega.app] [--personal] [--out DIR]
#   tools/package_release.sh win [--exe PATH/destrega.exe] [--dll-dir DIR]... [--personal] [--out DIR]
#   tools/package_release.sh launcher mac|win|linux [--bin PATH] [--zig DIR] [--dll-dir DIR]...
#                            [--out DIR]
#
# LAUNCHER PACKAGES (docs/RELEASES.md option A) hold no game code at all: an executable built with
#   -DDESTREGA_GAME_MODULE=ON -DDESTREGA_DEV_GAME_MODULE=OFF plus an unpacked Zig release (the C
#   compiler the launcher uses to build the game module from the player's disc on first launch).
#   Named Destrega-<version>-<os>-<arch>.zip (Linux: .AppImage via tools/make_appimage.sh). The
#   package is refused unless the executable is a launcher build (it has the module entry point
#   name) and tools/check_no_game_code.py finds nothing in the staged files.
#     mac    --bin build/launcher/destrega (default); the app is made by tools/make_app.sh --zig
#     win    --bin build/win-launcher/destrega.exe (default) + DLLs as for `win`, zig/ next to it
#     linux  --bin build/launcher/destrega; needs appimagetool on PATH
#   --zig DIR is required (the folder that holds the zig executable and its lib/).
#
# WHAT GETS PACKAGED, AND WHY IT MUST NOT BE PUBLISHED
#   A normal build contains C code that recomp/recomp.py translated from the game's own
#   executable on YOUR disc. That is a derivative of copyrighted code: the zip is for your own
#   machines (or a friend who owns the game), never for a public download page, a GitHub
#   Release or a CI artifact. The script therefore refuses to package a game build unless you
#   pass --personal, and names the file ...-PERSONAL-DO-NOT-REDISTRIBUTE.zip.
#
#   A STUB build (-DDESTREGA_STUB_GEN=ON, what CI builds, no game code, cannot play anything)
#   is packaged without --personal, as Destrega-STUB-NOT-PLAYABLE-...zip with a NOT-PLAYABLE.txt
#   inside. CI uses this to prove that packaging works. The script tells the two apart by a
#   marker string that only tools/ci/stub_gen/stub.c contains.
#
# Both zips hold the runtime, a README, THIRD_PARTY_LICENSES.txt and (Windows) the DLLs the
# exe needs. Nothing from the disc is ever included: the game imports the user's own disc image
# on first launch. Output goes to dist/ (SHA256SUMS written next to the zip).
#
#   mac   Destrega.app built by the normal macOS build (build/cmake/Destrega.app, ad-hoc signed;
#         see README for Developer ID signing and notarization). Zipped with ditto so the code
#         signature and symlinks survive.
#   win   destrega.exe (default build/win/destrega.exe, the MinGW cross build, or build/cmake/
#         destrega.exe under MSYS2) plus every DLL it imports that is found next to it, in
#         --dll-dir DIR (repeatable) or in $MINGW_PREFIX/bin (MSYS2). Windows' own DLLs are
#         skipped. For the cross build SDL3.dll sits next to the exe.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 1 ] || { sed -n 2,5p "$0"; exit 1; }
KIND="$1"; shift
LKIND=""
if [ "$KIND" = launcher ]; then
    [ $# -ge 1 ] || { echo "package_release: launcher needs mac, win or linux" >&2; exit 1; }
    LKIND="$1"; shift
fi
OUT="$ROOT/dist"; APP=""; EXE=""; PERSONAL=0; DLLDIRS=(); LBIN=""; ZIGDIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --bin) LBIN="$2"; shift 2 ;;
        --zig) ZIGDIR="$2"; shift 2 ;;
        --dll-dir) DLLDIRS+=("$2"); shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --personal) PERSONAL=1; shift ;;
        -h|--help) sed -n 2,48p "$0"; exit 0 ;;
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

if [ "$KIND" = launcher ]; then
    [ -n "$ZIGDIR" ] || die "launcher packages need --zig DIR (an unpacked Zig release)"
    [ -x "$ZIGDIR/zig" ] || [ -f "$ZIGDIR/zig.exe" ] || die "no zig executable in $ZIGDIR"
    case "$LKIND" in
        mac)   [ -n "$LBIN" ] || LBIN="$ROOT/build/launcher/destrega"; OS=macos; ARCH="$(uname -m)" ;;
        win)   [ -n "$LBIN" ] || LBIN="$ROOT/build/win-launcher/destrega.exe"; OS=windows; ARCH=x86_64 ;;
        linux) [ -n "$LBIN" ] || LBIN="$ROOT/build/launcher/destrega"; OS=linux; ARCH="$(uname -m)" ;;
        *) die "launcher kind must be mac, win or linux" ;;
    esac
    [ -f "$LBIN" ] || die "no executable at $LBIN (configure with -DDESTREGA_GAME_MODULE=ON -DDESTREGA_DEV_GAME_MODULE=OFF)"
    grep -aq destrega_game_module_init "$LBIN" ||
        die "$LBIN is not a launcher build (configure with -DDESTREGA_GAME_MODULE=ON)"
    NAME="Destrega-$VER-$OS-$ARCH"
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/destrega-pkg.XXXXXX")"
    trap 'rm -rf "$STAGE"' EXIT
    PKG="$STAGE/$NAME"; mkdir -p "$PKG" "$OUT"
    cp "$ROOT/THIRD_PARTY_LICENSES.txt" "$PKG/"
    cat > "$PKG/README.txt" <<EOF
Destrega $VER ($OS, $ARCH)

No game is included. On first launch Destrega asks for your own disc image of the game (the
North American release, SLUS-00732: .cue/.bin), imports it, and then prepares the game for this
computer: it translates the game program on your disc and compiles it with the build tools in
the "zig" folder. That takes a minute or two, once; later launches start straight away.

Third-party licences: THIRD_PARTY_LICENSES.txt (Zig: zig/LICENSE)
EOF
    case "$LKIND" in
    mac)
        [ "$(uname -s)" = Darwin ] || die "mac packaging needs macOS"
        "$ROOT/tools/make_app.sh" --bin "$LBIN" --out "$PKG/Destrega.app" --zig "$ZIGDIR" \
            --entitlements "$ROOT/tools/launcher.entitlements" >/dev/null
        codesign --verify --strict "$PKG/Destrega.app" || die "app bundle signature is invalid"
        python3 "$ROOT/tools/check_no_game_code.py" "$PKG" || die "the package holds game code"
        ZIP="$OUT/$NAME.zip"
        [ -e "$ZIP" ] && rm "$OUT/$NAME.zip"
        ditto -c -k --norsrc --noextattr --noqtn --keepParent "$PKG" "$ZIP" ;;
    win)
        cp "$LBIN" "$PKG/destrega.exe"
        copy_dlls "$LBIN" "$PKG"
        cp -R "$ZIGDIR" "$PKG/zig"
        python3 "$ROOT/tools/check_no_game_code.py" "$PKG" || die "the package holds game code"
        ZIP="$OUT/$NAME.zip"
        zip_dir "$STAGE" "$NAME" "$ZIP" ;;
    linux)
        ZIP="$OUT/$NAME.AppImage"
        "$ROOT/tools/make_appimage.sh" --bin "$LBIN" --zig "$ZIGDIR" --out "$ZIP" ;;
    esac
    if command -v shasum >/dev/null; then SUM="shasum -a 256"; else SUM=sha256sum; fi
    ( cd "$OUT" && $SUM "$(basename "$ZIP")" > "$(basename "$ZIP").sha256" )
    echo "package_release: $ZIP ($(du -h "$ZIP" | cut -f1)) launcher, no game code"
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
    *) die "first argument must be mac, win or launcher" ;;
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
