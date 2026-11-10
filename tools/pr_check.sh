#!/bin/bash
# PR check: the verification for every pull request. A few minutes, and the machine stays usable:
# everything runs under `nice -n 10` (NICE=0 turns it off) with at most PR_JOBS (default 3) game
# processes at once; the steps run one after another. The full tools/run_tests.sh is the release
# suite (AGENTS.md "Verification").
#
# Always:
#   build        cmake configure (-DDESTREGA_TESTS=ON) + build of build/cmake, -j PR_JOBS
#   unit         the CTest module unit tests (except hires_gpu*, see renderer)
#   smoke        tools/smoke_test.sh: boot to a fight, --no-interp --verify-overrides
#   quick_check  tools/quick_check.sh: two replays, GPU == software at 4x6 widescreen, and the
#                speed segment alone at the end (warnings only)
# Groups, added from the files changed since the merge base with origin/main (committed,
# uncommitted and untracked), each a small subset of the full suite:
#   net       runtime/net*, netplay*, portmap*, platform_net*, smooth*, and their test tools:
#             test_netplay.py lan wan desync ping menu_session (the ones that exist), test_portmap.py,
#             test_netplay_wine.py mac_hosts win_hosts (only with build/win/destrega.exe + CrossOver)
#   renderer  runtime/hires*, gpu.[ch], pgxp*, shaders/, widescreen*, overrides/widescreen.c,
#             overrides/char_mesh.c, analysis/widescreen_cull.txt, the renderer test scripts:
#             CTest hires_gpu* (GPU == software on random streams), test_hires_determinism.sh
#             CASES="s4x6 verify1 c4x6", test_render_quality.sh CASES="call169 cxbr43 glive"
#             (each plus its reference run)
#   menus     runtime/menu*, settings*, input*, controls_*, main.c, their test tools:
#             test_menu.sh, test_input.py, test_governor.py (if present)
#   core      every other runtime/*.c|h, runtime/overrides/, recomp/, analysis/overrides.txt,
#             extra_seeds.txt, rea_*.json, overlays/, CMakeLists.txt, scenarios / replays tools:
#             4 scenarios (stage fight, story, options, practice), 3 replays, test_savestate.sh
#   Anything else (docs, other tools, runtime/tests/, runtime/win/) adds nothing.
# Usage: tools/pr_check.sh [--all-groups] [--group NAME]... [--base REF] [--dry-run]
#   --all-groups   every group's subset (still light);  --group NAME   force a group (repeatable)
#   --base REF     compare against REF instead of origin/main;  --dry-run   show what would run
# Env: PR_JOBS (default 3), NICE (default 10). Logs: build/pr/ (load samples in build/pr/load.log).
cd "$(dirname "$0")/.." || exit 2
. tools/testlib.sh
tl_renice "$@"

PR_JOBS=${PR_JOBS:-3}
export JOBS=$PR_JOBS
mkdir -p build/tmp
export TMPDIR=$PWD/build/tmp
GROUPS_ALL="net renderer menus core"
FORCE=""
ALLG=0
BASE_REF=origin/main
while [ $# -gt 0 ]; do
  case $1 in
    --all-groups) ALLG=1 ;;
    --group) shift; case " $GROUPS_ALL " in *" $1 "*) FORCE="$FORCE $1" ;; *) echo "pr_check: unknown group '$1' ($GROUPS_ALL)"; exit 2 ;; esac ;;
    --base) shift; BASE_REF=$1 ;;
    --dry-run) export TL_DRY=1 ;;
    -h|--help) sed -n '2,/^cd /p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "pr_check: unknown option '$1' (--help)"; exit 2 ;;
  esac
  shift
done

# ---------------------------------------------------------------- which groups
want=""
add() { case " $want " in *" $1 "*) ;; *) want="$want $1" ;; esac; }
classify() { # path -> group (or nothing)
  case $1 in
    runtime/net*|runtime/netplay*|runtime/portmap*|runtime/platform_net*|runtime/smooth*) echo net ;;
    tools/test_netplay*.py|tools/test_portmap.py|tools/fake_router.py) echo net ;;
    runtime/hires*|runtime/gpu.[ch]|runtime/pgxp*|runtime/shaders/*|runtime/widescreen*) echo renderer ;;
    runtime/overrides/widescreen.c|runtime/overrides/char_mesh.c|analysis/widescreen_cull.txt) echo renderer ;;
    tools/test_hires_determinism.sh|tools/test_render_quality.sh|tools/test_widescreen.sh|tools/check_ws_*.py) echo renderer ;;
    tools/gen_spirv.py|tools/gen_dxil.py) echo renderer ;;
    runtime/menu*|runtime/settings*|runtime/input*|runtime/controls_*|runtime/main.c) echo menus ;;
    tools/test_menu.sh|tools/test_input.py|tools/test_governor.py) echo menus ;;
    runtime/tests/*|runtime/win/*) ;;
    runtime/*.c|runtime/*.h|runtime/overrides/*|recomp/*) echo core ;;
    analysis/overrides.txt|analysis/extra_seeds.txt|analysis/rea_*.json|analysis/overlays/*|CMakeLists.txt) echo core ;;
    tools/scenarios.py|tools/scenario_expect.json|tools/replays.py|tools/replays/*|tools/test_savestate.sh|tools/fight_script.py) echo core ;;
  esac
}
BASE=$(git merge-base HEAD "$BASE_REF" 2>/dev/null)
if [ $ALLG = 1 ]; then
  for g in $GROUPS_ALL; do add $g; done
  why="--all-groups"
elif [ -z "$BASE" ]; then
  echo "pr_check: no merge base with $BASE_REF (git fetch?): running every group"
  for g in $GROUPS_ALL; do add $g; done
  why="no merge base"
else
  CHANGED=$( { git diff --name-only "$BASE"; git ls-files --others --exclude-standard; } | sort -u)
  nf=0
  for f in $CHANGED; do
    nf=$((nf + 1))
    g=$(classify "$f")
    [ -n "$g" ] && add $g
  done
  why="$nf files changed since $(git rev-parse --short "$BASE") ($BASE_REF)"
fi
for g in $FORCE; do add $g; done
# keep the canonical order
sel=""
for g in $GROUPS_ALL; do case " $want " in *" $g "*) sel="$sel $g" ;; esac; done
echo "pr_check: $why; groups:${sel:- none (always part only)}${FORCE:+ (forced:$FORCE)}; PR_JOBS=$PR_JOBS, nice $(ps -o nice= -p $$ | tr -d ' ')"
if [ "$TL_DRY" = 1 ] && [ -n "$CHANGED" ]; then
  for f in $CHANGED; do printf '  %-60s %s\n' "$f" "$(classify "$f")"; done
fi

half=$(( PR_JOBS / 2 )); [ $half -ge 1 ] || half=1
less1=$(( PR_JOBS - 1 )); [ $less1 -ge 1 ] || less1=1

tl_init build/pr

# ---------------------------------------------------------------- always
do_build() {
  if [ -f build/cmake/CMakeCache.txt ]; then cmake -S . -B build/cmake -DDESTREGA_TESTS=ON || return 1
  else cmake -S . -B build/cmake -G Ninja -DDESTREGA_TESTS=ON || return 1; fi
  cmake --build build/cmake -j "$PR_JOBS"
}
if ! tl_step build do_build; then
  tl_summary "pr_check"; exit 1
fi
tl_step unit ctest --test-dir build/cmake -j "$PR_JOBS" -E hires_gpu --output-on-failure
tl_step smoke tools/smoke_test.sh
tl_step quick_check tools/quick_check.sh

# ---------------------------------------------------------------- groups
for g in $sel; do
  case $g in
  net)
    avail=" $(python3 tools/test_netplay.py --list | awk '{print $1}' | tr '\n' ' ') "
    cases=""
    for c in lan wan desync ping menu_session; do case $avail in *" $c "*) cases="$cases $c" ;; esac; done
    # a case is two games (then an offline reference run): PR_JOBS / 2 cases at a time
    tl_step net:netplay python3 tools/test_netplay.py -j $half $cases
    tl_step net:portmap python3 tools/test_portmap.py -j $less1
    CROSSOVER="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine"
    if [ -f build/win/destrega.exe ] && [ -x "$CROSSOVER" ]; then
      tl_step net:netplay_wine env WINE="$CROSSOVER" WINE_ARGS="--bottle destrega-test --no-gui" \
        python3 tools/test_netplay_wine.py mac_hosts win_hosts
    else
      tl_note net:netplay_wine "skipped (needs build/win/destrega.exe and CrossOver)"
    fi
    ;;
  renderer)
    tl_step renderer:gpu_unit ctest --test-dir build/cmake -j "$PR_JOBS" -R hires_gpu --output-on-failure
    tl_step renderer:hires env CASES="s4x6 verify1 c4x6" tools/test_hires_determinism.sh
    tl_step renderer:quality env CASES="call169 cxbr43 glive" tools/test_render_quality.sh
    ;;
  menus)
    if tl_step menus:menu tools/test_menu.sh; then rm -rf build/menu; fi   # disc image + packs: ~1 GB
    tl_step menus:input python3 tools/test_input.py
    if [ -f tools/test_governor.py ]; then tl_step menus:governor python3 tools/test_governor.py
    else tl_note menus:governor "not present (tools/test_governor.py)"; fi
    ;;
  core)
    tl_step core:scenarios python3 tools/scenarios.py run vs_hills story option_game practice -j "$PR_JOBS" \
      --out build/pr/scenarios
    tl_step core:replays python3 tools/replays.py run -j "$PR_JOBS" practice_gradd opts_handicap_recover vs_falma_reyus_cave
    tl_step core:savestate tools/test_savestate.sh
    ;;
  esac
done

tl_summary "pr_check"
