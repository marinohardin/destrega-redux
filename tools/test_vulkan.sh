#!/bin/sh
# The Vulkan (SPIR-V) GPU renderer against the software renderer over a real fight:
# --renderer compare --gpu-driver vulkan, every 10th frame compared (guard band included).
#   v2x2  vw2x2   2x2, --pgxp off, 4:3 / --widescreen 16:9    0 differing pixels (CMP_MAX, default 0)
#   v4x6  vw4x6   4x6, --pgxp off, 4:3 / 16:9                 0 differing pixels
#   va4x6         4x6, --pgxp affine                          0 differing pixels
#   vp4x6 vwp4x6  4x6, --pgxp on, 4:3 / 16:9                  within CMP_MAX_PGXP % per frame (0.001)
#   vg4x6         --renderer gpu 4x6 PGXP on: state hash == software run (presentation only)
# Each run also has to reach the fight with --no-interp. Not part of run_tests.sh (it needs a
# Vulkan driver: MoltenVK + the Vulkan loader on macOS, any driver on Windows/Linux, and takes a
# few minutes); skipped with a note when there is no Vulkan device.
# Usage: tools/test_vulkan.sh [frames]   (default 7000; needs build/cmake/destrega and the data pack)
cd "$(dirname "$0")/.."
if [ "$1" != "--job" ]; then FRAMES=${1:-7000}; fi
OUT=build/vulkan
BIN=./build/cmake/destrega
[ -x "$BIN" ] || BIN=./build/cmake/destrega.exe
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
CMP_MAX=${CMP_MAX:-0}
CMP_MAX_PGXP=${CMP_MAX_PGXP:-0.001}
V="--gpu-driver vulkan --hires-min-fight 95"
C="--renderer compare --compare-every 10 --hires-threads 2"
W="--widescreen --window 1280x720"
cat > $OUT/jobs.txt <<EOF
vwp4x6 $C $V $W --scale 4x6 --pgxp on --compare-max $CMP_MAX_PGXP
vp4x6 $C $V --scale 4x6 --pgxp on --compare-max $CMP_MAX_PGXP
vw4x6 $C $V $W --scale 4x6 --pgxp off --compare-max $CMP_MAX
v4x6 $C $V --scale 4x6 --pgxp off --compare-max $CMP_MAX
va4x6 $C $V --scale 4x6 --pgxp affine --compare-max $CMP_MAX
vw2x2 $C $V $W --scale 2 --pgxp off --compare-max $CMP_MAX
v2x2 $C $V --scale 2 --pgxp off --compare-max $CMP_MAX
vg4x6 --renderer gpu $V --scale 4x6 --pgxp on
ref --scale 2 --hires-threads 1 --pgxp on
EOF
START=$(date +%s)
xargs -P "$JOBS" -L1 sh "$0" --job < $OUT/jobs.txt
echo "vulkan: ran $(wc -l < $OUT/jobs.txt) runs of $FRAMES frames, $JOBS at a time, in $(( $(date +%s) - START ))s"
if grep -q "GPU renderer unavailable" $OUT/v2x2.log; then
  echo "vulkan: no Vulkan device ($(grep -h 'hires-gpu: no GPU device' $OUT/v2x2.log | head -1 | sed 's/^\[ *[0-9]*\] //')); skipped"
  exit 0
fi
fail=0
for n in v2x2 vw2x2 v4x6 vw4x6 va4x6 vp4x6 vwp4x6 vg4x6 ref; do
  rc=$(cat $OUT/$n.rc 2>/dev/null || echo missing)
  if [ "$rc" != 0 ]; then echo "vulkan: $n: run FAILED (exit $rc)"; tail -5 $OUT/$n.log; fail=1; continue; fi
  case $n in ref) continue ;; esac
  grep -h "hires-gpu: SDL_GPU device ready" $OUT/$n.log | head -1 | grep -q "driver vulkan" || { echo "vulkan: $n: not on the Vulkan driver"; fail=1; }
  grep -h "hires-compare: fight" $OUT/$n.log | sed "s/^\[ *[0-9]*\] /vulkan: $n: /"
  case $n in vg4x6) ;; *)
    grep -q "hires-compare: fight: [1-9][0-9]* frames compared" $OUT/$n.log || { echo "vulkan: $n: no fight frames compared"; fail=1; } ;;
  esac
done
if cmp -s $OUT/ref.hash $OUT/vg4x6.hash; then echo "vulkan: state hash vg4x6 == software run: ok"
else echo "vulkan: state hash vg4x6 DIFFERS from the software run"; fail=1; fi
grep -h "hires-gpu:.*submits" $OUT/vg4x6.log | sed 's/^\[ *[0-9]*\] /vulkan: vg4x6: /'
grep -h "segment 3000-end" $OUT/vg4x6.log | sed 's/^\[ *[0-9]*\] /vulkan: vg4x6: /'
[ $fail = 0 ] && echo "vulkan: Vulkan renderer == software renderer: ok" || echo "vulkan: FAIL"
exit $fail
