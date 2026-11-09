#!/bin/sh
# Render-quality modes (texture filter, full-precision colour, SSAA; runtime/hires.c, hires_gpu.c)
# are presentation only and the two renderers agree on them exactly:
#  1. State hash: the same boot -> fight run with every mode on (software 4x6 and GPU 4x6, PGXP on,
#     and switched live through the menu actions mid-fight) hashes the same as a hi-res-off run.
#  2. Software == GPU: --renderer compare --pgxp off, every 10th frame, --compare-max 0 (no compared
#     frame may differ in a single pixel), for each mode alone and all combined, in 4:3 and 16:9
#     (--widescreen, guard band included). SSAA runs use a 2x3 base (= 4x6 render target).
#  3. In every run >= 95% of the fight frames come from the hi-res target.
# The default modes (nearest, 15-bit, no SSAA: headless default) are covered by
# tools/test_hires_determinism.sh (1x1 --hires-verify, compare at 1x1 and 4x6).
# Runs JOBS at a time (default: performance cores). CASES="call169 glive ..." runs only those (plus
# the reference run `off`; tools/pr_check.sh). Usage: tools/test_render_quality.sh [frames]
# (default 7000; needs build/cmake/destrega). Skips the GPU runs when SDL_GPU is unavailable.
cd "$(dirname "$0")/.."
if [ "$1" != "--job" ]; then FRAMES=${1:-7000}; fi
OUT=build/quality
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
CMP="--renderer compare --compare-every 10 --compare-max 0 --pgxp off --hires-threads 2 $FIGHT"
WS="--widescreen --window 1280x720"
ALL="--tex-filter xbr --hires-colour full"
LIVE="--menu-action 3900:tex-filter=bilinear --menu-action 4300:smooth-colours=on --menu-action 4700:ssaa=2 --menu-action 5100:tex-filter=xbr --menu-action 5500:smooth-colours=off --menu-action 5900:ssaa=1 --menu-action 6300:smooth-colours --menu-action 6600:tex-filter=nearest"
cat > $OUT/jobs.txt <<EOF
call169 $CMP $WS --scale 2x3 --ssaa 2 $ALL
cssaa169 $CMP $WS --scale 2x3 --ssaa 2
call43 $CMP --scale 2x3 --ssaa 2 $ALL
cssaa43 $CMP --scale 2x3 --ssaa 2
cbil169 $CMP $WS --scale 4x6 --tex-filter bilinear
cxbr169 $CMP $WS --scale 4x6 --tex-filter xbr
cfull169 $CMP $WS --scale 4x6 --hires-colour full
cbil43 $CMP --scale 4x6 --tex-filter bilinear
cxbr43 $CMP --scale 4x6 --tex-filter xbr
cfull43 $CMP --scale 4x6 --hires-colour full
sall --scale 4x6 --hires-threads 3 --pgxp on $ALL --ssaa 2 --max-scale 4x6 $FIGHT
gall --renderer gpu --scale 4x6 --pgxp on $ALL --ssaa 2 $FIGHT
glive --renderer gpu --scale 2x3 --pgxp on $LIVE $FIGHT
slive --scale 2x3 --hires-threads 2 --pgxp on $WS $LIVE $FIGHT
off --scale 0 --pgxp off
EOF
# CASES: a subset (the reference run `off` is always included)
has() { [ -z "$CASES" ] && return 0; case " off $CASES " in *" $1 "*) return 0 ;; esac; return 1; }
sel() { for c in "$@"; do if has $c; then printf '%s ' $c; fi; done; }
if [ -n "$CASES" ]; then
  awk -v keep=" off $CASES " 'index(keep, " " $1 " ")' $OUT/jobs.txt > $OUT/jobs.sel && mv $OUT/jobs.sel $OUT/jobs.txt
fi
START=$(date +%s)
xargs -P "$JOBS" -L1 sh "$0" --job < $OUT/jobs.txt
ELAPSED=$(( $(date +%s) - START ))

fail=0
echo "quality: ran $(wc -l < $OUT/jobs.txt) runs of $FRAMES frames, $JOBS at a time, in ${ELAPSED}s"
SW=$(sel sall slive)
CMPS=$(sel call169 cssaa169 call43 cssaa43 cbil169 cxbr169 cfull169 cbil43 cxbr43 cfull43)
GPU="$(sel gall glive) $CMPS"
if [ -n "${GPU# }" ] && grep -q "GPU renderer unavailable" $(for n in $GPU; do echo $OUT/$n.log; done); then
  echo "quality: SDL_GPU unavailable, GPU and compare runs skipped"; GPU=""; CMPS=""
fi
for n in off $SW $GPU; do
  rc=$(cat $OUT/$n.rc 2>/dev/null || echo missing)
  if [ "$rc" != 0 ]; then echo "quality: $n: run FAILED (exit $rc)"; tail -5 $OUT/$n.log; fail=1; fi
done
[ $fail = 0 ] || exit 1
lines=$(wc -l < $OUT/off.hash)
[ "$lines" -ge "$FRAMES" ] || { echo "quality: FAIL (only $lines hashed frames)"; exit 1; }
for n in $SW $GPU; do
  if cmp -s $OUT/off.hash $OUT/$n.hash; then echo "quality: state hash $n == off ($lines frames): ok"
  else echo "quality: state hash $n DIFFERS from off:"; diff $OUT/off.hash $OUT/$n.hash | head -3; fail=1; fi
  grep -h "texture filter\|hires-frames" $OUT/$n.log | tail -2 | sed "s/^\[ *[0-9]*\] /quality: $n: /"
done
for n in $(sel slive glive); do   # every live switch reached the target
  c=$(grep -c "hires: texture filter" $OUT/$n.log)
  [ "$c" -ge 9 ] || { echo "quality: $n: only $c render-quality changes logged (want 9)"; fail=1; }
done
for n in $CMPS; do
  grep -h "hires-compare" $OUT/$n.log | sed "s/^\[ *[0-9]*\] /quality: $n: /"
  grep -q "hires-compare: fight: [1-9][0-9]* frames compared" $OUT/$n.log || { echo "quality: $n: no fight frames compared"; fail=1; }
  grep -q "hires-compare: all: [0-9]* frames compared, [0-9]* identical; 0 of [0-9]* pixels differ.*, 0 in the mask bit only" $OUT/$n.log || { echo "quality: $n: software and GPU differ"; fail=1; }
done
[ $fail = 0 ] || exit 1
echo "quality: render-quality modes: state hash unchanged, software == GPU in every mode ok"
