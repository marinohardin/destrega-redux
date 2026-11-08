#!/bin/sh
# Windows build under Wine vs the native build: same headless boot-to-fight inputs, compare
# the per-frame --state-hash (all emulated state) and the dumped frames. Emulation is
# deterministic, so a correct Windows build matches the native one bit for bit.
#
#   WINE=wine                                     # the wine command (path may contain spaces)
#   WINE="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine" \
#   WINE_ARGS="--bottle destrega-test --no-gui"  # CrossOver: use a dedicated bottle
#   WIN_EXE=build/win/destrega.exe NATIVE=build/cmake/destrega tools/test_wine.sh [extra args...]
#
# build/win is the MinGW cross build (cmake/mingw-w64-x86_64.cmake, see AGENTS.md); SDL3.dll
# must sit next to the .exe. Extra args (e.g. --scale 4x6 --widescreen) go to both runs.
# Needs the data pack (PAK=..., default the macOS/Linux per-user location).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
: "${WINE:=wine}"; : "${WINE_ARGS:=}"
: "${WIN_EXE:=build/win/destrega.exe}"
: "${NATIVE:=build/cmake/destrega}"
: "${FRAMES:=4000}"
: "${TAG:=fight}"
case "$(uname -s)" in
  Darwin) : "${PAK:=$HOME/Library/Application Support/Destrega/destrega.pak}" ;;
  *) : "${PAK:=${XDG_DATA_HOME:-$HOME/.local/share}/Destrega/destrega.pak}" ;;
esac
[ -f "$PAK" ] || { echo "wine: no data pack at $PAK"; exit 1; }
[ -f "$WIN_EXE" ] || { echo "wine: no $WIN_EXE (cross-build first)"; exit 1; }
winpath() { printf 'Z:%s' "$1" | tr '/' '\\'; }   # Wine maps Z: to /
O=build/wine/$TAG
rm -rf "$O"; mkdir -p "$O/native" "$O/win"
PRESS=1200:8,1500:8,1800:8,2100:8,2400:4000,2700:4000,3000:4000,3300:4000,3600:4000
COMMON="--headless --no-interp --verify-overrides --frames $FRAMES --dump-every 1000 --press $PRESS"

"$NATIVE" $COMMON --dump "$O/native" --state-hash "$O/native/hash.txt" "$@" "$PAK" > "$O/native/log.txt" 2>&1
export WINEDEBUG=${WINEDEBUG:--all}
# shellcheck disable=SC2086  # WINE_ARGS and COMMON are word lists
"$WINE" $WINE_ARGS "$(winpath "$ROOT/$WIN_EXE")" $COMMON --dump "$(winpath "$ROOT/$O/win")" \
  --state-hash "$(winpath "$ROOT/$O/win/hash.txt")" "$@" "$(winpath "$PAK")" > "$O/win/log.txt" 2>&1 || true

fail=0
for s in native win; do
  if grep -E "FATAL|dispatch: no recompiled|unhandled|MISMATCH" "$O/$s/log.txt"; then echo "wine: $s run reports errors"; fail=1; fi
  grep -q "reached $FRAMES frames" "$O/$s/log.txt" || { echo "wine: $s run did not reach frame $FRAMES (see $O/$s/log.txt)"; fail=1; }
done
if cmp -s "$O/native/hash.txt" "$O/win/hash.txt"; then
  echo "wine: state hash identical for all $(wc -l < "$O/native/hash.txt" | tr -d ' ') frames"
else
  echo "wine: state hash DIFFERS, first difference:"; diff "$O/native/hash.txt" "$O/win/hash.txt" | head -3; fail=1
fi
for f in "$O"/native/*.png; do
  b=$(basename "$f"); cmp -s "$f" "$O/win/$b" || { echo "wine: $b differs"; fail=1; }
done
[ $fail = 0 ] && echo "wine: ok" || { echo "wine: FAIL"; exit 1; }
