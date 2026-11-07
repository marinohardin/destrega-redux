#!/bin/sh
# Hi-res rendering must never influence emulation, must implement the same rasterization
# rules as the 1x GPU, and must actually be what gets shown during gameplay.
#  1. Same scripted input (boot -> 1P battle -> fight) with hi-res off, uniform scales (2x2, 4x4,
#     8x8), non-uniform scales (2x3, 4x6) and a live-resize schedule that changes Sx and Sy
#     (including Sx != Sy) mid-fight: the per-frame hash of all emulated state (RAM, scratchpad,
#     1x VRAM, SPU RAM, CPU regs) must be identical.
#  2. At 1x1 the hi-res target must equal the 1x VRAM region at every frame (also in the fight).
#  3. Every textured hi-res sample must lie inside its triangle's vertex UV range when fetched (no
#     texture bleed); at 1x1 nothing may need clamping.
#  4. In every hi-res run >= 95% of the fight-window frames (frame >= 3700) must be presentable
#     from the hi-res target rather than the 1x fallback.
# The runs are independent processes and run concurrently (JOBS at a time, default: performance
# cores). Usage: tools/test_hires_determinism.sh [frames]   (default 7000; needs build/cmake/destrega)
cd "$(dirname "$0")/.."
if [ "$1" != "--job" ]; then FRAMES=${1:-7000}; fi
OUT=build/hires
BIN=./build/cmake/destrega
export FRAMES OUT BIN

if [ "$1" = "--job" ]; then   # worker: --job name args...
  name=$2; shift 2
  PRESS=$(python3 tools/fight_script.py "$FRAMES")
  $BIN --headless --no-interp --frames "$FRAMES" --press "$PRESS" --state-hash $OUT/$name.hash "$@" > $OUT/$name.log 2>&1
  rc=$?
  if grep -qE "FATAL|dispatch: no recompiled|unhandled" $OUT/$name.log; then rc=9; fi
  echo $rc > $OUT/$name.rc
  exit 0
fi

mkdir -p $OUT
rm -f $OUT/*.hash $OUT/*.log $OUT/*.rc
JOBS=${JOBS:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
FIGHT="--hires-min-fight 95"
RESIZE="--max-scale 8 --resize 3000:640x480,3700:960x720,4300:1920x1440,4900:3840x2880,5400:480x360,5900:1920x1080,6400:1280x960,6800:2560x1920"
# longest first so the pool stays busy
cat > $OUT/jobs.txt <<EOF
s8x8 --scale 8 --hires-threads 3 $FIGHT
resize --scale 0 --hires-threads 3 $RESIZE $FIGHT
s4x6 --scale 4x6 --hires-threads 3 $FIGHT
s4x4 --scale 4 --hires-threads 1 $FIGHT
verify1 --scale 1 --hires-verify --hires-threads 1 $FIGHT
s2x3 --scale 2x3 --hires-threads 1 $FIGHT
s2x2 --scale 2 --hires-threads 0 $FIGHT
off --scale 0
EOF
START=$(date +%s)
xargs -P "$JOBS" -L1 sh "$0" --job < $OUT/jobs.txt
ELAPSED=$(( $(date +%s) - START ))

fail=0
echo "hires: ran $(wc -l < $OUT/jobs.txt) runs of $FRAMES frames, $JOBS at a time, in ${ELAPSED}s"
for n in off s2x2 s4x4 s8x8 s2x3 s4x6 resize verify1; do
  rc=$(cat $OUT/$n.rc 2>/dev/null || echo missing)
  if [ "$rc" != 0 ]; then echo "hires: $n: run FAILED (exit $rc)"; tail -5 $OUT/$n.log; fail=1; fi
done
[ $fail = 0 ] || exit 1
lines=$(wc -l < $OUT/off.hash)
[ "$lines" -ge "$FRAMES" ] || { echo "hires: FAIL (only $lines hashed frames)"; exit 1; }
echo "hires: resize steps applied: $(grep -c 'render scale' $OUT/resize.log) ($(grep -o 'render scale [0-9x]*' $OUT/resize.log | sed 's/render scale //' | tr '\n' ' '))"
for n in s2x2 s4x4 s8x8 s2x3 s4x6 resize verify1; do
  if cmp -s $OUT/off.hash $OUT/$n.hash; then echo "hires: state hash $n == off ($lines frames): ok"
  else echo "hires: state hash $n DIFFERS from off:"; diff $OUT/off.hash $OUT/$n.hash | head -3; fail=1; fi
  grep -h "hires-frames" $OUT/$n.log | sed "s/^\[ *[0-9]*\] /hires: $n: /"
  grep -h "hires-uv" $OUT/$n.log | sed "s/^\[ *[0-9]*\] /hires: $n: /"
  grep -q "hires-uv: [0-9]* textured samples, [0-9]* clamped to the 1x-sampled UV range, 0 out of range when fetched" $OUT/$n.log || { echo "hires: $n: UV out-of-range samples (or no UV report)"; fail=1; }
done
grep -q "hires-uv: [0-9]* textured samples, 0 clamped to the 1x-sampled" $OUT/verify1.log || { echo "hires: verify1: UV clamp is not a no-op at 1x1"; fail=1; }
grep -h "hires-verify" $OUT/verify1.log | sed 's/^\[ *[0-9]*\] /hires: verify1: /'
[ $fail = 0 ] || exit 1
grep -q "hires-verify: $FRAMES frames checked ([1-9][0-9]* in fight window), 0 mismatching" $OUT/verify1.log || { echo "hires: 1x1 equivalence FAIL"; exit 1; }
echo "hires: determinism + 1x1 equivalence + hi-res coverage of fight frames ok"
