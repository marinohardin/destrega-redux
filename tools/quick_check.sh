#!/bin/bash
# Quick check (~20-30 s): catches gameplay changes and big renderer / speed regressions before the
# full tools/run_tests.sh. Needs a built build/cmake/destrega and the data pack (or DESTREGA_PAK).
#  1. Gameplay: two replays (tools/replays.py run: a VS fight and a 1P battle against the CPU),
#     every frame's gameplay hashes and the complete state hash compared with the recording.
#  2. Renderer: the Hills match replay with --renderer compare --pgxp off at the 3834x2328 window
#     (4x6, widescreen guard band) from frame 3600 into the fight (hi-res starts with the resize),
#     every 10th frame, once with the headless defaults (nearest, 15-bit) and once with the window
#     defaults (xbr, full colour): GPU must equal software in every pixel.
#  3. Speed (after 1 and 2, alone on the machine): the user configuration headless, GPU renderer,
#     widescreen, 3834x2328 -> 4x6, xbr + full colour, PGXP on, 4000 frames of the Hills replay;
#     ms/frame per segment (frames 0-1200 intro FMV at 1x; 1200-3700 title, menus, character
#     select, stage load; 3700-4000 fight) against
#     tools/quick_check_baseline.txt for this machine (keyed by CPU name). Slower than baseline by
#     more than TOL percent (default 25) or 1 ms is a WARNING only: the machine is noisy (other
#     processes use the GPU too). With no entry for this machine it prints the numbers.
# Usage: tools/quick_check.sh [--update-baseline]   (exit 0 = gameplay and renderer checks pass)
# Env: TOL (percent), BIN, DESTREGA_PAK, JOBS (game processes at once in 1 + 2, default 4 = all at once). Work files go to build/quick (TMPDIR too).
cd "$(dirname "$0")/.."
BIN=${BIN:-./build/cmake/destrega}
OUT=build/quick
mkdir -p $OUT/tmp
export TMPDIR=$PWD/$OUT/tmp
TOL=${TOL:-25}
BASE=tools/quick_check_baseline.txt
PAK=()
[ -n "$DESTREGA_PAK" ] && PAK=("$DESTREGA_PAK")
MATCH=tools/replays/match_gradd_celia_hills.dtr.gz
[ -x "$BIN" ] || { echo "quick_check: no $BIN (build first)"; exit 2; }
START=$(date +%s)
fail=0
JOBS=${JOBS:-4}
RJ=2; [ "$JOBS" -lt 4 ] && RJ=1   # the replays step runs two games with -j 2
slot() { # wait until fewer than JOBS game processes run (the replays job counts RJ)
  while [ $(( $(jobs -pr | wc -l) + RJ - 1 )) -ge "$JOBS" ] && [ "$(jobs -pr | wc -l)" -gt 0 ]; do sleep 1; done
}

# 1 + 2 concurrently
DESTREGA_BIN=$BIN python3 tools/replays.py run -j $RJ vs_tieme_rohzen_villa 1p_reyus_cpu > $OUT/replays.txt 2>&1 &
RP=$!
cmp_run() { # name extra...
  local n=$1; shift
  "$BIN" --headless --no-interp --no-memcard --replay $MATCH --frames 3950 --fight-from 3700 \
    --renderer compare --compare-every 10 --compare-max 0 --pgxp off \
    --resize 3600:3834x2328 --widescreen "$@" "${PAK[@]}" > $OUT/$n.log 2>&1
}
slot
cmp_run cmp_default &
C1=$!
slot
cmp_run cmp_window --tex-filter xbr --hires-colour full &
C2=$!

wait $RP; rprc=$?
if [ $rprc = 0 ]; then echo "replays   ok   ($(grep -c '^ok' $OUT/replays.txt) replays, every frame equal)"
else echo "replays   FAIL"; grep -vE '^ok' $OUT/replays.txt | head -20; fail=1; fi
for n in cmp_default cmp_window; do
  if [ $n = cmp_default ]; then wait $C1; else wait $C2; fi
  line=$(grep 'hires-compare: all:' $OUT/$n.log)
  if [ -z "$line" ]; then
    if grep -q 'GPU renderer unavailable' $OUT/$n.log; then echo "$n skipped (no GPU device)"; continue; fi
    echo "$n FAIL (no compare result)"; tail -5 $OUT/$n.log; fail=1; continue
  fi
  nf=$(echo "$line" | sed -E 's/.*all: ([0-9]+) frames compared.*/\1/')
  nd=$(echo "$line" | sed -E 's/.*; ([0-9]+) of [0-9]+ pixels differ.*/\1/')
  if grep -qE 'FATAL|dispatch: no recompiled' $OUT/$n.log; then echo "$n FAIL (fatal)"; fail=1
  elif [ "$nd" = 0 ] && [ "$nf" -gt 0 ]; then echo "$(printf '%-9s' ${n#cmp_}) ok   (GPU == software, $nf frames at 4x6 widescreen)"
  else echo "$n FAIL: $nd pixels differ over $nf frames ($OUT/$n.log)"; fail=1; fi
done

# 3 alone
"$BIN" --headless --no-interp --no-memcard --replay $MATCH --frames 4000 --segments 1200,1800,2400,3000,3700 \
  --renderer gpu --resize 1:3834x2328 --widescreen --tex-filter xbr --hires-colour full "${PAK[@]}" > $OUT/bench.log 2>&1
MACHINE=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sed -n 's/^model name[^:]*: //p' /proc/cpuinfo 2>/dev/null | head -1)
MACHINE=${MACHINE:-unknown}
if grep -q 'GPU renderer unavailable' $OUT/bench.log; then echo "speed     skipped (no GPU device)"
else
  python3 - "$OUT/bench.log" "$BASE" "$MACHINE" "$TOL" "$1" <<'EOF'
import re, sys
log, base, machine, tol, upd = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5] == '--update-baseline'
names = ['0-1200 FMV', '1200-1800', '1800-2400', '2400-3000', '3000-3700', '3700- fight']
segs = [float(m.group(1)) for m in re.finditer(r'segment \S+: \d+ frames, ([0-9.]+) ms/frame', open(log).read())]
if len(segs) != len(names):
    print('speed     FAIL: no segment report in', log); sys.exit(0)
rows = []
try: rows = [l.rstrip('\n') for l in open(base)]
except OSError: pass
ref = None
for l in rows:
    if l.startswith(machine + '\t'): ref = [float(x) for x in l.split('\t')[1].split()]
warn = 0
print('speed     ms/frame, GPU 4x6 widescreen xbr full colour (%s)' % machine)
for k, n in enumerate(names):
    s = '          %-12s %6.2f' % (n, segs[k])
    if ref:
        lim = max(ref[k] * (1 + tol / 100), ref[k] + 1.0)
        s += '   baseline %6.2f' % ref[k]
        if segs[k] > lim: s += '   WARNING: slower than baseline + %g%%' % tol; warn += 1
    print(s)
if upd:
    rows = [l for l in rows if not l.startswith(machine + '\t') and l.strip()]
    if not rows or not rows[0].startswith('#'):
        rows.insert(0, '# tools/quick_check.sh speed baseline: CPU name <TAB> ms/frame for ' + ', '.join(names))
    rows.append(machine + '\t' + ' '.join('%.2f' % v for v in segs))
    open(base, 'w').write('\n'.join(rows) + '\n')
    print('          baseline for this machine written to', base)
elif not ref: print('          no baseline for this machine (tools/quick_check.sh --update-baseline stores one)')
elif warn: print('          %d segment(s) slower: a real regression if it repeats on an idle machine' % warn)
EOF
fi
echo "quick_check: $([ $fail = 0 ] && echo PASS || echo FAIL) in $(( $(date +%s) - START )) s"
exit $fail
