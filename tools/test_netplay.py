#!/usr/bin/env python3
"""Rollback netplay tests: two headless processes over loopback UDP (runtime/netplay.c).

Each case starts a host (player 1) and a joining peer (player 2) with the VS menu path and seeded
random fight input of a tools/replays.py replay, one pad per process, over a simulated network
(--net-sim latency:jitter:loss:reorder:seed on each side's sends). Pass criteria:

  * both peers write the same hash for every final (confirmed) frame they both reached: the
    gameplay hash every frame, the complete emulated-state hash every 30 frames;
  * that log equals an offline run (no netplay) of the same inputs shifted by the input delay,
    i.e. rollback / prediction left no trace in the emulated machine;
  * no desync was reported, no rollback was impossible (unless the case allows it), and the
    peers covered at least 95% of the frames.

Frames are paced at --net-pace HZ (default 240 = 4x real time), so the simulated latencies are in
real milliseconds but cover 4x as many vblanks as they would at 60 Hz: `--pace 60` runs in real
time. The `desync` case injects a fault on one peer (--net-inject-desync) and must be detected.

  test_netplay.py [CASE...] [-j N] [--pace HZ] [--list]
"""
import argparse, concurrent.futures as cf, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import replays as R  # noqa: E402

BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))
OUT = os.path.join(ROOT, 'build', 'netplay')

# name: (replay, delay, sim host->join, sim join->host, extra args for both, expectations)
CASES = {
    'lan':        ('vs_tieme_rohzen_villa', 2, '2:1:0:0:1', '2:1:0:0:2', [], {}),
    'wan':        ('vs_milena_cougar_fortress', 2, '20:8:1:2:3', '20:8:1:2:4', [], {}),
    'lossy':      ('vs_zauber_doyle_throne', 2, '15:15:10:10:5', '15:15:10:10:6', [], {}),
    'asym':       ('vs_anjie_laon_canyon', 1, '35:5:2:0:7', '5:2:2:0:8', [], {}),
    'match':      ('match_gradd_celia_hills', 3, '25:10:3:5:9', '25:10:3:5:10', [], {}),
    'desync':     ('vs_tieme_rohzen_villa', 2, '10:4:0:0:11', '10:4:0:0:12', [], {'desync': True}),
    # state-changing menu actions are refused during netplay, presentation-only ones run
    'menu':       ('vs_tieme_rohzen_villa', 2, '2:1:0:0:13', '2:1:0:0:14',
                   ['--menu-action', '1500:load=Destrega/Destrega.cue', '--menu-action', '1510:save-import=x.mcd',
                    '--menu-action', '1520:save-export=build/netplay/menu/x.mcd', '--menu-action', '1530:save-delete',
                    '--menu-action', '1540:widescreen=on', '--menu-action', '1550:reveal-saves'],
                   {'frames': 2400, 'menu': True}),
}


def shifted(steps, d):
    return [(f + d, m) for f, m in steps]


def summary(log):
    m = re.search(r'netplay: (\d+) frames, (\d+) predicted remote inputs, (\d+) mispredicted; (\d+) rollbacks '
                  r'\(max depth (\d+) vblanks, (\d+) deferred one vblank, (\d+) impossible\), (\d+) frames re-simulated, '
                  r're-sim avg ([\d.]+) ms max ([\d.]+) ms per rollback; (\d+) stalls \((\d+) ms\).*?(\d+) final frames, '
                  r'(\d+) hashes checked against the peer, (\d+) desyncs', log)
    if not m: return None
    k = ['frames', 'predicted', 'mispredicted', 'rollbacks', 'max_depth', 'deferred', 'impossible', 'resim_frames',
         'resim_avg_ms', 'resim_max_ms', 'stalls', 'stall_ms', 'final', 'checked', 'desyncs']
    d = dict(zip(k, (float(x) for x in m.groups())))
    c = re.search(r're-simulation cost ([\d.]+) ms per re-simulated vblank', log)
    d['resim_ms_per_frame'] = float(c.group(1)) if c else 0.0
    return d


def read_log(path):
    out = {}
    if os.path.exists(path):
        for line in open(path):
            p = line.split()
            if len(p) >= 2: out[int(p[0])] = tuple(p[1:])
    return out


def run_case(name, port, pace):
    rp_name, delay, sim_h, sim_j, extra, expect = CASES[name]
    rp = R.REPLAYS[rp_name]
    if 'frames' in expect: rp = R.Replay(rp.name, rp.desc, rp.p1, rp.p2, expect['frames'])
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    t0 = time.time()
    common = [BIN, '--headless', '--no-interp', '--frames', str(rp.frames), '--net-delay', str(delay),
              '--net-pace', str(pace), '--net-desync-dir', d] + extra + os.environ.get('NETPLAY_EXTRA', '').split()
    host = common + ['--press', R.fmt(rp.p1), '--host', str(port), '--net-sim', sim_h,
                     '--net-hash-log', os.path.join(d, 'host.hash')]
    join = common + ['--press', R.fmt(rp.p2), '--join', f'127.0.0.1:{port}', '--net-sim', sim_j,
                     '--net-hash-log', os.path.join(d, 'join.hash')]
    if expect.get('desync'):
        join += ['--net-inject-desync', str(rp.frames // 2)]
    with open(os.path.join(d, 'host.log'), 'w') as hl, open(os.path.join(d, 'join.log'), 'w') as jl:
        ph = subprocess.Popen(host, stdout=hl, stderr=subprocess.STDOUT, cwd=ROOT)
        pj = subprocess.Popen(join, stdout=jl, stderr=subprocess.STDOUT, cwd=ROOT)
        rc_j = pj.wait(timeout=900)
        rc_h = ph.wait(timeout=120)
    # offline reference: the same inputs, shifted by the input delay
    ref = os.path.join(d, 'offline.hash')
    off = [BIN, '--headless', '--no-interp', '--frames', str(rp.frames), '--net-hash-log', ref,
           '--press', R.fmt(shifted(rp.p1, delay)), '--press2', R.fmt(shifted(rp.p2, delay))] + \
        ([] if expect.get('menu') else extra)   # the menu case's actions must not run offline (load restarts)
    r = subprocess.run(off, capture_output=True, text=True, cwd=ROOT)
    open(os.path.join(d, 'offline.log'), 'w').write(r.stdout + r.stderr)
    hlog = open(os.path.join(d, 'host.log')).read()
    jlog = open(os.path.join(d, 'join.log')).read()
    sh, sj = summary(hlog), summary(jlog)
    H, J, O = read_log(os.path.join(d, 'host.hash')), read_log(os.path.join(d, 'join.hash')), read_log(ref)
    both = sorted(set(H) & set(J))
    problems = []
    if not sh or not sj: problems.append('no netplay summary (crash? see logs)')
    if expect.get('desync'):
        det = [l for l in (hlog + jlog).splitlines() if 'DESYNC at frame' in l]
        rep = [f for f in os.listdir(d) if f.startswith('desync-')]
        inj = int(rp.frames // 2)
        if not det: problems.append('injected desync NOT detected')
        frames = sorted({int(re.search(r'DESYNC at frame (\d+)', l).group(1)) for l in det})
        # not every final frame's hash reaches the peer (one per packet), so detection may come a little later
        if frames and not inj <= frames[0] <= inj + 30: problems.append(f'first desync at frame {frames[0]}, injected at {inj}')
        # the reports' input logs replay offline (M2 tool): the clean peer's timeline matches a clean
        # run, the faulty peer's diverges exactly at the injected frame, naming p1.hp
        for who, want in (('host', None), ('join', inj)):
            found = sorted(f for f in os.listdir(d) if f.startswith('desync-') and f.endswith(f'-{who}.dtr'))
            if not found: problems.append(f'no {who} desync report'); continue
            path = os.path.join(d, found[0])
            r = subprocess.run([BIN, '--headless', '--no-interp', '--replay', path], capture_output=True, text=True, cwd=ROOT)
            log = r.stdout + r.stderr
            open(os.path.join(d, f'replay-{who}.log'), 'w').write(log)
            m = re.search(r'replay: DIVERGED at frame (\d+)', log)
            if want is None and (r.returncode or m): problems.append(f'{who} report does not replay cleanly')
            if want is not None and not (m and int(m.group(1)) == want and re.search(r'p1\.hp +DIFFERS', log)):
                problems.append(f'{who} report replay: expected a p1.hp divergence at {want}')
        ok = not problems
        dt = time.time() - t0
        msg = '; '.join(problems) if problems else (f'injected at {inj}, detected at {frames[0]}; '
                                                    f'reports replay: host clean, join diverges at {inj} (p1.hp)')
        return ok, msg, dt, sh, sj
    if rc_h or rc_j: problems.append(f'exit codes host {rc_h} join {rc_j}')
    bad = [f for f in both if H[f] != J[f]]
    if bad: problems.append(f'peers differ at {len(bad)} final frames, first {bad[0]}')
    badref = [f for f in both if f in O and O[f] != H[f]]
    if badref: problems.append(f'differs from the offline run at {len(badref)} frames, first {badref[0]}')
    if len(both) < 0.95 * rp.frames: problems.append(f'only {len(both)} of {rp.frames} frames final on both')
    for s, who in ((sh, 'host'), (sj, 'join')):
        if s and s['desyncs']: problems.append(f'{who}: {int(s["desyncs"])} desyncs')
        if s and s['impossible'] and not expect.get('impossible_ok'): problems.append(f'{who}: {int(s["impossible"])} impossible rollbacks')
    if expect.get('menu'):
        for who, log in (('host', hlog), ('join', jlog)):
            ref = re.findall(r'menu: REFUSED (\S+):', log)
            if sorted(ref) != ['load', 'save-delete', 'save-export', 'save-import']:
                problems.append(f'{who}: refused {ref}, expected load / save-import / save-export / save-delete')
            if 'menu: REFUSED widescreen' in log or 'menu: REFUSED reveal' in log:
                problems.append(f'{who}: refused a presentation-only action')
            if 'restart' in log.lower() and 'menu: load' in log: problems.append(f'{who}: load ran')
    full = sum(1 for f in both if len(H[f]) > 1)
    dt = time.time() - t0
    msg = f'{len(both)} final frames equal on both peers and offline ({full} full-state hashes)'
    if sh and sj:
        msg += (f'; rollbacks {int(sh["rollbacks"])}/{int(sj["rollbacks"])} (max depth {int(max(sh["max_depth"], sj["max_depth"]))}), '
                f're-sim {max(sh["resim_ms_per_frame"], sj["resim_ms_per_frame"]):.3f} ms/vblank, '
                f'stalls {int(sh["stalls"])}/{int(sj["stalls"])}')
    return not problems, '; '.join(problems) if problems else msg, dt, sh, sj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cases', nargs='*')
    ap.add_argument('-j', type=int, default=3)
    ap.add_argument('--pace', type=float, default=240)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        for n, c in CASES.items(): print(f'{n:8s} {c[0]} delay {c[1]} sim {c[2]} / {c[3]}')
        return 0
    names = a.cases or list(CASES)
    base = 47100 + (os.getpid() % 50) * 20
    t0 = time.time()
    fail = 0
    with cf.ThreadPoolExecutor(a.j) as ex:
        futs = [ex.submit(run_case, n, base + i, a.pace) for i, n in enumerate(names)]
        for n, fu in zip(names, futs):
            ok, msg, dt, sh, sj = fu.result()
            print(f'{"ok  " if ok else "FAIL"} {n:8s} {dt:5.1f}s {msg}')
            fail += not ok
    print(f'netplay: {len(names) - fail}/{len(names)} ok ({time.time() - t0:.0f}s, pace {a.pace:g} Hz)')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
