#!/usr/bin/env python3
"""Print a --press script of random button presses for headless soak runs.

usage: mash_inputs.py SEED END_FRAME  >  presses.txt
       destrega --headless --frames N --press "$(cat presses.txt)"
"""
import random
import sys

seed, end = int(sys.argv[1]), int(sys.argv[2])
random.seed(seed)
# Start, Cross, d-pad, Circle, Square, Triangle, L1, R1 (Cross/Start weighted up)
masks = [0x8, 0x4000, 0x10, 0x40, 0x20, 0x80, 0x2000, 0x8000, 0x1000, 0x400, 0x800, 0x4000, 0x4000, 0x8]
out, f = [], 1200
while f < end:
    out.append(f"{f}:{random.choice(masks)}")
    out.append(f"{f + 12}:0")
    f += random.choice([30, 40, 60])
print(",".join(out))
