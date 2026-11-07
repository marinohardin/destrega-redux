#!/usr/bin/env python3
"""Print a --press script: boot -> title -> 1P battle -> a fight with varied inputs.
usage: fight_script.py [last_frame]   (default 9000)"""
import sys
last = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
steps = [(1200, 0x8), (1500, 0x8), (1800, 0x8), (2100, 0x8)]
steps += [(f, 0x4000) for f in (2400, 2700, 3000, 3300, 3600)]
# fight: cycle through attacks and movement (deterministic, no RNG)
cycle = [0x4000, 0x8000 | 0x20, 0x2000, 0x1000 | 0x80, 0x4000 | 0x10, 0x8000 | 0x40,
         0x2000 | 0x20, 0x20, 0x4000 | 0x80, 0x1000, 0x400, 0x800 | 0x4000]
k = 0
for f in range(3800, last, 24):
    steps.append((f, cycle[k % len(cycle)]))
    k += 1
print(",".join(f"{f}:{m:x}" for f, m in steps))
