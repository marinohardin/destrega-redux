#!/bin/bash
# Scripted first launch of a launcher build (DESTREGA_GAME_MODULE, no game code in the executable).
#
#   tools/test_launcher.sh [--bin PATH] [--cc "CMD ..."] [--jobs N] [--dir DIR] [--image FILE.cue]
#
# From an empty data directory: import a disc image (--import), first launch (recompile + compile
# the game module, then boot headless), second launch (module cached, no compiler run), a changed
# analysis input (--analysis-dir with one extra comment line: the key changes, the module is
# rebuilt), and a launch with the original inputs again (rebuilt back). Each launch runs
# --frames 900 headless and must reach the title (no FATAL). Prints the time of every step.
#
#   --bin    launcher executable (default build/launcher/destrega: -DDESTREGA_GAME_MODULE=ON
#            -DDESTREGA_DEV_GAME_MODULE=OFF)
#   --cc     compiler (passed as --game-cc; default: the launcher's own choice)
#   --jobs   --game-jobs N (default: the launcher's choice, the performance cores)
#   --dir    scratch data directory (default build/launcher-test/data; emptied first)
#   --image  disc image to import (default: Destrega/Destrega.cue, else one rebuilt from the
#            data pack with tools/pak_to_bin.py into build/launcher-test/img/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/launcher/destrega"; CC=""; JOBS=""; DIR="$ROOT/build/launcher-test/data"; IMG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --cc) CC="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --dir) DIR="$2"; shift 2 ;;
        --image) IMG="$2"; shift 2 ;;
        -h|--help) sed -n 2,19p "$0"; exit 0 ;;
        *) echo "test_launcher: unknown option $1" >&2; exit 1 ;;
    esac
done
fail() { echo "test_launcher: FAIL: $*" >&2; exit 1; }
[ -x "$BIN" ] || fail "no executable at $BIN"
case "$DIR" in "$ROOT"/build/*) ;; *) fail "--dir must be under $ROOT/build" ;; esac

if [ -z "$IMG" ]; then
    if [ -f "$ROOT/Destrega/Destrega.cue" ]; then IMG="$ROOT/Destrega/Destrega.cue"
    else
        IMG="$ROOT/build/launcher-test/img/Destrega.cue"
        if [ ! -f "$IMG" ]; then
            PAK="${PAK:-$HOME/Library/Application Support/Destrega/destrega.pak}"
            [ -f "$PAK" ] || PAK="$HOME/.local/share/Destrega/destrega.pak"
            [ -f "$PAK" ] || fail "no disc image and no data pack to rebuild one from"
            mkdir -p "$(dirname "$IMG")"
            python3 "$ROOT/tools/pak_to_bin.py" "$PAK" "${IMG%.cue}" >/dev/null
        fi
    fi
fi

mkdir -p "$DIR"
find "$DIR" -mindepth 1 -delete
LOGS="$DIR.logs"; mkdir -p "$LOGS"
EXTRA=(--game-jobs "${JOBS:-0}")
[ -n "$JOBS" ] || EXTRA=()
[ -n "$CC" ] && EXTRA+=(--game-cc "$CC")

now() { python3 -c 'import time; print(time.time())'; }
step() { # NAME EXPECT(built|cached) ARGS...
    local name=$1 expect=$2; shift 2
    local t0 t1 log="$LOGS/$name.log"
    t0=$(now)
    "$BIN" --headless --data-dir "$DIR" --frames 900 --no-interp ${EXTRA[@]+"${EXTRA[@]}"} "$@" >"$log" 2>&1 ||
        fail "$name: exit $? (see $log)"
    t1=$(now)
    grep -q FATAL "$log" && fail "$name: FATAL in $log"
    case "$expect" in
        built)  grep -q "launcher: game module built" "$log" || fail "$name: the module was not built ($log)" ;;
        cached) grep -q "launcher: game module built" "$log" && fail "$name: rebuilt although nothing changed ($log)"
                grep -q "is current" "$log" || fail "$name: no cached module ($log)" ;;
    esac
    printf '%-10s %6.1f s  %s\n' "$name" "$(python3 -c "print($t1 - $t0)")" \
        "$(grep -E 'launcher: (recompiled|compiled|linked|game module)' "$log" | sed 's/^launcher: //' | tr '\n' ';' | sed 's/;$//')"
}

t0=$(now)
"$BIN" --data-dir "$DIR" --import "$IMG" >"$LOGS/import.log" 2>&1 || fail "import (see $LOGS/import.log)"
printf '%-10s %6.1f s\n' import "$(python3 -c "print($(now) - $t0)")"

step first built
step second cached
AN="$DIR.analysis"; rm -rf "$AN"; cp -R "$ROOT/analysis" "$AN"
echo "# test_launcher: a changed analysis input changes the key" >> "$AN/extra_seeds.txt"
step changed built --analysis-dir "$AN"
step changed2 cached --analysis-dir "$AN"
step original built
echo "test_launcher: PASS (logs in $LOGS)"
