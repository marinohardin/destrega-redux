#!/bin/sh
# Verification: module unit tests, headless boot-to-gameplay smoke test, the hi-res
# determinism/equivalence runs and the --widescreen gameplay-equivalence runs. Everything
# independent runs concurrently.
# Usage: tools/run_tests.sh   (from anywhere; needs a built build/cmake/destrega)
cd "$(dirname "$0")/.."
mkdir -p build/tests build/smoke build/hires build/widescreen
START=$(date +%s)

# 1. compile the unit tests in parallel
CC="clang -std=c11 -O2 -Wall -Wextra -pthread -Iruntime"
rm -f build/tests/*.cc.rc
cc_job() { # name cmd...
  n=$1; shift
  "$@" > build/tests/$n.cc.log 2>&1; echo $? > build/tests/$n.cc.rc
}
cc_job gte $CC runtime/gte.c runtime/pgxp.c runtime/tests/test_gte.c -o build/tests/test_gte &
cc_job gpu $CC runtime/gpu.c runtime/pgxp.c runtime/tests/test_gpu.c -o build/tests/test_gpu &
cc_job hires $CC runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c runtime/tests/test_hires.c -o build/tests/test_hires -lm &
cc_job pgxp $CC runtime/gte.c runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c runtime/tests/test_pgxp.c -o build/tests/test_pgxp -lm &
cc_job spu $CC runtime/spu.c runtime/xa.c runtime/tests/test_spu.c -o build/tests/test_spu &
cc_job mdec $CC runtime/mdec.c runtime/tests/test_mdec.c -o build/tests/test_mdec -lm &
cc_job memcard $CC runtime/memcard.c runtime/platform.c runtime/tests/test_memcard.c -o build/tests/test_memcard &
SDLF=$(pkg-config --cflags --libs sdl3 2>/dev/null || echo "-I/opt/homebrew/include -L/opt/homebrew/lib -lSDL3")
cc_job input $CC runtime/input.c runtime/settings.c runtime/platform.c runtime/tests/test_input.c -o build/tests/test_input $SDLF -lm &
cc_job hgpu $CC runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/pgxp.c runtime/platform.c runtime/tests/test_hires_gpu.c -o build/tests/test_hires_gpu $SDLF -lm &

# 2. the long runs start right away (the binary is already built): smoke + hi-res matrix
tools/smoke_test.sh > build/smoke/out.txt 2>&1 &
SMOKE=$!
tools/test_hires_determinism.sh > build/hires/out.txt 2>&1 &
HIRES=$!
JOBS=4 tools/test_widescreen.sh > build/widescreen/out.txt 2>&1 &
WIDE=$!
# render-quality modes (texture filter, full colour, SSAA): state hash + software == GPU
mkdir -p build/quality
JOBS=4 tools/test_render_quality.sh > build/quality/out.txt 2>&1 &
QUAL=$!
# 3. scenario library: every mode / stage / character path, --no-interp (SCENARIOS=0 to skip), the
#    hi-res determinism + 1x1 equivalence checks on the stage subset, and every scenario with
#    periodic save-state rewinds
JOBS_SC=${JOBS_SC:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
if [ "${SCENARIOS:-1}" != 0 ]; then
  mkdir -p build/scenarios
  python3 tools/scenarios.py run --all -j "$JOBS_SC" > build/scenarios/out.txt 2>&1 &
  SCN=$!
  python3 tools/scenarios.py hires -j "$JOBS_SC" > build/scenarios/hires_out.txt 2>&1 &
  SCNH=$!
  python3 tools/scenarios.py savestate -j "$JOBS_SC" > build/scenarios/savestate_out.txt 2>&1 &
  SCNS=$!
fi
tools/test_memcard.sh > build/memcard.out 2>&1 &
MCARD=$!
tools/test_menu.sh > build/menu.out 2>&1 &
MENU=$!
tools/test_savestate.sh > build/savestate.out 2>&1 &
SAVST=$!
# input layer + settings file: custom bindings and synthetic host events == the --press run
python3 tools/test_input.py > build/input.out 2>&1 &
INPT=$!
# gameplay regression replays (tools/replays/*.dtr.gz): every frame's gameplay fields must match
python3 tools/replays.py run -j 4 > build/replays.out 2>&1 &
REPL=$!
# rollback netplay: host + join over loopback with simulated latency / loss, against an offline run
python3 tools/test_netplay.py -j 6 > build/netplay.out 2>&1 &
NETP=$!
# the same between the native build and the Windows build under Wine (skips itself without Wine /
# build/win/destrega.exe)
python3 tools/test_netplay_wine.py > build/netplay_wine.out 2>&1 &
NETW=$!

wait %1 %2 %3 %4 %5 %6 %7 %8 %9 2>/dev/null
fail=0
# 3. run the unit tests concurrently, report in order
for t in gte gpu hires pgxp spu mdec memcard input; do
  if [ "$(cat build/tests/$t.cc.rc)" != 0 ]; then echo "$t: COMPILE FAIL"; cat build/tests/$t.cc.log; fail=1; continue; fi
  ./build/tests/test_$t > build/tests/test_$t.log 2>&1 &
  eval "UT_$t=$!"
done
for t in gte gpu hires pgxp spu mdec memcard input; do
  eval "pid=\$UT_$t"
  [ -n "$pid" ] || continue
  printf '%-6s ' "$t"
  if wait "$pid"; then echo ok; else echo FAIL; tail build/tests/test_$t.log; fail=1; fi
done
# GPU backend vs software on random command streams: the default API (Metal on macOS), Vulkan
# (MoltenVK here, if the Vulkan loader is installed), Direct3D 12 (Windows only) and the live
# API switch (shutdown + restart on every available API); exit 77 = no such device, skipped
if [ "$(cat build/tests/hgpu.cc.rc)" != 0 ]; then echo "hires_gpu: COMPILE FAIL"; cat build/tests/hgpu.cc.log; fail=1
else
  for d in auto vulkan d3d12 switch; do
    printf '%-6s ' "gpu-$d"
    ./build/tests/test_hires_gpu $d > build/tests/test_hires_gpu_$d.log 2>&1; rc=$?
    if [ $rc = 0 ]; then echo ok; elif [ $rc = 77 ]; then echo "skipped ($(head -1 build/tests/test_hires_gpu_$d.log))"
    else echo FAIL; grep FAIL build/tests/test_hires_gpu_$d.log | head; fail=1; fi
  done
fi
wait $SMOKE || { fail=1; echo "smoke: FAILED"; }
cat build/smoke/out.txt
wait $HIRES || { fail=1; echo "hires: FAILED"; }
cat build/hires/out.txt
wait $WIDE || { fail=1; echo "widescreen: FAILED"; }
cat build/widescreen/out.txt
wait $QUAL || { fail=1; echo "render quality: FAILED"; }
cat build/quality/out.txt
if [ "${SCENARIOS:-1}" != 0 ]; then
  wait $SCN || { fail=1; echo "scenarios: FAILED"; }
  cat build/scenarios/out.txt
  wait $SCNH || { fail=1; echo "scenarios hires: FAILED"; }
  cat build/scenarios/hires_out.txt
  wait $SCNS || { fail=1; echo "scenarios savestate: FAILED"; }
  cat build/scenarios/savestate_out.txt
fi
wait $MCARD || { fail=1; echo "memcard e2e: FAILED"; }
cat build/memcard.out
wait $MENU || { fail=1; echo "menu: FAILED"; }
cat build/menu.out
wait $SAVST || { fail=1; echo "savestate: FAILED"; }
cat build/savestate.out
wait $INPT || { fail=1; echo "input: FAILED"; }
cat build/input.out
wait $REPL || { fail=1; echo "replays: FAILED (a gameplay change? see AGENTS.md \"Replays\")"; }
cat build/replays.out
wait $NETP || { fail=1; echo "netplay: FAILED"; }
cat build/netplay.out
wait $NETW || { fail=1; echo "netplay (Mac <-> Windows under Wine): FAILED"; }
cat build/netplay_wine.out
echo "tests: $(( $(date +%s) - START ))s wall clock, $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
