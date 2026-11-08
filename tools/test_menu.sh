#!/bin/sh
# File-menu actions, headless, via the same handlers the native menu items call (--menu-action).
# Everything happens in build/menu/ with --data-dir/--memcard1 scratch paths; the user's real data
# folder, pack and cards are never written (the pack is only read, to rebuild a disc image when
# Destrega/Destrega.cue is not around).
#   load     : Load Game Image from a .cue (and from a raw 2352-byte .iso) into an empty data dir ->
#              the process restarts on the new pack -> same state hashes as a plain run from that pack
#   reject   : wrong type, 2048-byte .iso, junk image, truncated copies, missing file, a data pack:
#              error reported, game keeps running, no pack written
#   cards    : import (valid/invalid, with timestamped backup), export, eject/insert, delete, and a
#              swapped-in card's save really is loaded by the game (ROUND COUNT option byte)
set -e
cd "$(dirname "$0")/.."
D=build/menu
BIN=./build/cmake/destrega
rm -rf $D
mkdir -p $D
fail() { echo "menu: FAIL: $*"; exit 1; }

# a raw disc image: the real one if present, else rebuilt from the user's data pack (read-only)
if [ -f Destrega/Destrega.cue ]; then
  IMG=$(pwd)/Destrega/Destrega.cue
  BINF=$(pwd)/Destrega/Destrega.bin
else
  case "$(uname -s)" in
    Darwin) PAK="$HOME/Library/Application Support/Destrega/destrega.pak" ;;
    *) PAK="${XDG_DATA_HOME:-$HOME/.local/share}/Destrega/destrega.pak" ;;
  esac
  [ -f "$PAK" ] || { echo "menu: need Destrega/Destrega.cue or an imported data pack"; exit 1; }
  mkdir -p $D/img
  python3 tools/pak_to_bin.py "$PAK" $D/img/Destrega > /dev/null
  IMG=$(pwd)/$D/img/Destrega.cue
  BINF=$(pwd)/$D/img/Destrega.bin
fi

logok() { # log pattern
  grep -q -- "$2" "$1" || fail "$1: expected '$2'"
}
nolog() {
  if grep -q -- "$2" "$1"; then fail "$1: unexpected '$2'"; fi
}
clean() { if grep -E "FATAL|dispatch: no recompiled|unhandled" "$1"; then fail "$1 has errors"; fi; }
peek() { python3 -c "import sys; print(open(sys.argv[1],'rb').read()[int(sys.argv[2])])" "$1" "$2"; }

# ---------------------------------------------------------------- load game image
# Run 1 plays from the loose .cue; at frame 5 the menu action imports the (same) image into the empty
# data dir and the process re-execs without the positional image, so run 2 plays from the new pack.
mkdir -p $D/a $D/a2
ln -s "$BINF" $D/a2/Destrega.iso     # a raw 2352-byte image named .iso is accepted
( $BIN --headless --no-interp --data-dir $D/a --frames 1700 --press 1200:8 --dump $D/a --dump-every 1700 \
    --state-hash $D/a.hash --menu-action 5:load="$IMG" "$IMG" > $D/a.log 2>&1 || fail "load run failed (see $D/a.log)" ) &
LA=$!
( $BIN --headless --no-interp --data-dir $D/a2 --frames 300 \
    --menu-action 5:load=$D/a2/Destrega.iso "$IMG" > $D/a2.log 2>&1 || fail "iso load run failed (see $D/a2.log)" ) &
LB=$!
wait $LA || fail "load run"
wait $LB || fail "iso load run"
for l in a a2; do
  logok $D/$l.log "menu: load OK"
  logok $D/$l.log "menu: restarting on the new data pack"
  logok $D/$l.log "menu: restarted on the new data pack"
  clean $D/$l.log
  [ -f $D/$l/destrega.pak ] || fail "$l: no data pack written"
done
logok $D/a.log "reached 1700 frames"
logok $D/a2.log "reached 300 frames"
# the restarted process is a clean machine: identical to a plain run from the new pack
$BIN --headless --no-interp --data-dir $D/a --frames 1700 --press 1200:8 --state-hash $D/a_ref.hash > $D/a_ref.log 2>&1 \
  || fail "reference run failed"
cmp -s $D/a.hash $D/a_ref.hash || fail "restarted run differs from a plain run on the same pack"
python3 - $D/a/frame_001700.png <<'EOF' || fail "no picture after restart"
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('>II', d[16:24])
pos, idat = 8, b''
while pos < len(d):
    n, = struct.unpack('>I', d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b'IDAT': idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat)
lit = sum(1 for i in range(1, len(raw), 4) if raw[i] > 40) / (w * h)
print(f'menu: after loading an image the game restarted and shows a {w}x{h} frame, {lit:.0%} lit')
sys.exit(0 if lit > 0.05 else 1)
EOF
echo "menu: load game image (.cue and raw .iso) -> restart on the new pack ok"

# ---------------------------------------------------------------- rejection paths
mkdir -p $D/b
echo "hello" > $D/b/notes.txt
python3 - $D/b "$BINF" <<'EOF'
import os, sys
d, binf = sys.argv[1], sys.argv[2]
open(d + '/fake2048.iso', 'wb').write(bytes(2048 * 400))                  # 2048-byte-sector "iso"
sync = b'\0' + b'\xff' * 10 + b'\0'
open(d + '/junk.bin', 'wb').write((sync + bytes(2340)) * 40)                # raw sectors, not Destrega
size = os.path.getsize(binf)
with open(binf, 'rb') as f:
    open(d + '/trunc_sector.bin', 'wb').write(f.read(2352 * 30000))        # cut at a sector boundary
    f.seek(0)
    open(d + '/trunc_odd.bin', 'wb').write(f.read(2352 * 30000 + 1000))    # cut mid-sector
open(d + '/nobin.cue', 'w').write('FILE "missing.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
open(d + '/empty.bin', 'wb').close()
EOF
$BIN --headless --no-interp --data-dir $D/b --frames 120 \
  --menu-action 5:load=$D/b/notes.txt --menu-action 10:load=$D/b/fake2048.iso --menu-action 15:load=$D/b/junk.bin \
  --menu-action 20:load=$D/b/trunc_sector.bin --menu-action 25:load=$D/b/trunc_odd.bin \
  --menu-action 30:load=$D/b/nobin.cue --menu-action 35:load=$D/b/missing.cue --menu-action 40:load=$D/b/empty.bin \
  --menu-action 45:load="$HOME/Library/Application Support/Destrega/destrega.pak" \
  --menu-action 50:load="$IMG" "$IMG" > $D/b.log 2>&1 || fail "reject run failed (see $D/b.log)"
echo "menu: (rejection log)"; grep "menu: ERROR" $D/b.log | sed 's/^/    /'
logok $D/b.log "ERROR: Unsupported file type"
logok $D/b.log "ERROR: This looks like a 2048-byte-sector .iso"
logok $D/b.log "ERROR: This disc image is not Destrega"
logok $D/b.log "ERROR: This disc image is incomplete"
logok $D/b.log "ERROR: That disc image is damaged or truncated"
logok $D/b.log "ERROR: Could not open the disc image"
logok $D/b.log "ERROR: That file is empty"
[ "$(grep -c 'menu: load REJECTED' $D/b.log)" = 9 ] || fail "expected the rejections to be reported"
# the last action (a good image) is accepted: it restarts; everything before kept the game running
logok $D/b.log "menu: load OK"
clean $D/b.log
echo "menu: rejected wrong type / 2048-byte iso / junk / truncated / missing / empty images; game kept running"

# ---------------------------------------------------------------- memory card actions
mkdir -p $D/c
# a card holding a Destrega save (round count 3), made like tools/test_memcard.sh does
SAVE_SCRIPT=1200:8,1400:10,1450:4000,1550:4000,1620:40,1650:20,1700:2000,1780:40,1800:40,1820:40,1840:40,1870:4000,1960:40,2000:4000
$BIN --headless --no-interp --frames 2600 --memcard1 $D/c/saved.mcd --press $SAVE_SCRIPT "$IMG" > $D/c_save.log 2>&1 \
  || fail "could not make a card with a save"
python3 - $D/c/saved.mcd <<'EOF' || fail "saved card content"
import sys
d = open(sys.argv[1], 'rb').read()
assert d[0:2] == b'MC' and d[128] == 0x51 and d[0x2208] == 2
EOF
OPT_ADDR=$((0xbee64 + 8))
LOAD_SCRIPT=1200:8

# (a) a card swapped in while the game runs is the one it loads its options from (round count 3 -> index 2)
cp /dev/null $D/c/slot1.mcd; rm $D/c/slot1.mcd
$BIN --headless --no-interp --data-dir $D/c --memcard1 $D/c/slot1.mcd --frames 1700 --press $LOAD_SCRIPT \
  --ram-dump $D/c/swap.ram --menu-action 100:card-import=1:$D/c/saved.mcd "$IMG" > $D/c/swap.log 2>&1 || fail "swap run"
logok $D/c/swap.log "menu: card-import OK slot 1"
logok $D/c/swap.log "menu: confirm"
cmp -s $D/c/slot1.mcd $D/c/saved.mcd || fail "imported card differs from the source"
BK=$(ls $D/c/slot1-backup-*.mcd 2>/dev/null | head -1)
[ -n "$BK" ] || fail "no timestamped backup of the replaced card"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); assert len(d)==131072 and d[:2]==b'MC' and d[128]==0xa0" "$BK" \
  || fail "backup is not the old (blank) card"
[ "$(peek $D/c/swap.ram $OPT_ADDR)" = 2 ] || fail "game did not load the imported card's save (round count $(peek $D/c/swap.ram $OPT_ADDR))"
clean $D/c/swap.log
echo "menu: imported card is loaded by the game (round count index 2), old card backed up as $(basename "$BK")"

# (b) eject / insert (card 2 starts empty) and the card-changed flag: ejecting slot 1 then inserting the
#     saved card loads it too
$BIN --headless --no-interp --data-dir $D/c --memcard1 $D/c/blank1.mcd --frames 1700 --press $LOAD_SCRIPT \
  --ram-dump $D/c/ins.ram --menu-action 50:card-eject=1 --menu-action 60:card-insert=1:$D/c/saved.mcd \
  --menu-action 70:card-insert=2:$D/c/slot2.mcd --menu-action 75:card-eject=2 --menu-action 80:card-eject=2 \
  --menu-action 85:card-insert=1 "$IMG" > $D/c/ins.log 2>&1 || fail "insert run"
logok $D/c/ins.log "menu: card-eject OK slot 1"
logok $D/c/ins.log "menu: card-insert OK slot 1"
logok $D/c/ins.log "menu: card-insert OK slot 2"
logok $D/c/ins.log "menu: card-eject OK slot 2"
logok $D/c/ins.log "ERROR: No card is inserted"            # second eject of slot 2
logok $D/c/ins.log "ERROR: A card is already inserted"      # insert into occupied slot 1
[ "$(peek $D/c/ins.ram $OPT_ADDR)" = 2 ] || fail "inserted card's save not loaded"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); assert len(d)==131072 and d[:2]==b'MC'" $D/c/slot2.mcd \
  || fail "card inserted in slot 2 was not created blank"
echo "menu: eject / insert per slot ok (slot 2 starts empty, a missing file becomes a blank card)"

# (c) export, invalid imports, delete
cp $D/c/saved.mcd $D/c/work.mcd
head -c 4 /dev/zero > $D/c/tiny.mcd
head -c 131072 /dev/zero > $D/c/nomc.mcd
python3 -c "
d=bytearray(open('$D/c/saved.mcd','rb').read()); d[127]^=1; open('$D/c/badsum.mcd','wb').write(d)"
$BIN --headless --no-interp --data-dir $D/c --memcard1 $D/c/work.mcd --frames 200 \
  --menu-action 10:card-export=1:$D/c/exported.mcd \
  --menu-action 20:card-import=1:$D/c/tiny.mcd --menu-action 21:card-import=1:$D/c/nomc.mcd \
  --menu-action 22:card-import=1:$D/c/badsum.mcd --menu-action 23:card-import=1:$D/c/nothere.mcd \
  --menu-action 24:card-import=1:$D/c/work.mcd \
  --menu-action 30:card-delete=1 --menu-action 31:card-delete=1 --menu-action 40:card-export=2:$D/c/e2.mcd \
  --menu-action 50:card-import=3:$D/c/saved.mcd \
  --menu-action 60:reveal-cards --menu-action 61:reveal-game \
  "$IMG" > $D/c/misc.log 2>&1 || fail "misc card run"
cmp -s $D/c/exported.mcd $D/c/saved.mcd || fail "exported card differs from the card in the slot"
[ "$(grep -c 'menu: card-import REJECTED' $D/c/misc.log)" = 4 ] || fail "expected 4 rejected card imports"
logok $D/c/misc.log "ERROR: That is not a raw 128 KiB memory card image"
logok $D/c/misc.log "ERROR: That file is 128 KiB but has no memory card header"
logok $D/c/misc.log "ERROR: That memory card image has a damaged header"
logok $D/c/misc.log "ERROR: Could not open that file"
logok $D/c/misc.log "ERROR: That file is already the card in this slot"
logok $D/c/misc.log "menu: card-delete OK slot 1 (BASLUS-00732DESTREGA)"
logok $D/c/misc.log "ERROR: This card has no Destrega save"       # second delete
logok $D/c/misc.log "ERROR: No card is inserted in that slot"     # export from empty slot 2
logok $D/c/misc.log "ERROR: Memory card slot must be 1 or 2"
logok $D/c/misc.log "menu: show memory cards in file manager: $D/c"
logok $D/c/misc.log "menu: show game files in file manager"
[ ! -e $D/c/e2.mcd ] || fail "exported an empty slot"
python3 - $D/c/work.mcd <<'EOF' || fail "card after delete"
import functools, sys
d = open(sys.argv[1], 'rb').read()
e = d[128:256]
assert d[:2] == b'MC' and e[0] == 0xa1, 'deleted entry state %x' % e[0]
assert e[127] == functools.reduce(lambda a, b: a ^ b, e[:127]), 'directory checksum'
EOF
# the failed imports left the card file untouched (still the saved card minus the deleted save)
clean $D/c/misc.log
echo "menu: export, invalid-card rejection, delete ok"

# (d) headless safety: without --data-dir or --memcardN a card import has nowhere to go
$BIN --headless --no-interp --frames 60 --menu-action 10:card-import=1:$D/c/saved.mcd "$IMG" > $D/c/safe.log 2>&1 || fail "safety run"
logok $D/c/safe.log "ERROR: No card location"
nolog $D/c/safe.log "card-import OK"
echo "menu: ok"
