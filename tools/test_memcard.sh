#!/bin/sh
# Memory card end-to-end test (also run by tools/run_tests.sh), headless, from empty cards in build/memcard/:
#   1. save:   change ROUND COUNT 2 -> 3 in OPTION > GAME OPTION, then OPTION > MEMORY CARD > SAVE
#              on a fresh blank card; the card must now hold the BASLUS-00732DESTREGA save file.
#   2. load:   boot with that card; the game reads its options at startup, so the live option block
#              (0x800bee64, round-count index at +8) must be 2 instead of 0 (nothing loaded).
#   3. control: a blank card (and no card at all) leaves it 0.
#   4. determinism: two identical runs from the same starting card give identical --state-hash files
#              and identical card files.
set -e
cd "$(dirname "$0")/.."
D=build/memcard
rm -rf $D
mkdir -p $D
BIN=./build/cmake/destrega
OPT_ADDR=$((0xbee64 + 8))

# boot -> title (Start), Up to OPTION, Cross; GAME OPTION: Cross, Down, Right (round count), Circle back;
# OPTION: Down x4 to MEMORY CARD, Cross; menu: Down to SAVE, Cross.
SAVE_SCRIPT=1200:8,1400:10,1450:4000,1550:4000,1620:40,1650:20,1700:2000,1780:40,1800:40,1820:40,1840:40,1870:4000,1960:40,2000:4000
LOAD_SCRIPT=1200:8

peek() { python3 -c "import sys; print(open(sys.argv[1],'rb').read()[int(sys.argv[2])])" "$1" "$2"; }
fail() { echo "memcard: FAIL: $*"; exit 1; }
run() { # name card frames script
  $BIN --headless --no-interp --frames "$3" --memcard1 "$2" --press "$4" --state-hash $D/$1.hash --ram-dump $D/$1.ram \
    > $D/$1.log 2>&1 || fail "$1 run failed (see $D/$1.log)"
  if grep -E "FATAL|dispatch: no recompiled|unhandled" $D/$1.log; then fail "$1 log has errors"; fi
}

run save1 $D/s1.mcd 2600 $SAVE_SCRIPT
run save2 $D/s2.mcd 2600 $SAVE_SCRIPT
cmp -s $D/s1.mcd $D/s2.mcd || fail "two identical save runs produced different card files"
cmp -s $D/save1.hash $D/save2.hash || fail "two identical save runs produced different state hashes"
[ "$(wc -c < $D/s1.mcd | tr -d ' ')" = 131072 ] || fail "card is not 128 KiB"
python3 - $D/s1.mcd <<'EOF' || fail "saved card content"
import sys
d = open(sys.argv[1], 'rb').read()
assert d[0:2] == b'MC', 'header'
e = d[128:256]
assert e[0] == 0x51 and e[10:30].rstrip(b'\0') == b'BASLUS-00732DESTREGA', 'directory entry'
assert int.from_bytes(e[4:8], 'little') == 8192, 'size'
assert d[127 + 128] == __import__('functools').reduce(lambda a, b: a ^ b, e[:127]), 'directory checksum'
assert d[0x2000:0x2002] == b'SC', 'save header'
assert d[0x2208] == 2, 'round count index (expected 2 = 3 rounds)'
EOF
echo "memcard: saved BASLUS-00732DESTREGA with round count 3"

cp $D/s1.mcd $D/l1.mcd
cp $D/s1.mcd $D/l2.mcd
run load1 $D/l1.mcd 1700 $LOAD_SCRIPT
run load2 $D/l2.mcd 1700 $LOAD_SCRIPT
[ "$(peek $D/load1.ram $OPT_ADDR)" = 2 ] || fail "load did not restore round count (got $(peek $D/load1.ram $OPT_ADDR))"
cmp -s $D/load1.hash $D/load2.hash || fail "two identical load runs produced different state hashes"
cmp -s $D/s1.mcd $D/l1.mcd || fail "loading modified the card"
echo "memcard: loaded options from the card (round count index 2)"

run blank $D/blank.mcd 1700 $LOAD_SCRIPT
[ "$(peek $D/blank.ram $OPT_ADDR)" = 0 ] || fail "blank card changed the options"
$BIN --headless --no-interp --frames 1700 --press $LOAD_SCRIPT --ram-dump $D/none.ram > $D/none.log 2>&1 || fail "no-card run"
[ "$(peek $D/none.ram $OPT_ADDR)" = 0 ] || fail "no-card run changed the options"
echo "memcard: ok"
