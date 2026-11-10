# Shared helpers for tools/pr_check.sh and tools/run_tests.sh (bash, sourced; not run directly).
#
#   tl_renice "$@"        re-exec the calling script under `nice -n $NICE` (default 10) once;
#                         every child process inherits it. NICE=0 turns it off.
#   tl_default_jobs DIV   performance cores / DIV (at least 1)
#   tl_init DIR           step log directory; starts a load sampler (uptime every 10 s ->
#                         DIR/load.log) that stops when the script exits
#   tl_step NAME cmd...   run one step in the foreground, output to DIR/NAME.log; exit 77 = skipped.
#                         TL_SHOW=1 prints the whole log after the step (run_tests.sh), otherwise
#                         only the tail of a failing one. TL_DRY=1 only prints the command;
#                         TL_ONLY="name ..." runs only those steps (the others are left out).
#   tl_note NAME TEXT     a table row for something not run (reason in TEXT)
#   tl_summary TITLE      the step table, load at start / end / peak, wall clock; returns 0/1
# Steps run one after another: each one keeps at most $JOBS game processes busy itself, so the
# whole run never has more than that.

tl_renice() {
  if [ -z "$DESTREGA_NICED" ] && [ "${NICE:-10}" != 0 ] && command -v nice > /dev/null; then
    DESTREGA_NICED=1 exec nice -n "${NICE:-10}" "$0" "$@"
  fi
  export DESTREGA_NICED=1
}

tl_cores() {
  sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4
}

tl_default_jobs() { # divisor
  local n=$(( $(tl_cores) / ${1:-1} ))
  [ "$n" -ge 1 ] || n=1
  echo $n
}

tl_load() { # 1-minute load average
  uptime | sed -E 's/.*load averages?: *//; s/,/ /g' | awk '{print $1}'
}

tl_init() { # dir
  TL_OUT=$1
  mkdir -p "$TL_OUT"
  : > "$TL_OUT/steps.txt"
  TL_START=$(date +%s)
  TL_LOAD0=$(tl_load)
  echo "$(date +%H:%M:%S) $TL_LOAD0" > "$TL_OUT/load.log"
  ( while sleep 10; do echo "$(date +%H:%M:%S) $(tl_load)"; done ) >> "$TL_OUT/load.log" 2>/dev/null &
  TL_SAMPLER=$!
  trap 'kill $TL_SAMPLER 2>/dev/null' EXIT
}

tl_step() { # name cmd...
  local name=$1 t0 rc dt res
  shift
  if [ -n "$TL_ONLY" ]; then
    case " $TL_ONLY " in *" $name "*) ;; *) return 0 ;; esac
  fi
  if [ "${TL_DRY:-0}" = 1 ]; then
    printf '%-24s would run: %s\n' "$name" "$*"
    printf '%s\t%s\t-\n' "$name" "dry run" >> "$TL_OUT/steps.txt"
    return 0
  fi
  t0=$(date +%s)
  printf '%-24s ' "$name"
  "$@" > "$TL_OUT/$name.log" 2>&1
  rc=$?
  dt=$(( $(date +%s) - t0 ))
  case $rc in
    0) res=ok ;;
    77) res=skipped ;;
    *) res=FAIL ;;
  esac
  printf '%s\t%s\t%s\n' "$name" "$res" "$dt" >> "$TL_OUT/steps.txt"
  echo "$res (${dt}s, load $(tl_load))"
  if [ "${TL_SHOW:-0}" = 1 ]; then sed 's/^/    /' "$TL_OUT/$name.log"
  elif [ $res = FAIL ]; then tail -20 "$TL_OUT/$name.log" | sed 's/^/    /'; echo "    (log: $TL_OUT/$name.log)"; fi
  [ $res != FAIL ]
}

tl_note() { # name text
  printf '%s\t%s\t-\n' "$1" "$2" >> "$TL_OUT/steps.txt"
}

tl_summary() { # title
  local fail=0 peak load1
  load1=$(tl_load)
  echo "$(date +%H:%M:%S) $load1" >> "$TL_OUT/load.log"
  peak=$(awk '$2 > m { m = $2 } END { print m + 0 }' "$TL_OUT/load.log")
  echo
  echo "$1 summary"
  printf '  %-24s %-8s %s\n' step result time
  while IFS=$'\t' read -r n r t; do
    [ "$r" = FAIL ] && fail=1
    if [ "$t" = - ]; then printf '  %-24s %s\n' "$n" "$r"
    else printf '  %-24s %-8s %ss\n' "$n" "$r" "$t"; fi
  done < "$TL_OUT/steps.txt"
  echo "  load average: start $TL_LOAD0, end $load1, peak $peak (sampled every 10 s: $TL_OUT/load.log)"
  echo "  wall clock: $(( $(date +%s) - TL_START ))s; jobs: ${JOBS:-?}; exit status $fail ($([ $fail = 0 ] && echo PASS || echo FAIL))"
  return $fail
}
