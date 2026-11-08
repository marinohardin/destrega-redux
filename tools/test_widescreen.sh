#!/bin/sh
# --widescreen must not change the game. The widened stage renderer only ever runs as a
# sandboxed shadow pass (runtime/overrides/widescreen.c), so every piece of emulated state stays
# identical to a 4:3 run. Same scripted input (boot -> 1P battle -> two fights, frames >= 3700),
# runs concurrently:
#   off      4:3 reference (--scale 2)
#   ws2      --widescreen, 2x2, 16:9 window          ws2b  the same again (run-to-run determinism)
#   ws46     --widescreen, 4x6, worker threads
#   wsresize --widescreen, auto scale, window resized between 16:9, 4:3, 21:9 and 16:10 mid-fight
#   ws1      --widescreen, 1x1 with --hires-verify (the 4:3 part of the target == 1x VRAM)
#   wsrender starts on the software renderer and 4:3, switches renderer and widescreen mid-fight
#   ws46g    --widescreen, 4x6, --renderer gpu
#   wstoggle starts 4:3, switched on/off six times mid-fight: five times through View >
#            Widescreen (--menu-action widescreen[=on|off], the path the menu and F9 take), once
#            via --widescreen-at; screenshots every 50 frames
# Checks:
#  1. per frame, the gameplay trace (both fighters' health, gauge and position, round time,
#     round wins, RNG seed, a hash of both fighter structs; --gameplay-trace) equals off's;
#  2. per frame, the hash of ALL emulated state (RAM, scratchpad, 1x VRAM, SPU RAM, CPU incl.
#     cycle counter; --state-hash) equals off's - i.e. nothing at all differs;
#  3. the shadow pass ran, found extra stage packets and was never rejected;
#  4. >= 80% of fight-window frames were presented wider than 4:3 in the 16:9 runs;
#  5. 0 textured samples outside their clamp range; 1x1: 0 mismatching frames;
#  6. wstoggle: presented wide only while switched on, and soon after every switch-on
#     (tools/check_ws_toggle.py, from the log and the screenshots);
#  7. wsrender: View > Renderer switched software <-> gpu six times mid-fight, interleaved with
#     four widescreen switches (also while on) and two full-screen actions (headless: logged
#     only): every switch applied, same checks as 1-3 and 6.
#     ws46g: --widescreen on the GPU renderer at 4x6.
# Usage: tools/test_widescreen.sh [frames]   (default 9000; needs build/cmake/destrega)
cd "$(dirname "$0")/.."
if [ "$1" != "--job" ]; then FRAMES=${1:-9000}; fi
OUT=build/widescreen
BIN=./build/cmake/destrega
export FRAMES OUT BIN

if [ "$1" = "--job" ]; then   # worker: --job name args...
  name=$2; shift 2
  PRESS=$(python3 tools/fight_script.py "$FRAMES")
  $BIN --headless --no-interp --frames "$FRAMES" --press "$PRESS" --state-hash $OUT/$name.hash \
    --gameplay-trace $OUT/$name.trace "$@" > $OUT/$name.log 2>&1
  rc=$?
  if grep -qE "FATAL|dispatch: no recompiled|unhandled|MISMATCH" $OUT/$name.log; then rc=9; fi
  echo $rc > $OUT/$name.rc
  exit 0
fi

mkdir -p $OUT
rm -f $OUT/*.hash $OUT/*.trace $OUT/*.log $OUT/*.rc
rm -rf $OUT/toggle $OUT/render; mkdir -p $OUT/toggle $OUT/render
JOBS=${JOBS:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
cat > $OUT/jobs.txt <<EOF
ws46 --widescreen --scale 4x6 --window 1280x720 --hires-threads 3
wsresize --widescreen --max-scale 4x6 --hires-threads 1 --resize 2000:1280x720,4300:960x720,5200:2560x1080,6100:1440x900,7000:1920x1080
ws2 --widescreen --scale 2 --window 1280x720 --hires-threads 1
ws2b --widescreen --scale 2 --window 1280x720 --hires-threads 0
ws1 --widescreen --scale 1 --window 1280x720 --hires-verify --hires-threads 0
off --scale 2 --hires-threads 0
wsrender --scale 2 --window 1280x720 --hires-threads 0 --renderer software --menu-action 3800:renderer=gpu --menu-action 4200:widescreen=on --menu-action 4500:renderer=software --menu-action 5000:renderer=gpu --menu-action 5400:widescreen=off --menu-action 5800:renderer=software --menu-action 6300:widescreen=on --menu-action 6600:renderer=gpu --menu-action 7200:renderer --menu-action 7600:widescreen=off --menu-action 4100:fullscreen=on --menu-action 6900:fullscreen --dump build/widescreen/render --dump-every 50
ws46g --widescreen --scale 4x6 --window 1280x720 --renderer gpu
wstoggle --scale 2 --window 1280x720 --hires-threads 0 --menu-action 3900:widescreen=on --menu-action 4600:widescreen --widescreen-at 5300:1 --menu-action 6200:widescreen=off --menu-action 6900:widescreen --menu-action 8000:widescreen=off --dump build/widescreen/toggle --dump-every 50
EOF
START=$(date +%s)
xargs -P "$JOBS" -L1 sh "$0" --job < $OUT/jobs.txt
ELAPSED=$(( $(date +%s) - START ))

fail=0
WS="ws2 ws2b ws46 wsresize ws1 wstoggle wsrender ws46g"
echo "widescreen: ran $(wc -l < $OUT/jobs.txt) runs of $FRAMES frames, $JOBS at a time, in ${ELAPSED}s"
for n in off $WS; do
  rc=$(cat $OUT/$n.rc 2>/dev/null || echo missing)
  if [ "$rc" != 0 ]; then echo "widescreen: $n: run FAILED (exit $rc)"; tail -5 $OUT/$n.log; fail=1; fi
done
[ $fail = 0 ] || exit 1
lines=$(wc -l < $OUT/off.hash)
[ "$lines" -ge "$FRAMES" ] || { echo "widescreen: FAIL (only $lines hashed frames)"; exit 1; }
for n in $WS; do
  if cmp -s $OUT/off.trace $OUT/$n.trace; then echo "widescreen: gameplay trace $n == off ($lines frames): ok"
  else echo "widescreen: gameplay trace $n DIFFERS from off:"; diff $OUT/off.trace $OUT/$n.trace | head -3; fail=1; fi
  if cmp -s $OUT/off.hash $OUT/$n.hash; then echo "widescreen: full state hash $n == off: ok"
  else echo "widescreen: full state hash $n DIFFERS from off:"; diff $OUT/off.hash $OUT/$n.hash | head -3; fail=1; fi
  l=$(grep -h "widescreen: [0-9]* shadow passes" $OUT/$n.log | sed 's/^\[ *[0-9]*\] //')
  echo "widescreen: $n: $l"
  echo "$l" | grep -qE "[1-9][0-9]* shadow passes, [1-9][0-9]* extra stage packets, 0 rejected" ||
    { echo "widescreen: $n: shadow pass did not run, found nothing or was rejected"; fail=1; }
  grep -q "hires-uv: [0-9]* textured samples, [0-9]* clamped to the 1x-sampled UV range, 0 out of range when fetched" $OUT/$n.log ||
    { echo "widescreen: $n: UV out-of-range samples (or no UV report)"; fail=1; }
done
for n in ws2 ws2b ws46 ws1 ws46g; do
  l=$(grep -h "widescreen-frames:" $OUT/$n.log | sed 's/^\[ *[0-9]*\] //')
  echo "widescreen: $n: $l"
  wide=$(echo "$l" | sed -n 's/.*(\([0-9]*\) in fight window of \([0-9]*\)).*/\1 \2/p')
  set -- $wide
  [ -n "$1" ] && [ $(( $1 * 100 )) -ge $(( $2 * 80 )) ] || { echo "widescreen: $n: < 80% of fight frames presented wide"; fail=1; }
done
grep -h "resize\|render scale" $OUT/wsresize.log | sed 's/^\[ *[0-9]*\] /widescreen: wsresize: /' | head -12
grep -h "hires-verify" $OUT/ws1.log | sed 's/^\[ *[0-9]*\] /widescreen: ws1: /'
grep -q "hires-verify: $FRAMES frames checked ([1-9][0-9]* in fight window), 0 mismatching" $OUT/ws1.log ||
  { echo "widescreen: ws1: the 4:3 part of the target differs from the 1x VRAM"; fail=1; }
python3 tools/check_ws_toggle.py $OUT/wstoggle.log $OUT/toggle 6 || fail=1
python3 tools/check_ws_toggle.py $OUT/wsrender.log $OUT/render 4 > $OUT/wsrender.chk || fail=1
sed 's/wstoggle/wsrender/' $OUT/wsrender.chk
nr=$(grep -c "hires: renderer [a-z]* at frame" $OUT/wsrender.log)
echo "widescreen: wsrender: $nr renderer switches applied: $(grep -h 'hires: renderer [a-z]* at frame' $OUT/wsrender.log | sed 's/.*renderer \([a-z]*\) at frame \([0-9]*\)/\1@\2/' | tr '\n' ' ')"
[ "$nr" = 6 ] || { echo "widescreen: wsrender: expected 6 renderer switches"; fail=1; }
nf=$(grep -c "fullscreen: o[nf]* at frame" $OUT/wsrender.log)
[ "$nf" = 2 ] || { echo "widescreen: wsrender: expected 2 full-screen switches, got $nf"; fail=1; }
[ $fail = 0 ] || { echo "widescreen: FAIL"; exit 1; }
echo "widescreen: gameplay and all emulated state identical with --widescreen; shadow pass + 16:9 presentation ok"
