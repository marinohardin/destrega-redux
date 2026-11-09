#!/usr/bin/env python3
"""Frame-to-frame instability of a screen region in dumped frames (e.g. one fighter).

usage: flicker_metric.py DIR X Y W H [FIRST LAST]

DIR holds frame_NNNNNN.png from `destrega --dump DIR --dump-every 1` (e.g. with `--replay`,
`--resize F:3834x2328 --widescreen --renderer gpu --dump-from F --frames F+60`). The box is cropped
from every frame; repeated frames are dropped (fights update every second vblank), then it prints
for the remaining updates t:
  flicker    mean |I(t) - (I(t-1) + I(t+1)) / 2| over the box (0..255 per channel): smooth motion
             contributes little, pixels that jump back and forth (holes, snapping) a lot
  frame-diff mean |I(t) - I(t-1)|
  hot px     pixels per update whose flicker exceeds 64 in some channel
The real animation is in the numbers too, so compare runs of the same frames (e.g. --pgxp on / off)
rather than reading them as absolute values. Used for the character-mesh PGXP work (NOTES.md
"Character mesh and PGXP")."""
import glob
import os
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) not in (6, 8):
        sys.exit(__doc__)
    d = sys.argv[1]
    x, y, w, h = map(int, sys.argv[2:6])
    files = sorted(glob.glob(os.path.join(d, 'frame_[0-9]*.png')))
    if len(sys.argv) == 8:
        lo, hi = int(sys.argv[6]), int(sys.argv[7])
        files = [f for f in files if lo <= int(os.path.basename(f)[6:12]) <= hi]
    frames, last = [], None
    for f in files:
        a = np.asarray(Image.open(f).convert('RGB').crop((x, y, x + w, y + h)), dtype=np.float32)
        if last is None or not np.array_equal(a, last):
            frames.append(a)
        last = a
    if len(frames) < 3:
        sys.exit('flicker_metric: need at least 3 distinct frames')
    A = np.stack(frames)
    sd = np.abs(A[1:-1] - 0.5 * (A[:-2] + A[2:]))
    fd = np.abs(A[1:] - A[:-1])
    hot = np.mean((sd.max(axis=3) > 64).sum(axis=(1, 2)))
    print(f'updates {len(frames)}  flicker {sd.mean():.3f}  frame-diff {fd.mean():.3f}  hot px {hot:.1f}')


if __name__ == '__main__':
    main()
