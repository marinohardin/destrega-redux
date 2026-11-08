#!/usr/bin/env python3
"""Move lab: find out which command inputs do what, by experiment (headless, deterministic).

P1 (the character under test) performs a battery of candidate inputs, one every GAP frames, against an
idle P2 in a VS fight; the --gameplay-trace says which candidate spent gauge (a drop of > 100 in the
following window) or hurt P2. Results are printed as a table; nothing is written to the repo.

  movelab.py probe CHAR [--vs OPP] [--stage S] [--part N] [-j N]   run the battery (split in parts)
  movelab.py list                                                  the candidate names

Candidates are written for a fighter facing right (P1 on the left starts facing right):
f = right, b = left, d = down, u = up. Motions are 6 frames per step like the replay generator.
"""
import argparse, concurrent.futures as cf, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import scenarios as S  # noqa: E402

B = S.BTN
BIN = S.BIN
BUTTONS = ['square', 'cross', 'triangle', 'circle', 'l1', 'r1', 'l2', 'r2']
GAP = 120          # frames between candidates (gauge regenerates ~6 per frame; a spend costs ~600)
STEP = 6


def candidates():
    """name -> list of masks (one per 6-frame step)."""
    f, b, d, u = B['right'], B['left'], B['down'], B['up']
    c = {}
    for n in BUTTONS:
        m = B[n]
        c[f'{n}'] = [m]
        c[f'f+{n}'] = [f | m]
        c[f'b+{n}'] = [b | m]
        c[f'd+{n}'] = [d | m]
        c[f'u+{n}'] = [u | m]
        c[f'qcf+{n}'] = [d, d | f, f | m]
        c[f'qcb+{n}'] = [d, d | b, b | m]
        c[f'dp+{n}'] = [f, d, d | f | m]
        c[f'hcf+{n}'] = [b, d | b, d, d | f, f | m]
        c[f'ff+{n}'] = [f, 0, f | m]
        c[f'bb+{n}'] = [b, 0, b | m]
        c[f'hold {n}'] = [m] * 8
    for a, bb in [('square', 'cross'), ('square', 'triangle'), ('square', 'circle'), ('cross', 'triangle'),
                  ('cross', 'circle'), ('triangle', 'circle'), ('l1', 'r1'), ('l2', 'r2'), ('l1', 'l2'),
                  ('r1', 'r2'), ('square', 'l1'), ('cross', 'r1')]:
        c[f'{a}+{bb}'] = [B[a] | B[bb]]
        c[f'f+{a}+{bb}'] = [f | B[a] | B[bb]]
        c[f'qcf+{a}+{bb}'] = [d, d | f, f | B[a] | B[bb]]
        c[f'qcb+{a}+{bb}'] = [d, d | b, b | B[a] | B[bb]]
    return c


def battery_names():
    """The 96 commands the replays use (taps, 4 directions + button, qcf / qcb / dp, forward and back
    double taps for each of the 8 buttons, the 12 button pairs, holds of the 4 face buttons)."""
    keep = []
    for n in candidates():
        if n.startswith(('hcf+', 'f+square+', 'f+cross+', 'f+triangle+', 'f+l', 'f+r', 'qcf+square+', 'qcf+cross+',
                         'qcf+triangle+', 'qcf+l', 'qcf+r', 'qcb+square+', 'qcb+cross+', 'qcb+triangle+', 'qcb+l', 'qcb+r')):
            continue
        if n.startswith('hold ') and n[5:] not in ('square', 'cross', 'triangle', 'circle'): continue
        keep.append(n)
    return keep


def schedule(names, t0, gap=GAP):
    steps, starts, t = [], {}, t0
    cands = candidates()
    for n in names:
        starts[n] = t
        for m in cands[n]:
            steps.append((t, m)); t += STEP
        t = starts[n] + gap
    return steps, starts


def fmt(steps):
    return ','.join(f'{f}:{m:x}' for f, m in sorted(steps))


def trace_rows(path):
    rows = {}
    for line in open(path):
        m = re.match(r'(\d+) p1 hp=(-?\d+) gauge=(-?\d+) pos=(\S+) p2 hp=(-?\d+) gauge=(-?\d+) pos=(\S+) time=(\d+) wins=(\d+),(\d+)', line)
        if m:
            fr = int(m.group(1))
            rows[fr] = dict(hp1=int(m.group(2)), g1=int(m.group(3)), hp2=int(m.group(5)), g2=int(m.group(6)),
                            time=int(m.group(8)), w=(int(m.group(9)), int(m.group(10))))
    return rows


def analyse(rows, starts, gap=GAP):
    res = {}
    for n, t in starts.items():
        w = [rows[k] for k in range(t, t + gap - 10) if k in rows]
        if len(w) < 20: res[n] = None; continue
        # a round restart in the window makes the numbers meaningless
        if any(w[i + 1]['time'] < w[i]['time'] for i in range(len(w) - 1)):
            res[n] = ('round', 0, 0); continue
        spend = max((w[i]['g1'] - w[i + 1]['g1'] for i in range(len(w) - 1)), default=0)
        dmg = w[0]['hp2'] - w[-1]['hp2']
        res[n] = ('ok', spend, dmg)
    return res


def run_part(char, opp, stage, names, tag):
    d1, d2, t_end = S.vs_script(char, opp, stage)
    t0 = t_end + 800
    steps, starts = schedule(names, t0)
    frames = t0 + GAP * len(names) + 100
    os.makedirs(os.path.join(ROOT, 'build', 'movelab'), exist_ok=True)
    tr = os.path.join(ROOT, 'build', 'movelab', f'{char}_{tag}.trace')
    cmd = [BIN, '--headless', '--no-interp', '--no-memcard', '--frames', str(frames), '--gameplay-trace', tr,
           '--press', fmt(S.parse(d1) + steps), '--press2', fmt(S.parse(d2))]
    subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    return analyse(trace_rows(tr), starts)


def anim_map(char, opp, stage, names, tag):
    """Per candidate: the set of distinct animation pointers (fighter struct +0x114) P1 showed in the
    window, and the gauge spend. A pointer is the fighter's current animation / move data
    (per-character overlay data), so a candidate that shows pointers no earlier one showed is a
    distinct move (a special/magic, a throw, a dash...)."""
    d1, d2, t_end = S.vs_script(char, opp, stage)
    t0 = t_end + 800
    steps, starts = schedule(names, t0)
    frames = t0 + GAP * len(names) + 100
    tr = os.path.join(ROOT, 'build', 'movelab', f'{char}_{tag}.trace')
    os.makedirs(os.path.dirname(tr), exist_ok=True)
    env = dict(os.environ, DESTREGA_STRUCT_TRACE='1')
    subprocess.run([BIN, '--headless', '--no-interp', '--no-memcard', '--frames', str(frames), '--gameplay-trace', tr,
                    '--press', fmt(S.parse(d1) + steps), '--press2', fmt(S.parse(d2))],
                   env=env, capture_output=True, text=True, cwd=ROOT)
    anims = {}
    for line in open(tr):
        if line.startswith('S1 '):
            p = line.split(); anims[int(p[1])] = int(p[2 + 0x114 // 4], 16)
    res = analyse(trace_rows(tr), starts)
    out = {}
    for n, t in starts.items():
        out[n] = (res.get(n), sorted({anims[k] for k in range(t, t + GAP - 10) if k in anims}))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['probe', 'map', 'list'])
    ap.add_argument('char', nargs='?')
    ap.add_argument('--vs', default='celia')
    ap.add_argument('--stage', default='hills')
    ap.add_argument('--part', type=int, default=-1)
    ap.add_argument('--per', type=int, default=24)
    ap.add_argument('--only', default='', help='comma separated candidate names')
    ap.add_argument('-j', type=int, default=3)
    a = ap.parse_args()
    names = list(candidates())
    if a.cmd == 'list':
        print('\n'.join(names)); return 0
    if a.only: names = a.only.split(',')
    parts = [names[i:i + a.per] for i in range(0, len(names), a.per)]
    todo = [(i, p) for i, p in enumerate(parts) if a.part < 0 or a.part == i]
    opp = a.vs if a.vs != a.char else 'gradd'
    if a.cmd == 'map':
        out = anim_map(a.char, opp, a.stage, names, 'map')
        seen = set()
        idle = set()
        for n in names:
            r, an = out[n]
            new = [x for x in an if x not in seen]
            seen |= set(an)
            sp = r[1] if r and r[0] == 'ok' else '-'
            dm = r[2] if r and r[0] == 'ok' else '-'
            print(f'{n:20s} spend={sp!s:>4} dmg={dm!s:>3} anims={len(an):2d} new={len(new)}')
        print(f'total distinct animations: {len(seen)}')
        return 0
    with cf.ThreadPoolExecutor(a.j) as ex:
        out = list(ex.map(lambda ip: run_part(a.char, opp, a.stage, ip[1], str(ip[0])), todo))
    for (i, p), res in zip(todo, out):
        for n in p:
            r = res.get(n)
            if r is None: print(f'{n:22s} (no data)')
            elif r[0] == 'round': print(f'{n:22s} (round change)')
            else: print(f'{n:22s} spend={r[1]:5d} p2dmg={r[2]:4d}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
