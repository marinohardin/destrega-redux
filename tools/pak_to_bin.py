#!/usr/bin/env python3
"""Rebuild a raw MODE2/2352 .bin + .cue from a destrega.pak data pack (the pack holds every
sector of the disc byte-exact). Used by tools/test_menu.sh when no disc image is at hand.
usage: pak_to_bin.py destrega.pak OUT_BASENAME   -> OUT_BASENAME.bin / OUT_BASENAME.cue"""
import os, struct, sys, zlib

pak, out = sys.argv[1], sys.argv[2]
f = open(pak, 'rb')
assert f.read(8) == b'DSTRPAK1', 'not a data pack'
sectors, bs, nb, _crc = struct.unpack('<4I', f.read(16))
offs = struct.unpack('<%dQ' % (nb + 1), f.read(8 * (nb + 1)))
left = sectors
with open(out + '.bin', 'wb') as o:
    for b in range(nb):
        f.seek(offs[b])
        raw = zlib.decompress(f.read(offs[b + 1] - offs[b]))
        n = min(left, bs)
        o.write(raw[:n * 2352])
        left -= n
name = os.path.basename(out)
with open(out + '.cue', 'w') as c:
    c.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % name)
print(sectors, 'sectors')
