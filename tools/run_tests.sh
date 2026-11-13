#!/bin/bash
# The full verification suite, run before a release (tools/pr_check.sh is the check for every PR;
# AGENTS.md "Verification"): module unit tests, headless boot-to-gameplay smoke test, the hi-res
# determinism/equivalence runs, the --widescreen gameplay-equivalence runs, render quality, the
# scenario library, memory cards, menus, save states, input, replays, windowed determinism,
# netplay (also against the Windows build under Wine) and port mapping.
#
# Gentle on the machine: the whole run is under `nice -n 10` (NICE=0 turns it off) and the steps run
# one after another, each with at most JOBS game processes at once (default: half the performance
# cores; it used to start everything together, load ~450 on a 10-core machine for an hour).
# Usage: tools/run_tests.sh   (from anywhere; needs a built build/cmake/destrega)
# Env: JOBS, NICE, SCENARIOS=0 (skip the scenario library), DRY_RUN=1 (list the steps only),
# ONLY="unit smoke ..." (run only those steps; names as in the summary table).
# Check `uptime` first and never run two heavy test runs at once.
cd "$(dirname "$0")/.." || exit 2
. tools/testlib.sh
tl_renice "$@"
JOBS=${JOBS:-$(tl_default_jobs 2)}
export JOBS
[ "${DRY_RUN:-0}" = 1 ] && export TL_DRY=1
export TL_ONLY="$ONLY"
mkdir -p build/tests build/smoke build/hires build/widescreen build/quality build/tmp
export TMPDIR=$PWD/build/tmp
half=$(( JOBS / 2 )); [ $half -ge 1 ] || half=1
less1=$(( JOBS - 1 )); [ $less1 -ge 1 ] || less1=1
echo "run_tests: JOBS=$JOBS, nice $(ps -o nice= -p $$ | tr -d ' '), load $(tl_load)"
tl_init build/run_tests
export TL_SHOW=1   # print every step's whole output, as before

# 1. module unit tests: compile (JOBS at a time), run, report in order
unit_tests() {
  local CC="clang -std=c11 -O2 -Wall -Wextra -pthread -Iruntime" SDLF t fail=0
  SDLF=$(pkg-config --cflags --libs sdl3 2>/dev/null || echo "-I/opt/homebrew/include -L/opt/homebrew/lib -lSDL3")
  rm -f build/tests/*.cc.rc
  # (only our own compile jobs are waited for and counted: the load sampler is a job too)
  local pids=() p
  cc_job() { # name cmd...
    local n=$1 running; shift
    while :; do
      running=0
      for p in "${pids[@]}"; do kill -0 $p 2>/dev/null && running=$((running + 1)); done
      [ $running -lt "$JOBS" ] && break
      sleep 0.2
    done
    ( "$@" > build/tests/$n.cc.log 2>&1; echo $? > build/tests/$n.cc.rc ) &
    pids+=($!)
  }
  cc_job gte $CC runtime/gte.c runtime/pgxp.c runtime/tests/test_gte.c -o build/tests/test_gte
  cc_job gpu $CC runtime/gpu.c runtime/pgxp.c runtime/tests/test_gpu.c -o build/tests/test_gpu
  cc_job hires $CC runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c runtime/tests/test_hires.c -o build/tests/test_hires -lm
  cc_job pgxp $CC runtime/gte.c runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c runtime/tests/test_pgxp.c -o build/tests/test_pgxp -lm
  cc_job spu $CC runtime/spu.c runtime/xa.c runtime/tests/test_spu.c -o build/tests/test_spu
  cc_job mdec $CC runtime/mdec.c runtime/tests/test_mdec.c -o build/tests/test_mdec -lm
  cc_job memcard $CC runtime/memcard.c runtime/platform.c runtime/tests/test_memcard.c -o build/tests/test_memcard
  cc_job portmap $CC runtime/portmap.c runtime/settings.c runtime/platform.c runtime/platform_net.c runtime/tests/test_portmap.c -o build/tests/test_portmap
  cc_job input $CC runtime/input.c runtime/settings.c runtime/platform.c runtime/tests/test_input.c -o build/tests/test_input $SDLF -lm
  cc_job hgpu $CC runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/pgxp.c runtime/platform.c runtime/tests/test_hires_gpu.c -o build/tests/test_hires_gpu $SDLF -lm
  for p in "${pids[@]}"; do wait $p; done
  for t in gte gpu hires pgxp spu mdec memcard input portmap; do
    printf '%-6s ' "$t"
    if [ "$(cat build/tests/$t.cc.rc)" != 0 ]; then echo "COMPILE FAIL"; cat build/tests/$t.cc.log; fail=1; continue; fi
    if ./build/tests/test_$t > build/tests/test_$t.log 2>&1; then echo ok; else echo FAIL; tail build/tests/test_$t.log; fail=1; fi
  done
  # GPU backend vs software on random command streams: the default API (Metal on macOS), Vulkan
  # (MoltenVK here, if the Vulkan loader is installed), Direct3D 12 (Windows only) and the live
  # API switch (shutdown + restart on every available API); exit 77 = no such device, skipped
  if [ "$(cat build/tests/hgpu.cc.rc)" != 0 ]; then echo "hires_gpu: COMPILE FAIL"; cat build/tests/hgpu.cc.log; fail=1
  else
    local d rc
    for d in auto vulkan d3d12 switch; do
      printf '%-6s ' "gpu-$d"
      ./build/tests/test_hires_gpu $d > build/tests/test_hires_gpu_$d.log 2>&1; rc=$?
      if [ $rc = 0 ]; then echo ok; elif [ $rc = 77 ]; then echo "skipped ($(head -1 build/tests/test_hires_gpu_$d.log))"
      else echo FAIL; grep FAIL build/tests/test_hires_gpu_$d.log | head; fail=1; fi
    done
  fi
  return $fail
}
tl_step unit unit_tests

# 2. headless boot -> fight smoke test, hi-res matrix, widescreen, render-quality modes (texture
#    filter, full colour, SSAA: state hash + software == GPU)
tl_step smoke tools/smoke_test.sh
tl_step hires tools/test_hires_determinism.sh
tl_step widescreen tools/test_widescreen.sh
tl_step quality tools/test_render_quality.sh
# 3. scenario library: every mode / stage / character path, --no-interp (SCENARIOS=0 to skip), the
#    hi-res determinism + 1x1 equivalence checks on the stage subset, and every scenario with
#    periodic save-state rewinds
JOBS_SC=${JOBS_SC:-$JOBS}
if [ "${SCENARIOS:-1}" != 0 ]; then
  mkdir -p build/scenarios
  tl_step scenarios python3 tools/scenarios.py run --all -j "$JOBS_SC"
  tl_step scenarios_hires python3 tools/scenarios.py hires -j "$JOBS_SC"
  tl_step scenarios_savestate python3 tools/scenarios.py savestate -j "$JOBS_SC"
else
  tl_note scenarios "skipped (SCENARIOS=0)"
fi
tl_step memcard tools/test_memcard.sh
tl_step menu tools/test_menu.sh
tl_step savestate tools/test_savestate.sh
tl_step bgm_loop python3 tools/test_bgm_loop.py -j "$JOBS"
# input layer + settings file: custom bindings and synthetic host events == the --press run
tl_step input python3 tools/test_input.py
tl_step overlay_menu python3 tools/test_overlay_menu.py
# View > Resolution and the scale governor (fake load: steps down / up, back-off, fixed choices)
tl_step governor python3 tools/test_governor.py -j "$JOBS"
# gameplay regression replays (tools/replays/*.dtr.gz): every frame's gameplay fields must match
# (a failure means a gameplay change: see AGENTS.md "Replays")
tl_step replays python3 tools/replays.py run -j "$JOBS"
# windowed play is deterministic: two windowed runs and a headless one hash identically
tl_step windowed tools/test_windowed_determinism.sh
# rollback netplay: host + join over loopback with simulated latency / loss, against an offline
# run; a case is two games, so JOBS / 2 cases at a time
tl_step netplay python3 tools/test_netplay.py -j $half
# the same between the native build and the Windows build under Wine (skips itself without Wine /
# build/win/destrega.exe); one case at a time
tl_step netplay_wine python3 tools/test_netplay_wine.py
# the host's automatic port forwarding against a fake router on 127.0.0.1 (tools/fake_router.py;
# never a real router): PCP, NAT-PMP, UPnP, refusals, CGNAT, no answer, cleanup, menu restart
tl_step portmap python3 tools/test_portmap.py -j $less1

tl_summary "run_tests"
