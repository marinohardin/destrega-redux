#!/bin/sh
# Windowed play must be deterministic: emulated state must never depend on host timing (audio
# queue underruns, pacing, thread scheduling). Two WINDOWED runs (real window, paced at 60 Hz, real
# audio device) and one headless run with the same scripted inputs from boot into a fight must give
# identical per-frame --state-hash files. Before the fix (main.c rendered an extra 367 SPU samples
# whenever the host audio queue ran low) two windowed runs diverged around frame 189.
# If no window / audio device can be opened the windowed runs fail and the test says so.
cd "$(dirname "$0")/.."
D=build/windowed
mkdir -p $D
BIN=./build/cmake/destrega
P="--press 1200:8,1500:8,1800:8,2100:8,2400:4000,2700:4000,3000:4000,3300:4000,3600:4000"
F=${WD_FRAMES:-4300}
fail=0
JOBS=${JOBS:-3}   # the three runs are independent: below 3, one after another
run() { # name args...
  n=$1; shift
  ( $BIN "$@" > $D/$n.log 2>&1; echo $? > $D/$n.rc ) &
  [ "$JOBS" -lt 3 ] && wait
}
rm -f $D/*.rc
run w1 --no-memcard --no-settings --window 640x480 $P --frames $F --state-hash $D/w1.txt
run w2 --no-memcard --no-settings --window 640x480 $P --frames $F --state-hash $D/w2.txt
run h --headless --no-interp --no-memcard $P --frames $F --state-hash $D/h.txt
wait
[ "$(cat $D/w1.rc)" = 0 ] || { echo "windowed run 1: FAIL (exit)"; fail=1; }
[ "$(cat $D/w2.rc)" = 0 ] || { echo "windowed run 2: FAIL (exit)"; fail=1; }
[ "$(cat $D/h.rc)" = 0 ] || { echo "headless run: FAIL (exit)"; fail=1; }
n=$(wc -l < $D/w1.txt)
[ "$n" -ge $((F - 2)) ] || { echo "windowed run produced only $n hashes of $F"; fail=1; }
if cmp -s $D/w1.txt $D/w2.txt; then echo "windowed vs windowed: identical ($n frames)"; else echo "windowed vs windowed: DIFFER ($(cmp $D/w1.txt $D/w2.txt))"; fail=1; fi
if cmp -s $D/w1.txt $D/h.txt; then echo "windowed vs headless: identical"; else echo "windowed vs headless: DIFFER ($(cmp $D/w1.txt $D/h.txt))"; fail=1; fi
grep -h "audio:" $D/w1.log $D/w2.log
if [ $fail = 0 ]; then echo "windowed determinism: ok"; else echo "windowed determinism: FAILED"; fi
exit $fail
