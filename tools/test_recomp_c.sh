#!/bin/sh
# The C recompiler (runtime/launcher/recompiler.c, used by the launcher) must write exactly what
# recomp/recomp.py writes. Runs both on the real analysis inputs and on perturbed copies (inputs
# removed, extra seeds that start mid-function, in overlays too) and diffs every output file.
# Needs disc/ (tools/extract_disc.py). Scratch in build/recomp_c_test/.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/build/recomp_c_test"
CC=${CC:-cc}
rm -rf "$W"
mkdir -p "$W"
[ -f "$ROOT/disc/SLUS_007.32" ] || { echo "test_recomp_c: disc/ missing (tools/extract_disc.py)"; exit 1; }
$CC -O2 -Wall -Wextra -Werror -o "$W/recomp_c" "$ROOT/runtime/launcher/recompiler.c" "$ROOT/runtime/launcher/recomp_cli.c"

fail=0
run_case() {   # name, then a shell snippet that edits $D/analysis
    name=$1; edit=$2
    D="$W/$name"
    mkdir -p "$D/gen" "$D/cgen"
    ln -s "$ROOT/disc" "$D/disc"
    cp -R "$ROOT/analysis" "$D/analysis"
    (cd "$D" && eval "$edit")
    t0=$(python3 -c 'import time; print(time.time())')
    (cd "$D" && python3 "$ROOT/recomp/recomp.py" --out gen > py.log 2>&1) || { echo "FAIL $name: recomp.py failed"; tail -3 "$D/py.log"; fail=1; return; }
    t1=$(python3 -c 'import time; print(time.time())')
    "$W/recomp_c" "$D/disc" "$D/analysis" "$D/cgen" > "$D/c.log" 2>&1 || { echo "FAIL $name: recomp_c failed"; tail -3 "$D/c.log"; fail=1; return; }
    t2=$(python3 -c 'import time; print(time.time())')
    if diff -r "$D/gen" "$D/cgen" > "$D/diff.txt"; then
        n=$(ls "$D/gen" | wc -l | tr -d ' ')
        printf 'ok   %-12s %s files identical (python %.2f s, C %.2f s)\n' "$name" "$n" \
            "$(echo "$t1 - $t0" | bc)" "$(echo "$t2 - $t1" | bc)"
    else
        echo "FAIL $name: outputs differ (see $D/diff.txt)"; head -20 "$D/diff.txt"; fail=1
    fi
}

run_case real ':'
run_case no_rea 'rm analysis/rea_list_procedures_main.json analysis/overlays/MGRAD.procs.json analysis/overlays/OPTOVL.procs.json'
run_case seeds 'printf "8003000c\n80040010\n800a9000\n80010104 MGRAD\n8001a300 MSEREA2\n80010040 OPTOVL\n" >> analysis/extra_seeds.txt'
run_case bare 'rm -f analysis/overrides.txt analysis/idle_loops.txt analysis/widescreen_cull.txt analysis/extra_seeds.txt'
exit $fail
