#!/usr/bin/env python3
"""Count PGXP cracks in widescreen screenshots.

usage: check_ws_cracks.py ON_DIR OFF_DIR SX SY [--max N] [--mask DIR]

ON_DIR and OFF_DIR hold `--dump` frames of two runs that differ only in `--pgxp on` / `off`
(same input, `--widescreen`, render scale SX x SY). A crack is a gap between two triangles that
should share an edge, through which the background (black) shows as a thin sliver.

Candidate pixels: a run of background (exact black) pixels in the PGXP frame, horizontal and at
most 2*SX long (or vertical and at most 2*SY long), with lit pixels at both ends, where *every*
pixel of the run is lit in the PGXP-off frame, and so is everything within one 1x pixel of it
(lit: r + g + b > 48). Dark texture stripes that reach black, and see-through texels, move by
less than a 1x pixel with PGXP, so the off frame has a dark pixel next to them: they do not count. PGXP moves edges by less than one 1x pixel, so legitimate
differences sit at silhouettes (one side is background) and do not qualify.

Candidates are grouped (8-connected). A crack is a group that runs at least 16 1x pixels in some
direction and is not axis-aligned (bounding box at least 2 1x pixels in both directions):
shifted see-through texels and dark texture rows give small blobs or 1-row lines, which also
appear without widescreen. The crack this was written for (a guard-band triangle drawn at
integer positions next to a precise neighbour) gave lines of 16..66 1x pixels.

Prints per frame the cracks (and candidate pixels) in the guard band (the columns outside the
centred 512 * SX) and in the centre; exit 1 when a frame has more than --max cracks (default 0).
--mask DIR writes a PGM per frame with the candidate pixels.
"""
import os, struct, sys, zlib


LIT = 48   # r + g + b above this: a lit surface (a crack is black against one)


def read_png(path):
    """`--dump` PNGs: 8-bit RGBA, filter 0 on every row. Returns w, h and a class per pixel:
    1 = background (exact black: the clear colour; 0x0000 texels are transparent, so textures
    rarely give exact black), 2 = dark (r + g + b <= LIT), 0 = lit."""
    d = open(path, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    pos, idat = 8, b''
    while pos < len(d):
        n, = struct.unpack('>I', d[pos:pos + 4])
        if d[pos + 4:pos + 8] == b'IDAT': idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    s = w * 4 + 1
    bg = bytearray(w * h)
    for y in range(h):
        row = raw[y * s + 1:(y + 1) * s]
        r, g, b = row[0::4], row[1::4], row[2::4]
        bg[y * w:(y + 1) * w] = bytes(1 if not (r[x] or g[x] or b[x]) else 2 if r[x] + g[x] + b[x] <= LIT else 0
                                      for x in range(w))
    return w, h, bg


def runs(on, off, start, step, count, maxlen, hit):
    """Background runs along one line (count pixels from `start`, stride `step`)."""
    k = 0
    while k < count:
        if on[start + k * step] != 1: k += 1; continue
        e = k
        while e < count and on[start + e * step] == 1: e += 1
        if 0 < k and e < count and e - k <= maxlen and not on[start + (k - 1) * step] and \
                not on[start + e * step] and not any(off[start + j * step] for j in range(k, e)):
            for j in range(k, e): hit[start + j * step] = 1
        k = e


def near_unlit(off, w, h, sx, sy):
    """1 where the PGXP-off frame has a pixel that is not lit within +-SX columns / +-SY rows
    (one 1x pixel): row bitmasks, dilated by shifts."""
    rows = []
    for y in range(h):
        m = int(''.join('1' if v else '0' for v in reversed(off[y * w:(y + 1) * w])) or '0', 2)
        d = m
        for k in range(1, sx + 1): d |= (m << k) | (m >> k)
        rows.append(d)
    out = bytearray(w * h)
    full = (1 << w) - 1
    for y in range(h):
        d = 0
        for yy in range(max(0, y - sy), min(h, y + sy + 1)): d |= rows[yy]
        d &= full
        bits = bin(d)[2:].zfill(w)[::-1]
        out[y * w:(y + 1) * w] = bytes(1 if c == '1' else 0 for c in bits)
    return out


def candidates(on, off, w, h, sx, sy):
    off = near_unlit(off, w, h, sx, sy)
    hit = bytearray(w * h)
    for y in range(h): runs(on, off, y * w, 1, w, 2 * sx, hit)
    for x in range(w): runs(on, off, x, w, h, 2 * sy, hit)
    return hit


def cracks(hit, w, h, sx, sy, guard):
    """[(in_guard, length in 1x pixels, x0, y0, x1, y1)]"""
    seen = bytearray(w * h)
    out = []
    for s in range(w * h):
        if not hit[s] or seen[s]: continue
        st = [s]; seen[s] = 1
        x0 = x1 = s % w; y0 = y1 = s // w
        while st:
            i = st.pop()
            x, y = i % w, i // w
            if x < x0: x0 = x
            if x > x1: x1 = x
            if y < y0: y0 = y
            if y > y1: y1 = y
            for j in (i - w - 1, i - w, i - w + 1, i - 1, i + 1, i + w - 1, i + w, i + w + 1):
                if 0 <= j < w * h and abs(j % w - x) <= 1 and hit[j] and not seen[j]:
                    seen[j] = 1; st.append(j)
        bw, bh = (x1 - x0 + 1) / sx, (y1 - y0 + 1) / sy
        if max(bw, bh) >= 16 and min(bw, bh) >= 2:
            out.append((x1 < guard or x0 >= w - guard, max(bw, bh), x0, y0, x1, y1))
    return out


def main():
    args = sys.argv[1:]
    mx, mask = 0, None
    if '--max' in args:
        k = args.index('--max'); mx = int(args[k + 1]); del args[k:k + 2]
    if '--mask' in args:
        k = args.index('--mask'); mask = args[k + 1]; del args[k:k + 2]; os.makedirs(mask, exist_ok=True)
    on_dir, off_dir, sx, sy = args[0], args[1], int(args[2]), int(args[3])
    names = sorted(n for n in os.listdir(on_dir) if n.startswith('frame_0') and n.endswith('.png'))
    fail = not names
    total = 0
    for n in names:
        if not os.path.exists(os.path.join(off_dir, n)): continue
        w, h, on = read_png(os.path.join(on_dir, n))
        w2, h2, off = read_png(os.path.join(off_dir, n))
        if (w, h) != (w2, h2): print(f'{n}: sizes differ, skipped'); continue
        hit = candidates(on, off, w, h, sx, sy)
        guard = max(0, (w - 512 * sx) // 2)
        g = c = 0
        for y in range(h):
            row = hit[y * w:(y + 1) * w]
            g += sum(row[:guard]) + sum(row[w - guard:])
            c += sum(row[guard:w - guard])
        ls = cracks(hit, w, h, sx, sy, guard)
        lg = sum(1 for l in ls if l[0])
        total += len(ls)
        bad = len(ls) > mx
        fail |= bad
        print(f'{n}: {w}x{h}: {lg} cracks in the guard band, {len(ls) - lg} in the centre '
              f'({g} + {c} candidate pixels){"  FAIL" if bad else ""}')
        for l in ls:
            print(f'  crack: {l[1]:.0f} 1x px long, pixels ({l[2]},{l[3]})-({l[4]},{l[5]})')
        if mask:
            with open(os.path.join(mask, n.replace('.png', '.pgm')), 'wb') as f:
                f.write(b'P5 %d %d 255\n' % (w, h) + bytes(255 if v else 0 for v in hit))
    print(f'cracks: {total} in {len(names)} frames')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
