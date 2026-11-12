#!/usr/bin/env python3
"""Count thin bright seams that the native 3D renderer (--hlr on) adds to a frame.

usage: check_hlr_seams.py ON.png OFF.png [ON.png OFF.png ...] [--max N] [--mask DIR]

ON and OFF are `--dump` frames of two runs that differ only in `--hlr on` / `off` (same input,
render scale, window). A seam is a line 1-2 px wide and at least MINLEN px long that is brighter
than the pixels on both sides of it (by T in luminance, measured 2 and 3 px away across it) in the
ON frame and nowhere near (2 px) such a ridge in the OFF frame.

The bug this was written for: with MSAA every native pass started from the resolved target, so a
pixel on an edge shared by two floor polygons drawn in different passes mixed in what was under
the floor; the pass boundaries around the fighters (their shadows split the floor into passes)
showed as thin bright outlines of rectangles and trapezoids (vs_gate frame 3300 at 4x6: lines of
345 and 485 px). Texture detail gives short or thicker ridges: bright gravel streaks are 40-75 px
long with 1.3-1.5 px of area per px of length, so MINLEN 80 and a thinness limit of 1.6 keep them
out.

Prints the seams per frame; exit 1 when a frame has more than --max (default 0).
Needs numpy, scipy and Pillow.
"""
import os, sys

T = 10.0          # luminance step above both sides
MINLEN = 80       # px, along the longer side of the bounding box
THIN = 1.6        # at most this many ridge pixels per px of length


def _mods():
    try:
        import numpy as np
        from PIL import Image
        from scipy import ndimage as nd
    except ImportError as e:
        sys.exit(f'check_hlr_seams: needs numpy, scipy and Pillow ({e})')
    return np, Image, nd


def ridges(path):
    np, Image, _ = _mods()
    a = np.asarray(Image.open(path).convert('RGB')).astype(np.float32)
    lum = a @ np.array([0.299, 0.587, 0.114], np.float32)
    m = np.zeros(lum.shape, bool)
    for d in (2, 3):
        for ax in (0, 1):
            m |= (lum - np.maximum(np.roll(lum, d, ax), np.roll(lum, -d, ax))) > T
    m[:4] = m[-4:] = False
    m[:, :4] = m[:, -4:] = False
    return m


def seams(on_path, off_path, mask_path=None):
    """[(x, y, w, h, pixels)] of the seams in on_path that off_path does not have"""
    np, Image, nd = _mods()
    m = ridges(on_path) & ~nd.binary_dilation(ridges(off_path), iterations=2)
    lab, _ = nd.label(m, structure=np.ones((3, 3)))
    out, keep = [], np.zeros(m.shape, bool)
    for i, sl in enumerate(nd.find_objects(lab), 1):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        ln = max(h, w)
        if ln < MINLEN:
            continue
        px = int((lab[sl] == i).sum())
        if px > THIN * ln:
            continue
        out.append((sl[1].start, sl[0].start, w, h, px))
        keep[sl] |= lab[sl] == i
    if mask_path:
        Image.fromarray((keep * 255).astype(np.uint8)).save(mask_path)
    return out


def main():
    args, mx, mask = sys.argv[1:], 0, None
    if '--max' in args:
        i = args.index('--max'); mx = int(args[i + 1]); del args[i:i + 2]
    if '--mask' in args:
        i = args.index('--mask'); mask = args[i + 1]; del args[i:i + 2]
        os.makedirs(mask, exist_ok=True)
    if not args or len(args) % 2:
        sys.exit(__doc__)
    bad = 0
    for on, off in zip(args[0::2], args[1::2]):
        mp = os.path.join(mask, os.path.basename(on)) if mask else None
        s = seams(on, off, mp)
        print(f'{on}: {len(s)} seams' + (': ' + ' '.join(f'{w}x{h}@{x},{y}' for x, y, w, h, _ in s[:8]) if s else ''))
        bad += len(s) > mx
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
