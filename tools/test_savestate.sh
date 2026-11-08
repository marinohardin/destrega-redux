#!/bin/sh
# Save-state tests (also run by tools/run_tests.sh). Needs a built build/cmake/destrega and the data pack.
#  1. --savestate-test: save at N, restore at the first frame >= M with the same host context,
#     re-run with the same scripted inputs; every frame's complete state hash must match the
#     first pass. Points in the KOEI logo, intro FMV (STR + XA), title, menus, stage load and fight.
#  2. --savestate-rewind over a whole boot -> fight run: every 37 frames rewind 23+ frames.
#     The final timeline's per-frame complete state hash (--ss-hash) and the frontend's
#     --state-hash must equal a plain run's.
#     Slot 1 has a fresh card (its image is part of the state).
#  3. --savestate-inplace: save / corrupt everything / restore round trips leave the state unchanged.
cd "$(dirname "$0")/.."
D=build/savestate
mkdir -p $D
BIN=./build/cmake/destrega
P="--press 1200:8,1500:8,1800:8,2100:8,2400:4000,2700:4000,3000:4000,3300:4000,3600:4000"
F=${SS_FRAMES:-6000}
fail=0
rm -f $D/card_plain.mcd $D/card_rewind.mcd $D/card_inplace.mcd
# 1: logo 100, FMV 400/700, title 1300, menu 1900/2200, character select 2450, stage load 3000, fight 3900/5000
$BIN --headless --no-interp $P --frames $F \
  --savestate-test 100:130,400:460,700:790,1300:1360,1900:1930,2200:2260,2450:2510,3000:3090,3300:3360,3900:3960,5000:5240 \
  > $D/test.log 2>&1 &
T1=$!
$BIN --headless --no-interp $P --frames $F --memcard1 $D/card_plain.mcd --ss-hash $D/plain.txt --state-hash $D/plain_sh.txt \
  > $D/plain.log 2>&1 &
T2=$!
$BIN --headless --no-interp $P --frames $F --memcard1 $D/card_rewind.mcd --savestate-rewind 37:23 --ss-hash $D/rewind.txt \
  --state-hash $D/rewind_sh.txt \
  > $D/rewind.log 2>&1 &
T3=$!
$BIN --headless --no-interp $P --frames $F --memcard1 $D/card_inplace.mcd --savestate-inplace 7 --state-hash $D/inplace_sh.txt > $D/inplace.log 2>&1 &
T4=$!
wait $T1 || { echo "savestate-test: FAIL (exit)"; fail=1; }
wait $T2 || { echo "savestate plain run: FAIL (exit)"; fail=1; }
wait $T3 || { echo "savestate rewind: FAIL (exit)"; fail=1; }
wait $T4 || { echo "savestate in-place: FAIL (exit)"; fail=1; }
grep -E "^savestate" $D/test.log
n=$(grep -c "re-run: identical" $D/test.log)
[ "$n" = 11 ] || { echo "savestate-test: FAIL ($n of 11 points identical)"; fail=1; }
grep -E "^savestate: [0-9]+ (rewinds|saves)" $D/rewind.log
cmp -s $D/plain.txt $D/rewind.txt || { echo "savestate rewind: FAIL (per-frame state hashes differ from a plain run)"; fail=1; }
cmp -s $D/plain_sh.txt $D/rewind_sh.txt || { echo "savestate rewind: FAIL (--state-hash differs from a plain run)"; fail=1; }
grep -E "^savestate: [0-9]+ in-place" $D/inplace.log
cmp -s $D/card_plain.mcd $D/card_rewind.mcd || { echo "savestate rewind: FAIL (memory card image differs)"; fail=1; }
cmp -s $D/plain_sh.txt $D/inplace_sh.txt || { echo "savestate in-place: FAIL (--state-hash differs from a plain run)"; fail=1; }
grep -E "FATAL|MISMATCH" $D/*.log && fail=1
[ $fail = 0 ] && echo "savestate: ok" || echo "savestate: FAIL (logs in $D)"
exit $fail
