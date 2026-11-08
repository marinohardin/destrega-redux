#!/usr/bin/env python3
"""Check that a run with runtime 4:3/16:9 switches presented frames that follow the switch.
   check_ws_toggle.py LOG DUMPDIR SWITCHES [FIRST_FRAME]   (used by tools/test_widescreen.sh)

LOG has the 'widescreen: on|off at frame N' and 'widescreen-present: frame N wide|4:3' lines,
DUMPDIR the --dump screenshots. Passes when: the run saw SWITCHES switches; no frame was
presented wide while switched off; every switch-on was presented wide within 8 frames; no
screenshot taken while off is wider than 4:3; >= 80% of the screenshots taken while on are wide.
"""
import os, re, struct, sys

log, dumps, want = sys.argv[1], sys.argv[2], int(sys.argv[3])
first = int(sys.argv[4]) if len(sys.argv) > 4 else 3700   # screenshots from the fight on
sw, pres = [], []                       # (frame, on), (frame, wide)
for line in open(log):
    m = re.search(r'widescreen: (on|off) at frame (\d+)', line)
    if m: sw.append((int(m.group(2)), m.group(1) == 'on'))
    m = re.search(r'widescreen-present: frame (\d+) (wide|4:3)', line)
    if m: pres.append((int(m.group(1)), m.group(2) == 'wide'))

def on_at(f):
    st = False
    for fr, on in sw:
        if fr <= f: st = on
    return st

wide_while_off = [f for f, w in pres if w and not on_at(f)]
late = []
for fr, on in sw:
    if on:
        nxt = [f for f, w in pres if w and f >= fr]
        if not nxt or nxt[0] - fr > 8: late.append(fr)

dims = {}
for n in os.listdir(dumps):
    m = re.match(r'frame_(\d+)\.png$', n)
    if m and int(m.group(1)) >= first:
        with open(os.path.join(dumps, n), 'rb') as fh:
            dims[int(m.group(1))] = struct.unpack('>II', fh.read(24)[16:24])
# screenshots are raw target pixels (one display mode during fights): the narrowest is 4:3
base =min(dims[f][0] / dims[f][1] for f in dims) if dims else 0
wide = lambda f: dims[f][0] / dims[f][1] > base * 1.1
on_shots = [f for f in dims if on_at(f)]
on_wide = [f for f in on_shots if wide(f)]
off_wide = [f for f in dims if not on_at(f) and wide(f)]
print(f"widescreen: wstoggle: {len(sw)} switches; {len(on_wide)} of {len(on_shots)} screenshots "
      f"taken while on are wide, {len(off_wide)} wide while off; {len(wide_while_off)} frames "
      f"presented wide while off; switch-ons not wide within 8 frames: {late or 'none'}")
ok = (len(sw) == want and not wide_while_off and not late and not off_wide and on_shots
      and len(on_wide) * 10 >= len(on_shots) * 8)
print("widescreen: wstoggle: presentation follows the switch: " + ("ok" if ok else "FAIL"))
sys.exit(0 if ok else 1)
