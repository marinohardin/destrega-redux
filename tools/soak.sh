#!/bin/sh
# Soak run with random inputs and the interpreter / override checks enabled.
# usage: tools/soak.sh SEED [FRAMES]   (log in build/soak/log<SEED>.txt)
cd "$(dirname "$0")/.."
seed=$1
frames=${2:-60000}
mkdir -p build/soak
python3 -I tools/mash_inputs.py "$seed" "$frames" > build/soak/press$seed.txt
./build/cmake/destrega --headless --no-interp --verify-overrides --frames "$frames" \
  --press "$(cat build/soak/press$seed.txt)" > build/soak/log$seed.txt 2>&1
echo "seed $seed exit $?: $(grep '^override' build/soak/log$seed.txt | tr '\n' ' ')"
