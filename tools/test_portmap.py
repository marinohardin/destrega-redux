#!/usr/bin/env python3
"""Automatic port forwarding for the netplay host (runtime/portmap.c) against a fake router.

Every case starts tools/fake_router.py (PCP + NAT-PMP over UDP, UPnP over HTTP, 127.0.0.1 only) in
this process and a headless game that hosts through the menu action, pointed at the fake router
with --net-portmap-gateway / --net-portmap-upnp. Nothing is ever sent to a real router: headless
runs only map ports when given those overrides, and with an override the game tries nothing else
(no gateway lookup, no SSDP multicast).

Cases: natpmp, pcp, upnp, upnp_permanent, refused, cgnat, noanswer (frame loop not stalled),
off, no_override / plain (headless without overrides, with --net-portmap on or with no
port-mapping argument at all: nothing), restart (the menu session restart
keeps the mapping: the new process renews it with the same PCP nonce and deletes it when the
opponent leaves).

Every basic case also checks Netplay > Copy My Address (menu action netplay-copy-address; headless
runs only log what they would copy): refused before hosting; while hosting, the router's internet
address when it gave a public one, else this computer's home-network address, labelled as such with
the reason (CGNAT, refused, no answer, port opening off).

  test_portmap.py [CASE...] [-j N] [--win]     (--win: the host is build/win/destrega.exe under
                                                Wine, see tools/test_netplay_wine.py for WINE /
                                                WINE_ARGS / WIN_EXE / PAK)
"""
import argparse, concurrent.futures as cf, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from fake_router import FakeRouter  # noqa: E402

BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))
OUT = os.path.join(ROOT, 'build', 'portmap')
WIN = False
LEASE = '2'   # seconds: renewals every second, so a few seconds of hosting show them


def host_cmd(args, side=None):
    """The hosting game: native, or the Windows build under Wine with --win."""
    side = side or ('win' if WIN else 'mac')
    if side == 'win':
        import test_netplay_wine as W
        return W.cmd_for('win', args), W.env_for('win')
    return [BIN] + args, dict(os.environ)


def run_host(name, args, timeout=900):
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    cmd, env = host_cmd(['--headless', '--no-interp'] + args)
    log = os.path.join(d, 'host.log')
    t0 = time.time()
    with open(log, 'w') as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT, env=env, timeout=timeout).returncode
    return rc, open(log, errors='replace').read(), time.time() - t0


def ops(r, proto):
    return [e['op'] for e in r.events if e['proto'] == proto]


def common_checks(problems, rc, log, frames):
    if rc: problems.append(f'exit code {rc}')
    if f'headless: reached {frames} frames' not in log: problems.append('the game did not keep running to the end')


# what Copy My Address must copy per case: 'internet' (the router's address) or the reason given
# with the home-network address
COPY = {'natpmp': 'internet', 'pcp': 'internet', 'upnp': 'internet', 'upnp_permanent': 'internet',
        'refused': 'the router did not open the port', 'noanswer': 'the router did not open the port',
        'cgnat': "your internet provider doesn't allow incoming connections",
        'off': 'automatic port opening is off', 'no_override': 'automatic port opening is off',
        'plain': 'automatic port opening is off'}


def check_copy(problems, name, log, port):
    if 'menu: REFUSED netplay-copy-address: not hosting a game' not in log:
        problems.append('copy address: not refused before hosting')
    want = COPY.get(name)
    if not want: return
    if want == 'internet':
        if f'netplay: copy address: 203.0.113.45:{port} (your internet address, from the router)' not in log:
            problems.append('copy address: the router\'s address was not copied')
        if 'Give your opponent this address: 203.0.113.45' not in log: problems.append('panel: no address for the opponent')
        return
    m = re.search(r'netplay: copy address: (\S+) \(your home-network address: (.*)\)', log)
    if not m:
        if 'This computer has no network address right now' not in log:
            problems.append('copy address: no home-network address copied and no explanation')
        return
    if not re.fullmatch(rf'\d+\.\d+\.\d+\.\d+:{port}', m.group(1)): problems.append(f'copy address: copied {m.group(1)}')
    if '100.72.1.9' in m.group(1): problems.append('copy address: copied the unreachable CGNAT address')
    if want not in m.group(2): problems.append(f'copy address: reason "{want}" missing in "{m.group(2)}"')
    if 'Players outside your home network need your internet address' not in log:
        problems.append('panel: does not say that players outside need the internet address')


def case_basic(name, port, router_kw, use_gw, use_upnp, expect, disc_at=None, frames=14000, extra=()):
    """Host from frame 30; cancel at disc_at (None = exit while hosting). Copy My Address at frame 10
    (refused) and shortly before the end of hosting."""
    r = FakeRouter(**router_kw)
    copy_at = (disc_at or frames) - 1000
    args = ['--frames', str(frames), '--menu-action', f'30:netplay-host={port},2',
            '--menu-action', '10:netplay-copy-address', '--menu-action', f'{copy_at}:netplay-copy-address']
    if use_gw or use_upnp: args += ['--net-portmap-lease', LEASE]   # 'plain': no port-mapping argument at all
    if use_gw: args += ['--net-portmap-gateway', r.gateway]
    if use_upnp: args += ['--net-portmap-upnp', r.upnp_url]
    if disc_at: args += ['--menu-action', f'{disc_at}:netplay-disconnect']
    args += list(extra)
    try:
        rc, log, dt = run_host(name, args)
    finally:
        r.close()
    problems = []
    common_checks(problems, rc, log, frames)
    expect(problems, r, log, port)
    check_copy(problems, name, log, port)
    return problems, dt, r.events


def lifecycle(proto, renew=True):
    """map (+ renewals) then a delete, nothing after"""
    def check(problems, r, log, port):
        o = ops(r, proto)
        if 'map' not in o: problems.append(f'{proto}: no mapping made ({o})'); return
        if renew and 'renew' not in o: problems.append(f'{proto}: never renewed ({o})')
        if not renew and 'renew' in o: problems.append(f'{proto}: renewed a permanent mapping ({o})')
        if not o or o[-1] != 'delete': problems.append(f'{proto}: the mapping was not deleted at the end ({o})')
        if o.count('map') != 1: problems.append(f'{proto}: mapped {o.count("map")} times')
        if f'Your address for the opponent: 203.0.113.45:{port}' not in log: problems.append('no address shown for the opponent')
        if not re.search(rf'portmap: \S+ mapping of port {port} deleted', log): problems.append('game did not log the delete')
        pcp_nonces = {e.get('nonce') for e in r.events if e['proto'] == 'pcp' and e['op'] in ('map', 'renew', 'delete')}
        if proto == 'pcp' and len(pcp_nonces) != 1: problems.append(f'PCP nonce changed: {pcp_nonces}')
    return check


def no_udp(check):
    def c(problems, r, log, port):
        check(problems, r, log, port)
        if ops(r, 'pcp') or ops(r, 'natpmp'): problems.append('UDP protocols tried although only UPnP was given')
    return c


def expect_text(text, no_map=True):
    def check(problems, r, log, port):
        if text.format(port=port) not in log: problems.append(f'missing "{text.format(port=port)}"')
        if no_map and any(e['op'] in ('map', 'renew') for e in r.events): problems.append('a mapping was made')
    return check


def check_cgnat(problems, r, log, port):
    if "Your internet provider doesn't allow incoming connections" not in log: problems.append('no CGNAT message')
    if '100.72.1.9' not in log: problems.append('the reported address is not mentioned')
    if 'Your address for the opponent' in log: problems.append('an unreachable address was offered to the opponent')
    o = ops(r, 'natpmp')
    if 'map' not in o or o[-1] != 'delete': problems.append(f'natpmp: mapping not made / deleted ({o})')


def check_noanswer(problems, r, log, port):
    if f"Your router didn't answer; you may need to forward UDP port {port}" not in log:
        problems.append('no "router didn\'t answer" message')
    m = re.search(r'frame loop: (\d+) frames ran while the router was being asked, longest gap between frames ([\d.]+) ms', log)
    if not m: problems.append('no frame-loop measurement'); return
    n, gap = int(m.group(1)), float(m.group(2))
    # A blocking call would hold one frame for a whole timeout (PCP 1.75 s, HTTP 3 s). A loaded
    # machine (run_tests.sh runs everything at once) makes frames slow and uneven, so the limits
    # stay well below the timeouts and well above scheduling noise.
    if n < 100: problems.append(f'only {n} frames ran while waiting for the router')
    if gap > 1000: problems.append(f'the frame loop stalled {gap} ms while waiting for the router')
    check_noanswer.last = f'{n} frames ran while PCP, NAT-PMP and UPnP timed out, longest gap between frames {gap} ms'


def check_off(problems, r, log, port):
    if r.events: problems.append(f'the router was contacted: {r.events[:3]}')
    if 'Your address for the opponent' in log: problems.append('address shown although off')


def check_no_override(problems, r, log, port):
    if 'portmap:' in log: problems.append('portmap ran in a headless run without a test router')
    if r.events: problems.append('the router was contacted')


def case_restart(name, port, *_):
    """Menu session: host + join through --menu-action, both restart, the session runs, the joiner
    disconnects (the host gets "opponent left"), both continue offline."""
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    r = FakeRouter(pcp='ok')
    frames = 1500
    common = ['--headless', '--no-interp', '--frames', str(frames), '--net-pace', '240']
    hcmd, henv = host_cmd(common + ['--net-portmap-gateway', r.gateway, '--net-portmap-lease', LEASE,
                                    '--menu-action', f'30:netplay-host={port},2'])
    jcmd, jenv = host_cmd(common + ['--menu-action', f'400:netplay-join=127.0.0.1:{port},2',
                                    '--menu-action', '1000:netplay-disconnect'], side='mac')
    t0 = time.time()
    hl, jl = os.path.join(d, 'host.log'), os.path.join(d, 'join.log')
    try:
        with open(hl, 'w') as hf, open(jl, 'w') as jf:
            ph = subprocess.Popen(hcmd, stdout=hf, stderr=subprocess.STDOUT, cwd=ROOT, env=henv)
            if WIN:   # Wine needs a few seconds to start: join once the host is hosting
                while ph.poll() is None and 'hosting a game on port' not in open(hl, errors='replace').read() and time.time() - t0 < 120:
                    time.sleep(0.2)
            pj = subprocess.Popen(jcmd, stdout=jf, stderr=subprocess.STDOUT, cwd=ROOT, env=jenv)
            rc_j = pj.wait(timeout=900)
            rc_h = ph.wait(timeout=900)
    finally:
        r.close()
    hlog, jlog = open(hl, errors='replace').read(), open(jl, errors='replace').read()
    problems = []
    if rc_h or rc_j: problems.append(f'exit codes host {rc_h} join {rc_j}')
    if 'netplay: restarting for the session' not in hlog: problems.append('host did not restart into the session')
    if f'keeping the PCP mapping of port {port}' not in hlog: problems.append('old process did not keep the mapping')
    if f'renewing the PCP mapping of port {port} made before the restart' not in hlog: problems.append('new process did not adopt it')
    if 'alert: Your opponent left' not in hlog: problems.append('host: no "opponent left"')
    if 'portmap:' in jlog: problems.append('the joiner mapped a port')
    o = ops(r, 'pcp')
    nonces = {e.get('nonce') for e in r.events if e['proto'] == 'pcp' and e['op'] in ('map', 'renew', 'delete')}
    if o.count('map') != 1: problems.append(f'mapped {o.count("map")} times (the restart must keep it): {o}')
    if 'nonce_mismatch' in o: problems.append('the new process used another PCP nonce')
    if len(nonces) != 1: problems.append(f'nonces {nonces}')
    if not o or o[-1] != 'delete' or o.count('delete') != 1: problems.append(f'not deleted exactly once at the end: {o}')
    # the delete must come after the session ended (the opponent left), not at the restart
    t_del = [e['t'] for e in r.events if e['op'] == 'delete']
    t_ren = [e['t'] for e in r.events if e['op'] == 'renew']
    if t_del and t_ren and t_del[0] < max(t_ren): problems.append('a renewal came after the delete')
    if f'headless: reached {frames} frames' not in hlog: problems.append('host did not keep playing offline')
    return problems, time.time() - t0, r.events


CASES = {
    'natpmp':         (case_basic, dict(pcp='unsupported', natpmp='ok'), True, False, lifecycle('natpmp'), 7000),
    'pcp':            (case_basic, dict(pcp='ok'), True, False, lifecycle('pcp'), None),
    'upnp':           (case_basic, dict(upnp='ok'), False, True, no_udp(lifecycle('upnp')), 7000),
    'upnp_permanent': (case_basic, dict(upnp='permanent'), False, True, lifecycle('upnp', renew=False), None),
    'refused':        (case_basic, dict(pcp='unsupported', natpmp='refuse', upnp='refuse'), True, True,
                       expect_text('Your router refused to open port {port}'), 7000),
    'cgnat':          (case_basic, dict(pcp='unsupported', natpmp='ok', ext_ip='100.72.1.9'), True, False, check_cgnat, None),
    'noanswer':       (case_basic, dict(pcp='silent', natpmp='silent', upnp='silent'), True, True, check_noanswer, None),
    'off':            (case_basic, dict(), True, True, check_off, None),
    'no_override':    (case_basic, dict(), False, False, check_no_override, None),
    'plain':          (case_basic, dict(), False, False, check_no_override, None),
    'restart':        (case_restart,),
}
EXTRA = {'off': ['--net-portmap', 'off'], 'noanswer': [], 'no_override': ['--net-portmap', 'on']}
FRAMES = {'noanswer': 20000}


def run(name, port):
    c = CASES[name]
    if c[0] is case_restart:
        problems, dt, ev = case_restart(name, port)
    else:
        _, kw, gw, up, expect, disc = c
        problems, dt, ev = case_basic(name, port, kw, gw, up, expect, disc, FRAMES.get(name, 8000), EXTRA.get(name, ()))
    summ = ' '.join(f"{e['proto']}:{e['op']}" for e in ev)
    if len(summ) > 160: summ = summ[:157] + '...'
    if name == 'noanswer' and getattr(check_noanswer, 'last', None): summ = check_noanswer.last
    return name, problems, dt, summ


def main():
    global WIN
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('cases', nargs='*')
    a.add_argument('-j', type=int, default=5)
    a.add_argument('--win', action='store_true')
    o = a.parse_args()
    WIN = o.win
    if WIN:
        import test_netplay_wine as W
        if not W.WINE or not os.path.exists(W.WIN_EXE):
            print('portmap (Windows build under Wine): skipped (no Wine or no build/win/destrega.exe)')
            return 0
    elif not os.path.exists(BIN):
        print(f'portmap: no {BIN}'); return 1
    names = o.cases or list(CASES)
    fail = 0
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=o.j) as ex:
        futs = [ex.submit(run, n, 31000 + 17 * i + (500 if WIN else 0)) for i, n in enumerate(names)]
        for f in futs:
            name, problems, dt, summ = f.result()
            ok = not problems
            fail |= not ok
            print(f'{name:15s} {"ok  " if ok else "FAIL"} {dt:5.1f}s  {"; ".join(problems) if problems else summ}', flush=True)
    print(f'portmap{" (Windows build under Wine)" if WIN else ""}: {len(names)} cases, '
          f'{"PASS" if not fail else "FAIL"} ({time.time() - t0:.0f} s)')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
