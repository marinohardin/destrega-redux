#!/usr/bin/env python3
"""Tile PNG screenshots (from --dump) into one contact sheet:
   contact_sheet.py [--cell WxH] [--cols N] OUT.png IN...   (default cell 320x240, 4 columns)"""
import struct, sys, zlib

def read_png(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    pos, idat = 8, b''
    while pos < len(d):
        n, = struct.unpack('>I', d[pos:pos + 4]); t = d[pos + 4:pos + 8]
        if t == b'IDAT': idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = [raw[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h)]
    return w, h, rows

def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

args = sys.argv[1:]
cols, cw, ch = 4, 320, 240
while args and args[0].startswith('--'):
    if args[0] == '--cell': cw, ch = map(int, args[1].lower().split('x'))
    elif args[0] == '--cols': cols = int(args[1])
    args = args[2:]
out, files = args[0], args[1:]
rows_n = (len(files) + cols - 1) // cols
canvas = [bytearray(cols * cw * 4) for _ in range(rows_n * ch)]
for k, f in enumerate(files):
    w, h, rows = read_png(f)
    ox, oy = (k % cols) * cw, (k // cols) * ch
    for y in range(min(h, ch)):
        for x in range(cw):
            sx = x * w // cw
            canvas[oy + y][(ox + x) * 4:(ox + x) * 4 + 4] = rows[y][sx * 4:sx * 4 + 4]
write_png(out, cols * cw, rows_n * ch, [bytes(r) for r in canvas])
