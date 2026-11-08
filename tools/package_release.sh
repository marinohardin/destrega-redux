#!/bin/bash
# Package a finished build into a zip, on the machine that built it.
#
#   tools/package_release.sh mac [--app PATH/Destrega.app] [--personal] [--out DIR]
#   tools/package_release.sh win [--exe PATH/destrega.exe] [--dll-dir DIR]... [--personal] [--out DIR]
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
OUT="$ROOT/dist"; APP=""; EXE=""; PERSONAL=0; DLLDIRS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --dll-dir) DLLDIRS+=("$2"); shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --personal) PERSONAL=1; shift ;;
        -h|--help) sed -n 2,34p "$0"; exit 0 ;;
        *) echo "package_release: unknown option: $1" >&2; exit 1 ;;
    esac
done

die() { echo "package_release: $*" >&2; exit 1; }
VER="$(python3 "$ROOT/tools/version.py" describe | tr + -)"
STUB_MARK="DESTREGA-CI-STUB-BUILD"

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
    *) die "first argument must be mac or win" ;;
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
