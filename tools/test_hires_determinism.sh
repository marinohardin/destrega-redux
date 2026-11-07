#!/bin/sh
# Hi-res rendering must never influence emulation, and must implement the same
# rasterization rules as the 1x GPU.
#  1. Same scripted input (boot -> 1P battle -> fight) with hi-res off, S=2, S=4, S=8 and a
#     live-resize schedule that changes S several times mid-fight: the per-frame hash of all
#     emulated state (RAM, scratchpad, 1x VRAM, SPU RAM, CPU regs) must be identical.
#  2. At S=1 the hi-res target must equal the 1x VRAM region at every frame.
# Usage: tools/test_hires_determinism.sh [frames]   (default 7000; needs build/cmake/destrega)
set -e
cd "$(dirname "$0")/.."
FRAMES=${1:-7000}
OUT=build/hires
mkdir -p $OUT
rm -f $OUT/*.hash $OUT/*.log
PRESS=$(python3 tools/fight_script.py "$FRAMES")
BIN=./build/cmake/destrega
run() { # name args...
  name=$1; shift
  $BIN --headless --no-interp --frames "$FRAMES" --press "$PRESS" --state-hash $OUT/$name.hash "$@" > $OUT/$name.log 2>&1 || { echo "hires: $name run FAILED"; tail -5 $OUT/$name.log; exit 1; }
  if grep -E "FATAL|dispatch: no recompiled|unhandled" $OUT/$name.log; then echo "hires: $name: FAIL"; exit 1; fi
}
run off --scale 0
for s in 2 4 8; do run s$s --scale $s; done
run resize --scale 0 --max-scale 8 --resize 3000:640x480,3700:1920x1440,4300:960x720,4900:3840x2880,5500:480x360,6100:1280x960,6700:2560x1920
echo "hires: resize steps applied: $(grep -c 'render scale' $OUT/resize.log)"
lines=$(wc -l < $OUT/off.hash)
[ "$lines" -ge "$FRAMES" ] || { echo "hires: FAIL (only $lines hashed frames)"; exit 1; }
for n in s2 s4 s8 resize; do
  if cmp -s $OUT/off.hash $OUT/$n.hash; then echo "hires: state hash $n == off ($lines frames): ok"
  else echo "hires: state hash $n DIFFERS from off:"; diff $OUT/off.hash $OUT/$n.hash | head -3; exit 1; fi
done
run verify1 --scale 1 --hires-verify
grep "hires-verify" $OUT/verify1.log
grep -q "hires-verify: $FRAMES frames checked, 0 mismatching" $OUT/verify1.log || { echo "hires: S=1 equivalence FAIL"; exit 1; }
cmp -s $OUT/off.hash $OUT/verify1.hash || { echo "hires: S=1 state hash differs"; exit 1; }
echo "hires: determinism + S=1 equivalence ok"
