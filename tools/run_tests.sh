#!/bin/sh
# Verification: module unit tests, headless boot-to-gameplay smoke test and the hi-res
# determinism/equivalence runs. Everything independent runs concurrently.
# Usage: tools/run_tests.sh   (from anywhere; needs a built build/cmake/destrega)
cd "$(dirname "$0")/.."
mkdir -p build/tests build/smoke build/hires
START=$(date +%s)

# 1. compile the unit tests in parallel
CC="clang -std=c11 -O2 -Wall -Wextra -pthread -Iruntime"
rm -f build/tests/*.cc.rc
cc_job() { # name cmd...
  n=$1; shift
  "$@" > build/tests/$n.cc.log 2>&1; echo $? > build/tests/$n.cc.rc
}
cc_job gte $CC runtime/gte.c runtime/tests/test_gte.c -o build/tests/test_gte &
cc_job gpu $CC runtime/gpu.c runtime/tests/test_gpu.c -o build/tests/test_gpu &
cc_job hires $CC runtime/gpu.c runtime/hires.c runtime/tests/test_hires.c -o build/tests/test_hires -lm &
cc_job spu $CC runtime/spu.c runtime/xa.c runtime/tests/test_spu.c -o build/tests/test_spu &
cc_job mdec $CC runtime/mdec.c runtime/tests/test_mdec.c -o build/tests/test_mdec -lm &
cc_job memcard $CC runtime/memcard.c runtime/tests/test_memcard.c -o build/tests/test_memcard &

# 2. the long runs start right away (the binary is already built): smoke + hi-res matrix
tools/smoke_test.sh > build/smoke/out.txt 2>&1 &
SMOKE=$!
tools/test_hires_determinism.sh > build/hires/out.txt 2>&1 &
HIRES=$!
# 3. scenario library: every mode / stage / character path, --no-interp (SCENARIOS=0 to skip), and the
#    hi-res determinism + 1x1 equivalence checks on the stage subset
JOBS_SC=${JOBS_SC:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
if [ "${SCENARIOS:-1}" != 0 ]; then
  mkdir -p build/scenarios
  python3 tools/scenarios.py run --all -j "$JOBS_SC" > build/scenarios/out.txt 2>&1 &
  SCN=$!
  python3 tools/scenarios.py hires -j "$JOBS_SC" > build/scenarios/hires_out.txt 2>&1 &
  SCNH=$!
fi
tools/test_memcard.sh > build/memcard.out 2>&1 &
MCARD=$!

wait %1 %2 %3 %4 %5 %6 2>/dev/null
fail=0
# 3. run the unit tests concurrently, report in order
for t in gte gpu hires spu mdec memcard; do
  if [ "$(cat build/tests/$t.cc.rc)" != 0 ]; then echo "$t: COMPILE FAIL"; cat build/tests/$t.cc.log; fail=1; continue; fi
  ./build/tests/test_$t > build/tests/test_$t.log 2>&1 &
  eval "UT_$t=$!"
done
for t in gte gpu hires spu mdec memcard; do
  eval "pid=\$UT_$t"
  [ -n "$pid" ] || continue
  printf '%-6s ' "$t"
  if wait "$pid"; then echo ok; else echo FAIL; tail build/tests/test_$t.log; fail=1; fi
done
wait $SMOKE || { fail=1; echo "smoke: FAILED"; }
cat build/smoke/out.txt
wait $HIRES || { fail=1; echo "hires: FAILED"; }
cat build/hires/out.txt
if [ "${SCENARIOS:-1}" != 0 ]; then
  wait $SCN || { fail=1; echo "scenarios: FAILED"; }
  cat build/scenarios/out.txt
  wait $SCNH || { fail=1; echo "scenarios hires: FAILED"; }
  cat build/scenarios/hires_out.txt
fi
wait $MCARD || { fail=1; echo "memcard e2e: FAILED"; }
cat build/memcard.out
echo "tests: $(( $(date +%s) - START ))s wall clock, $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
