#!/usr/bin/env python3
"""The BGM loops cleanly at the end of its track (regression test; pr_check core group, run_tests.sh).

The game polls the CD position with GetlocL every 2 vblanks while an XA track streams and, once the
position is past the track's end, pauses, Setlocs back to the track's start and ReadS again. GetlocL
used to report only the last *data* sector, so after that re-seek it still said "past the end" and
the game re-seeked every 9 vblanks forever: the BGM became a 0.15 s fragment repeated
(analysis/NOTES.md "CD: GetlocL and the BGM loop"). Two headless runs, each past the loop point:

  fight  VS Milena (P1) vs Doyle (P2) on Fortress, nobody moves: the fight BGM (XADATA.BNS file 1,
         its stage's channel) starts at ~3100 and loops at ~11200; run to 13000.
  menu   VS character select waiting for player 2: its BGM (XADATA 09:21:00) loops at ~9680;
         run to 11000.

Checks per run: the track was re-seeked at least once after it started (the loop point was crossed)
and never twice within 600 vblanks (no seek storm), the `--wav` output from the track's start has no
stuck loop (tools/xa_loop_check.py), and the log has no FATAL / dispatch miss / unhandled call.
usage: tools/test_bgm_loop.py [-j N] [--keep]
"""
import concurrent.futures as cf, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import scenarios  # noqa: E402

BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))
OUT = os.path.join(ROOT, 'build', 'bgm_loop')
BAD = re.compile(r'FATAL|dispatch: no recompiled|unhandled|MISMATCH|interp: enter')
CMD = re.compile(r'\[\s*(\d+)\] cd: cmd (\w\w) params \d+ \[(\w\w) (\w\w) (\w\w)\]')


def cases():
    d1, d2, _ = scenarios.vs_script('milena', 'doyle', 'fortress')
    m1, _, _ = scenarios.vs_script('milena', 'doyle', 'hills')
    return [('fight', scenarios.press_arg(d1), scenarios.press_arg(d2), 13000, 2500),
            # P1 picks, pad 2 never does: the character select (and its music) stays up
            ('menu', scenarios.press_arg(m1), '', 11000, 1500)]


def run(case):
    name, p1, p2, frames, bgm_after = case
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    wav = os.path.join(d, 'out.wav')
    cmd = [BIN, '--headless', '--no-interp', '--frames', str(frames), '--trace', '--wav', wav, '--press', p1]
    if p2: cmd += ['--press2', p2]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=1800)
    log = r.stdout + r.stderr
    open(os.path.join(d, 'log.txt'), 'w').write(log)
    errs = []
    if r.returncode != 0: errs.append(f'exit status {r.returncode}')
    if f'reached {frames} frames' not in log: errs.append('did not reach the last frame')
    bad = BAD.search(log)
    if bad: errs.append(f'log has "{bad.group(0)}"')
    # the BGM: the last Setloc before the first ReadS after `bgm_after`
    setloc, start, reads = None, None, []
    for m in CMD.finditer(log):
        f, c = int(m.group(1)), m.group(2)
        if c == '02': setloc = m.group(3, 4, 5)
        if c == '1b' and f >= bgm_after:
            if start is None: start = (f, setloc)
            elif setloc == start[1]: reads.append(f)
    if start is None:
        errs.append('no BGM ReadS found')
        return name, errs, ''
    info = f'BGM at {":".join(start[1])} from frame {start[0]}, re-seeks at {reads[:6]}'
    if not reads: errs.append('the track never looped: run too short or the end was not detected')
    gaps = [b - a for a, b in zip([start[0]] + reads, reads)]
    if any(g < 600 for g in gaps): errs.append(f'seek storm: re-seeks {min(gaps)} vblanks apart')
    t0 = start[0] / 59.94
    c = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'xa_loop_check.py'), wav, '--from', f'{t0:.0f}'],
                       capture_output=True, text=True)
    info += '; ' + c.stdout.strip()
    if c.returncode != 0: errs.append('stuck audio loop in the wav')
    return name, errs, info


def main():
    j = 2
    if '-j' in sys.argv: j = int(sys.argv[sys.argv.index('-j') + 1])
    fails = 0
    with cf.ThreadPoolExecutor(max_workers=max(1, j)) as ex:
        for name, errs, info in ex.map(run, cases()):
            print(f'{"ok  " if not errs else "FAIL"} {name}: {info}' + ''.join(f'\n     {e}' for e in errs))
            fails += bool(errs)
    if not fails and '--keep' not in sys.argv:
        shutil.rmtree(OUT, ignore_errors=True)     # ~100 MB of wav + trace
    print(f'test_bgm_loop: {"ok" if not fails else f"{fails} FAILED (files in build/bgm_loop)"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
