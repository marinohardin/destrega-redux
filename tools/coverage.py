#!/usr/bin/env python3
"""Coverage report: what the test suite exercises.

  coverage.py matrix                 characters / stages / modes covered by replays and scenarios
                                     (static: read from tools/replays.py and tools/scenarios.py)
  coverage.py build                  configure + build a coverage build in build/cov
                                     (-DDESTREGA_COVERAGE=ON: every recompiled function records its
                                     name the first time it runs; no cost otherwise)
  coverage.py run [--replays-only]   run all scenarios + replays (--all set) with that build, collecting the
                                     names in build/cov/hits.txt (one append per process);
                                     `run --append NAME...` adds only those replays to the hits
  coverage.py report [-v]            recompiled functions never executed by the collected runs,
                                     per image (main exe / each overlay); -v lists them

Tests use the normal build; only this tool uses build/cov (env DESTREGA_BIN is honoured by
scenarios.py / replays.py, DESTREGA_COVERAGE_OUT names the hit file).
"""
import argparse, collections, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
COV = os.path.join(ROOT, 'build', 'cov')
HITS = os.path.join(COV, 'hits.txt')


def matrix():
    import replays as R, scenarios as S
    chars = {c: [] for c in S.ROSTER}
    stages = {s: [] for s in S.STAGES}
    modes = collections.OrderedDict()
    for rp in R.REPLAYS.values():
        for c in rp.chars: chars.setdefault(c, []).append('replay:' + rp.name)
        if rp.stage: stages.setdefault(rp.stage, []).append('replay:' + rp.name)
        modes.setdefault(rp.mode, []).append('replay:' + rp.name)
    for sc in S.SCENARIOS.values():
        m = re.match(r'VS: (\w+) \(P1\) vs (\w+) \(P2\) on (\w+)', sc.desc)
        if m:
            for c in m.groups()[:2]: chars.setdefault(c, []).append('scenario:' + sc.name)
            stages.setdefault(m.group(3), []).append('scenario:' + sc.name)
            modes.setdefault('vs', []).append('scenario:' + sc.name)
        else:
            m = re.match(r'1P BATTLE as (\w+)', sc.desc)
            if m: chars.setdefault(m.group(1), []).append('scenario:' + sc.name)
            modes.setdefault(sc.name, []).append('scenario:' + sc.name)
    out = []
    for title, d in (('characters', chars), ('stages', stages), ('modes', modes)):
        out.append(f'{title}:')
        for k, v in d.items():
            nr = sum(1 for x in v if x.startswith('replay:')); ns = len(v) - nr
            out.append(f'  {k:12s} {nr} replays, {ns} scenarios' + ('' if v else '   <-- NOT COVERED'))
    return '\n'.join(out)


def build():
    subprocess.check_call(['cmake', '-S', ROOT, '-B', COV, '-G', 'Ninja', '-DDESTREGA_COVERAGE=ON',
                           '-DDESTREGA_MACOS_APP=OFF'], cwd=ROOT)
    subprocess.check_call(['cmake', '--build', COV, '-j', str(os.cpu_count() or 4)], cwd=ROOT)


def run(replays_only, append=False, names=()):
    env = dict(os.environ, DESTREGA_BIN=os.path.join(COV, 'destrega'), DESTREGA_COVERAGE_OUT=HITS)
    if os.path.exists(HITS) and not append: os.remove(HITS)
    rc = 0
    if names:   # only these replays (adds to the collected hits with --append)
        return subprocess.call([sys.executable, 'tools/replays.py', 'run', '--all'] + list(names), cwd=ROOT, env=env)
    if not replays_only:
        rc |= subprocess.call([sys.executable, 'tools/scenarios.py', 'run', '--all'], cwd=ROOT, env=env)
    rc |= subprocess.call([sys.executable, 'tools/replays.py', 'run', '--all'], cwd=ROOT, env=env)
    return rc


def all_functions():
    """[(name, image)] from gen/table.c (image 'main' or the overlay name)."""
    out = []
    for line in open(os.path.join(ROOT, 'gen', 'table.c')):
        m = re.match(r'\s*\{0x([0-9a-f]+), (f_\w+), (-?\d+),', line)
        if m: out.append((m.group(2), m.group(2).split('_', 2)[2] if m.group(2).count('_') >= 2 else 'main'))
    return out


def report(verbose):
    funcs = all_functions()
    hit = set(open(HITS).read().split()) if os.path.exists(HITS) else set()
    by = collections.OrderedDict()
    for n, img in funcs:
        by.setdefault(img, [0, 0, []])
        by[img][1] += 1
        if n in hit: by[img][0] += 1
        else: by[img][2].append(n)
    tot = sum(v[1] for v in by.values()); th = sum(v[0] for v in by.values())
    lines = [f'functions executed: {th} of {tot} ({100.0 * th / max(tot, 1):.1f}%)']
    for img, (h, n, miss) in by.items():
        lines.append(f'  {img:10s} {h:4d} / {n:4d}')
        if verbose and miss: lines.append('      never run: ' + ' '.join(m[2:] for m in miss))
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['matrix', 'build', 'run', 'report'])
    ap.add_argument('-v', action='store_true')
    ap.add_argument('--replays-only', action='store_true')
    ap.add_argument('--append', action='store_true', help='run: keep the hits collected so far')
    ap.add_argument('names', nargs='*', help='run: only these replays')
    a = ap.parse_args()
    if a.cmd == 'matrix': print(matrix())
    elif a.cmd == 'build': build()
    elif a.cmd == 'run': return run(a.replays_only, a.append, a.names)
    else: print(report(a.v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
