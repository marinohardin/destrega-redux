#!/bin/sh
# Verification: module unit tests + headless boot-to-gameplay smoke test.
# Usage: tools/run_tests.sh   (from anywhere; needs a built build/cmake/destrega)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests build/smoke

CC="clang -std=c11 -O2 -Wall -Wextra -Iruntime"
$CC runtime/gte.c runtime/tests/test_gte.c -o build/tests/test_gte
$CC runtime/gpu.c runtime/tests/test_gpu.c -o build/tests/test_gpu
$CC runtime/gpu.c runtime/hires.c runtime/tests/test_hires.c -o build/tests/test_hires -lm
$CC runtime/spu.c runtime/xa.c runtime/tests/test_spu.c -o build/tests/test_spu
$CC runtime/mdec.c runtime/tests/test_mdec.c -o build/tests/test_mdec -lm
for t in gte gpu hires spu mdec; do
  printf '%-6s ' "$t"; ./build/tests/test_$t > build/tests/test_$t.log 2>&1 && echo ok || { echo FAIL; tail build/tests/test_$t.log; exit 1; }
done

# The smoke test runs from the user's data pack (as a normal launch would);
# import it from the disc image first if it doesn't exist yet.
case "$(uname -s)" in
  Darwin) PAK="$HOME/Library/Application Support/Destrega/destrega.pak" ;;
  MINGW*|MSYS*|CYGWIN*) PAK="$APPDATA/Destrega/destrega.pak" ;;
  *) PAK="${XDG_DATA_HOME:-$HOME/.local/share}/Destrega/destrega.pak" ;;
esac
if [ ! -f "$PAK" ]; then
  if [ -f Destrega/Destrega.cue ]; then ./build/cmake/destrega --import Destrega/Destrega.cue 2>&1 | tail -1
  else echo "smoke: no data pack at $PAK and no disc image to import"; exit 1; fi
fi

# Boot -> intro FMV -> title -> 1P battle -> fight, via scripted Start/Cross presses.
rm -f build/smoke/*.png
./build/cmake/destrega --headless --no-interp --frames 4000 --dump build/smoke --dump-every 1000 \
  --press 1200:8,1500:8,1800:8,2100:8,2400:4000,2700:4000,3000:4000,3300:4000,3600:4000 \
  > build/smoke/log.txt 2>&1
if grep -E "FATAL|dispatch: no recompiled|unhandled" build/smoke/log.txt; then
  echo "smoke: FAIL (see build/smoke/log.txt)"; exit 1
fi
grep -q "reached 4000 frames" build/smoke/log.txt || { echo "smoke: FAIL (did not reach frame 4000)"; exit 1; }
python3 - <<'EOF'
import struct, zlib, sys
d = open('build/smoke/frame_004000.png', 'rb').read()
w, h = struct.unpack('>II', d[16:24])
pos, idat = 8, b''
while pos < len(d):
    n, = struct.unpack('>I', d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b'IDAT': idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat)
lit = sum(1 for i in range(1, len(raw), 4) if raw[i] > 40)
frac = lit / (w * h)
print(f'smoke: frame 4000 is {w}x{h}, {frac:.0%} lit pixels')
sys.exit(0 if frac > 0.2 else 1)
EOF
echo "smoke: ok"

# Hi-res rendering: determinism across scales/resizes + S=1 rasterizer equivalence.
tools/test_hires_determinism.sh
