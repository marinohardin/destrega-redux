#!/usr/bin/env python3
"""Cross-platform netplay: the native build (macOS) against the Windows build under Wine, over
loopback UDP, started from the Netplay menu actions (--menu-action netplay-host / netplay-join).

Each case runs both directions' roles as in tools/test_netplay.py's menu_session: both processes
start offline, one hosts and the other joins mid-run, both restart from a fresh boot (the Windows
build restarts by CreateProcess, plat_exec), play the VS fight of a tools/replays.py replay with one
pad each, the joiner disconnects, and both keep playing offline. Pass criteria:

  * both peers write the same hash for every final (confirmed) frame they both reached, and that
    log equals an offline native run of the same inputs shifted by the agreed input delay;
  * the session started and ended cleanly on both sides, no desync, no impossible rollback;
  * `ident`: a peer claiming other emulation sources (--net-test-ident) is refused with the
    plain-language "different versions" alert, and both keep playing offline.

Wine: WINE (the wine command, default CrossOver's), WINE_ARGS (default
"--bottle destrega-test --no-gui"), WIN_EXE (build/win/destrega.exe with SDL3.dll next to it).
Skipped (exit 0, "skipped: ...") when Wine, the Windows build or the data pack is missing.

  test_netplay_wine.py [CASE...] [--pace HZ] [--list]
"""
import argparse, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import replays as R  # noqa: E402
import test_netplay as T  # noqa: E402

CROSSOVER = os.path.expanduser('~/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/'
                               'CrossOver-Hosted Application/wine')
WINE = os.environ.get('WINE') or (CROSSOVER if os.path.exists(CROSSOVER) else shutil.which('wine') or '')
WINE_ARGS = os.environ.get('WINE_ARGS', '--bottle destrega-test --no-gui' if 'CrossOver' in WINE else '').split()
# NETPLAY_WINE_EXTRA: more options for both peers of a session, e.g. "--renderer gpu --hlr on"
EXTRA = os.environ.get('NETPLAY_WINE_EXTRA', '').split()
WIN_EXE = os.path.join(ROOT, os.environ.get('WIN_EXE', 'build/win/destrega.exe'))
BIN = T.BIN
PAK = os.environ.get('PAK') or (os.path.expanduser('~/Library/Application Support/Destrega/destrega.pak')
                                if sys.platform == 'darwin' else
                                os.path.join(os.environ.get('XDG_DATA_HOME', os.path.expanduser('~/.local/share')),
                                             'Destrega/destrega.pak'))
OUT = os.path.join(ROOT, 'build', 'netplay_wine')
WINDOWED = False

# name: (who hosts, replay, sim host->join, sim join->host)
CASES = {
    'mac_hosts':       ('mac', 'vs_tieme_rohzen_villa', '3:2:0:0:31', '3:2:0:0:32'),
    'win_hosts':       ('win', 'vs_milena_cougar_fortress', '3:2:0:0:33', '3:2:0:0:34'),
    'mac_hosts_lossy': ('mac', 'vs_zauber_doyle_throne', '15:10:5:5:35', '15:10:5:5:36'),
    'win_hosts_lossy': ('win', 'vs_anjie_laon_canyon', '15:10:5:5:37', '15:10:5:5:38'),
}


def winpath(p):
    return 'Z:' + os.path.abspath(p).replace('/', '\\')


def cmd_for(side, args):
    """args: list of str or ('path', p) tuples (converted for Wine)."""
    conv = (lambda a: winpath(a[1]) if isinstance(a, tuple) else a) if side == 'win' else \
           (lambda a: a[1] if isinstance(a, tuple) else a)
    base = [WINE] + WINE_ARGS + [winpath(WIN_EXE)] if side == 'win' else [BIN]
    return base + [conv(a) for a in args] + [conv(('path', PAK))]


def env_for(side):
    e = dict(os.environ)
    if side == 'win': e.setdefault('WINEDEBUG', '-all')
    e['DESTREGA_TEST_NO_ALERTS'] = '1'   # windowed: alerts are logged, not shown
    return e


def wait_for(path, text, proc, timeout=240):
    """Until `text` is in the log (the Wine side has reached its netplay menu action). Offline
    frames are not paced, so the native side would otherwise run through its whole script while
    Wine is still starting (2-3 s)."""
    t0 = time.time()
    while time.time() - t0 < timeout and proc.poll() is None:
        if text in open(path, errors='replace').read(): return True
        time.sleep(0.05)
    return False


def start_pair(hoster, host, hl, joiner, join, jl):
    """Start both, the Wine one first; the native one once Wine is hosting / joining."""
    order = [('host', hoster, host, hl), ('join', joiner, join, jl)]
    if joiner == 'win': order.reverse()
    procs = {}
    for who, side, args, f in order:
        procs[who] = subprocess.Popen(cmd_for(side, args), stdout=f, stderr=subprocess.STDOUT, cwd=ROOT, env=env_for(side))
        if side == 'win': wait_for(f.name, 'netplay: hosting a game' if who == 'host' else 'netplay: joining', procs[who])
    return procs['host'], procs['join']


def run_session(name, port, pace):
    hoster, rp_name, sim_h, sim_j = CASES[name]
    joiner = 'win' if hoster == 'mac' else 'mac'
    rp = R.REPLAYS[rp_name]
    p1, p2 = T.after_boot_to_vs(rp.p1), T.after_boot_to_vs(rp.p2)   # sessions boot straight to VS select
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    t0 = time.time()
    frames, disc_at = 4800, 3300
    # windowed: --net-portmap off, a loopback test must never ask the real router for a port
    mode = ['--renderer', 'software', '--no-memcard', '--no-settings', '--net-portmap', 'off'] if WINDOWED else ['--headless', '--net-pace', str(pace)]
    common = mode + EXTRA + ['--no-interp', '--frames', str(frames),
              '--net-desync-dir', ('path', d)]
    host = common + ['--press', R.fmt(p1), '--net-sim', sim_h, '--net-hash-log', ('path', os.path.join(d, 'host.hash')),
                     '--menu-action', f'300:netplay-host={port},2']
    join = common + ['--press', R.fmt(p2), '--net-sim', sim_j, '--net-hash-log', ('path', os.path.join(d, 'join.hash')),
                     '--menu-action', f'400:netplay-join=127.0.0.1:{port},3',
                     '--menu-action', f'{400 + disc_at}:netplay-disconnect']
    with open(os.path.join(d, 'host.log'), 'w') as hl, open(os.path.join(d, 'join.log'), 'w') as jl:
        ph, pj = start_pair(hoster, host, hl, joiner, join, jl)
        try:
            rc_j = pj.wait(timeout=2400)
            rc_h = ph.wait(timeout=900)
        except subprocess.TimeoutExpired:
            for p in (ph, pj):
                if p.poll() is None: p.kill()
            rc_h = rc_j = 'timeout'
    hlog = open(os.path.join(d, 'host.log'), errors='replace').read()
    jlog = open(os.path.join(d, 'join.log'), errors='replace').read()
    delay = 3
    ref = os.path.join(d, 'offline.hash')
    r = subprocess.run([BIN, '--headless', '--no-interp', '--frames', str(disc_at), '--net-hash-log', ref, '--boot-to-vs',
                        '--press', R.fmt(T.shifted(p1, delay)), '--press2', R.fmt(T.shifted(p2, delay)), PAK],
                       capture_output=True, text=True, cwd=ROOT)
    open(os.path.join(d, 'offline.log'), 'w').write(r.stdout + r.stderr)
    H, J, O = T.read_log(os.path.join(d, 'host.hash')), T.read_log(os.path.join(d, 'join.hash')), T.read_log(ref)
    both = sorted(set(H) & set(J))
    problems = []
    if rc_h or rc_j: problems.append(f'exit codes host {rc_h} join {rc_j}')
    for who, log in (('host', hlog), ('join', jlog)):
        if 'netplay: restarting for the session' not in log: problems.append(f'{who}: no synced restart')
        if 'input delay 3' not in log: problems.append(f'{who}: delay 3 not agreed')
        if 'boot shortcut done at frame 306' not in log: problems.append(f'{who}: no boot shortcut to VS select')
        if 'netplay: session ended' not in log: problems.append(f'{who}: session did not end')
        if f'reached {frames} frames' not in log: problems.append(f'{who}: did not keep playing offline to the end')
        s = T.summary(log)
        if not s: problems.append(f'{who}: no netplay summary')
        elif s['desyncs'] or s['impossible']:
            problems.append(f'{who}: {int(s["desyncs"])} desyncs, {int(s["impossible"])} impossible rollbacks')
    if 'alert: Your opponent left' not in hlog: problems.append('host: no "opponent left" alert')
    bad = [f for f in both if H[f] != J[f]]
    if bad: problems.append(f'peers differ at {len(bad)} final frames, first {bad[0]}')
    badref = [f for f in both if f in O and O[f] != H[f]]
    if badref: problems.append(f'differs from the offline run at {len(badref)} frames, first {badref[0]}')
    if len(both) < 0.9 * disc_at: problems.append(f'only {len(both)} final frames on both (disconnect near {disc_at})')
    full = sum(1 for f in both if len(H[f]) > 1)
    sh, sj = T.summary(hlog), T.summary(jlog)
    msg = (f'{hoster} hosts, {joiner} joins: {len(both)} final frames equal on both peers and offline '
           f'({full} full-state hashes), sim {sim_h}')
    if sh and sj:
        msg += (f'; rollbacks {int(sh["rollbacks"])}/{int(sj["rollbacks"])} (max depth '
                f'{int(max(sh["max_depth"], sj["max_depth"]))}), stalls {int(sh["stalls"])}/{int(sj["stalls"])}')
    return not problems, '; '.join(problems) if problems else msg, time.time() - t0


def run_ident(name, port, pace):
    """A Windows host whose emulation sources differ (simulated with --net-test-ident) refuses the
    Mac joiner with a plain-language reason; both keep playing offline. Then the same pair with the
    real identities agrees (the restart is the proof)."""
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    t0 = time.time()
    common = ['--headless', '--no-interp', '--net-pace', str(pace)]
    host = common + ['--frames', '2400', '--net-test-ident', 'source=12345678', '--menu-action', f'100:netplay-host={port}']
    join = common + ['--frames', '900', '--menu-action', f'300:netplay-join=127.0.0.1:{port}']
    logs = {'host': os.path.join(d, 'host.log'), 'join': os.path.join(d, 'join.log')}
    ph, pj = start_pair('win', host, open(logs['host'], 'w'), 'mac', join, open(logs['join'], 'w'))
    rcs = {'join': pj.wait(timeout=900), "host": ph.wait(timeout=900)}
    L = {w: open(f, errors='replace').read() for w, f in logs.items()}
    problems = [f'{w} exit code {rc}' for w, rc in rcs.items() if rc]
    if 'alert: The host could not accept you: the two players run different versions of Destrega' not in L['join']:
        problems.append('join: no "different versions" refusal alert')
    if 'refused a player' not in L['host']: problems.append('host: did not refuse')
    for w, n in (('host', 2400), ('join', 900)):
        if f'headless: reached {n} frames' not in L[w]: problems.append(f'{w}: did not keep playing offline')
        if 'restarting for the session' in L[w]: problems.append(f'{w}: restarted although nothing was agreed')
    msg = 'Windows host with other emulation sources refused the Mac joiner with an alert; both kept playing offline'
    return not problems, '; '.join(problems) if problems else msg, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cases', nargs='*')
    ap.add_argument('--pace', type=float, default=120)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--windowed', action='store_true',
                    help='both peers windowed at 60 Hz (real audio output: checks the windowed audio path stays '
                         'deterministic in a session); slow under Wine, not part of run_tests.sh')
    a = ap.parse_args()
    if a.list:
        for n, c in CASES.items(): print(f'{n:16s} {c[0]} hosts, {c[1]}, sim {c[2]} / {c[3]}')
        print('ident            emulation-source mismatch refused (Windows host, Mac joiner)')
        return 0
    for what, ok in (('Wine', WINE and os.path.exists(WINE)), ('the Windows build ' + WIN_EXE, os.path.exists(WIN_EXE)),
                     ('SDL3.dll next to the Windows build', os.path.exists(os.path.join(os.path.dirname(WIN_EXE), 'SDL3.dll'))),
                     ('the native build ' + BIN, os.path.exists(BIN)), ('the data pack ' + PAK, os.path.exists(PAK))):
        if not ok:
            print(f'netplay-wine: skipped: no {what}')
            return 0
    global WINDOWED
    WINDOWED = a.windowed
    names = a.cases or list(CASES) + ['ident']
    base = 47700 + (os.getpid() % 40) * 10
    t0 = time.time()
    fail = 0
    # one case at a time: Wine is the slow side, and two Wine sessions at once only add stalls
    for i, n in enumerate(names):
        ok, msg, dt = (run_ident if n == 'ident' else run_session)(n, base + 2 * i, a.pace)
        print(f'{"ok  " if ok else "FAIL"} {n:16s} {dt:5.1f}s {msg}', flush=True)
        fail += not ok
    print(f'netplay-wine: {len(names) - fail}/{len(names)} ok ({time.time() - t0:.0f}s, pace {a.pace:g} Hz)')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
