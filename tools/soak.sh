#!/bin/sh
# Soak run with random inputs and the interpreter / override checks enabled.
# usage: [VERIFY=0] tools/soak.sh SEED [FRAMES]   (log in build/soak/log<SEED>.txt)
#   VERIFY=0 skips --verify-overrides (which copies all of RAM on every override
#   call and is much slower); the interpreter is still fatal.
cd "$(dirname "$0")/.."
seed=$1
frames=${2:-60000}
verify=--verify-overrides
[ "${VERIFY:-1}" = 0 ] && verify=
mkdir -p build/soak
python3 -I tools/mash_inputs.py "$seed" "$frames" > build/soak/press$seed.txt
"${BIN:-./build/cmake/destrega}" --headless --no-interp $verify --frames "$frames" \
  --press "$(cat build/soak/press$seed.txt)" > build/soak/log$seed.txt 2>&1
echo "seed $seed exit $?: $(grep -E '^override|^FATAL' build/soak/log$seed.txt | tr '\n' ' ')"
