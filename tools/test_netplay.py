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
    # same fault with other link timings: the peer's hash packet reaches the faulty peer at different points of the
    # vblank (regression: a report written before its frame was recorded had a replay file one frame short)
    'desync_b':   ('vs_tieme_rohzen_villa', 2, '1:0:0:0:31', '1:0:0:0:32', [], {'desync': True}),
    'desync_c':   ('vs_tieme_rohzen_villa', 1, '30:20:0:0:33', '2:1:0:0:34', [], {'desync': True}),
    # state-changing menu actions are refused during netplay, presentation-only ones run
    'menu':       ('vs_tieme_rohzen_villa', 2, '2:1:0:0:13', '2:1:0:0:14',
                   ['--menu-action', '1500:load=Destrega/Destrega.cue', '--menu-action', '1510:save-import=x.mcd',
                    '--menu-action', '1520:save-export=build/netplay/menu/x.mcd', '--menu-action', '1530:save-delete',
                    '--menu-action', '1540:widescreen=on', '--menu-action', '1550:reveal-saves'],
                   {'frames': 2400, 'menu': True}),
}


def shifted(steps, d):
    return [(f + d, m) for f, m in steps]


# A session boots straight to VS mode's character select (netplay.c k_boot_vs; --boot-to-vs offline):
# it is up at frame ~380 instead of ~1560 with the normal title path of tools/scenarios.py, so the
# character / stage picks (from frame 1750 on) and the fight of a VS replay come BOOT_VS_SAVES earlier.
BOOT_VS_SAVES = 1180
BOOT_VS_READY = 400        # the character select is on screen by this frame


def after_boot_to_vs(steps):
    return [(f - BOOT_VS_SAVES, m) for f, m in steps if f >= 1700]


def is_vs_select(png):
    """VS character select: P1's portrait on red at the left, P2's on blue at the right (title, 1P
    select and loading screens have neither)."""
    import scenarios as S
    r, g, b = S.region_mean(png, (0.6, 0.1, 0.69, 0.15))       # top of P2's frame
    r1, g1, b1 = S.region_mean(png, (0.02, 0.1, 0.06, 0.15))   # top left of P1's frame
    return b > 60 and b > 2 * r and r1 > 60 and r1 > 2 * b1


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
    for f in os.listdir(d):
        if f.startswith('desync-'): os.remove(os.path.join(d, f))
    t0 = time.time()
    common = [BIN, '--headless', '--no-interp', '--frames', str(rp.frames), '--net-delay', str(delay),
              '--net-pace', str(pace), '--net-desync-dir', d, '--net-full-boot'] + extra + os.environ.get('NETPLAY_EXTRA', '').split()
    host = common + ['--press', R.fmt(rp.p1), '--host', str(port), '--net-sim', sim_h,
                     '--net-hash-log', os.path.join(d, 'host.hash')]
    join = common + ['--press', R.fmt(rp.p2), '--join', f'127.0.0.1:{port}', '--net-sim', sim_j,
                     '--net-hash-log', os.path.join(d, 'join.hash')]
    if expect.get('desync'):
        join += ['--net-inject-desync', str(rp.frames // 2)]
        for side in (host, join):   # Netplay > Copy Out-of-Sync Report Location (headless: logged only)
            side += ['--menu-action', f'{rp.frames - 5}:netplay-copy-report']
    with open(os.path.join(d, 'host.log'), 'w') as hl, open(os.path.join(d, 'join.log'), 'w') as jl:
        ph = subprocess.Popen(host, stdout=hl, stderr=subprocess.STDOUT, cwd=ROOT)
        pj = subprocess.Popen(join, stdout=jl, stderr=subprocess.STDOUT, cwd=ROOT)
        rc_j = pj.wait(timeout=2400)
        rc_h = ph.wait(timeout=600)
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
            nfr = int(re.search(r'desync-(\d+)-', found[0]).group(1))
            hdr = [l for l in open(path) if l.startswith('frames ')]
            if not hdr or int(hdr[0].split()[1]) < nfr: problems.append(f'{who} report replay file holds fewer frames than the reported frame {nfr}')
            r = subprocess.run([BIN, '--headless', '--no-interp', '--replay', path], capture_output=True, text=True, cwd=ROOT)
            log = r.stdout + r.stderr
            open(os.path.join(d, f'replay-{who}.log'), 'w').write(log)
            m = re.search(r'replay: DIVERGED at frame (\d+)', log)
            if want is None and (r.returncode or m): problems.append(f'{who} report does not replay cleanly')
            if want is not None and not (m and int(m.group(1)) == want and re.search(r'p1\.hp +DIFFERS', log)):
                problems.append(f'{who} report replay: expected a p1.hp divergence at {want}')
            wlog = hlog if who == 'host' else jlog
            c = re.search(r'netplay: copy report location: (\S+)', wlog)
            txt = os.path.join(os.path.abspath(d), found[0][:-4] + '.txt')
            if not c or os.path.normpath(c.group(1)) != txt: problems.append(f'{who}: copy report location: {c.group(1) if c else "nothing"}, expected {txt}')
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


def run_menu_session(name, port, pace):
    """Both processes start offline; one hosts and the other joins through the menu actions mid-run
    (Netplay > Host Game / Join Game); both restart from a fresh boot that goes straight to the VS
    character select (the default boot shortcut), play synced, then the joiner disconnects and both
    keep playing offline until --frames."""
    rp = R.REPLAYS['vs_tieme_rohzen_villa']
    p1, p2 = after_boot_to_vs(rp.p1), after_boot_to_vs(rp.p2)
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    t0 = time.time()
    frames, disc_at = 4800, 3300           # frames of the new boot; disconnect around frame disc_at
    common = [BIN, '--headless', '--no-interp', '--frames', str(frames), '--net-pace', str(pace),
              '--net-desync-dir', d]
    host = common + ['--press', R.fmt(p1), '--net-sim', '3:2:0:0:21', '--net-hash-log', os.path.join(d, 'host.hash'),
                     '--menu-action', f'300:netplay-host={port},2']
    join = common + ['--press', R.fmt(p2), '--net-sim', '3:2:0:0:22', '--net-hash-log', os.path.join(d, 'join.hash'),
                     '--menu-action', f'400:netplay-join=127.0.0.1:{port},3',
                     '--menu-action', f'{400 + disc_at}:netplay-disconnect']
    with open(os.path.join(d, 'host.log'), 'w') as hl, open(os.path.join(d, 'join.log'), 'w') as jl:
        ph = subprocess.Popen(host, stdout=hl, stderr=subprocess.STDOUT, cwd=ROOT)
        pj = subprocess.Popen(join, stdout=jl, stderr=subprocess.STDOUT, cwd=ROOT)
        rc_j = pj.wait(timeout=1800)
        rc_h = ph.wait(timeout=600)
    hlog = open(os.path.join(d, 'host.log')).read()
    jlog = open(os.path.join(d, 'join.log')).read()
    delay = 3                              # the larger of the two delays is agreed
    ref = os.path.join(d, 'offline.hash')
    dump = os.path.join(d, 'offline_dump')
    os.makedirs(dump, exist_ok=True)
    r = subprocess.run([BIN, '--headless', '--no-interp', '--frames', str(disc_at), '--net-hash-log', ref, '--boot-to-vs',
                        '--dump', dump, '--dump-every', str(BOOT_VS_READY), '--scale', '1',
                        '--press', R.fmt(shifted(p1, delay)), '--press2', R.fmt(shifted(p2, delay))],
                       capture_output=True, text=True, cwd=ROOT)
    open(os.path.join(d, 'offline.log'), 'w').write(r.stdout + r.stderr)
    H, J, O = read_log(os.path.join(d, 'host.hash')), read_log(os.path.join(d, 'join.hash')), read_log(ref)
    both = sorted(set(H) & set(J))
    problems = []
    if rc_h or rc_j: problems.append(f'exit codes host {rc_h} join {rc_j}')
    for who, log in (('host', hlog), ('join', jlog)):
        if 'netplay: restarting for the session' not in log: problems.append(f'{who}: no synced restart')
        if 'input delay 3' not in log: problems.append(f'{who}: delay 3 not agreed')
        if 'boot shortcut done at frame 306' not in log: problems.append(f'{who}: no boot shortcut to VS select')
        if 'netplay: session ended' not in log: problems.append(f'{who}: session did not end')
        if f'headless: reached {frames} frames' not in log: problems.append(f'{who}: did not keep playing offline to the end')
    if 'alert: Your opponent left' not in hlog: problems.append('host: no "opponent left" alert')
    if 'portmap:' in hlog + jlog: problems.append('a headless test tried to map a port on the real router')
    bad = [f for f in both if H[f] != J[f]]
    if bad: problems.append(f'peers differ at {len(bad)} final frames, first {bad[0]}')
    badref = [f for f in both if f in O and O[f] != H[f]]
    if badref: problems.append(f'differs from the offline run at {len(badref)} frames, first {badref[0]}')
    if len(both) < 0.9 * disc_at: problems.append(f'only {len(both)} final frames on both (disconnect near {disc_at})')
    if BOOT_VS_READY not in both: problems.append(f'frame {BOOT_VS_READY} not final on both peers')
    vs = os.path.join(dump, f'frame_{BOOT_VS_READY:06d}.png')
    if not os.path.exists(vs) or not is_vs_select(vs): problems.append(f'not on the VS character select at frame {BOOT_VS_READY}')
    if f'headless: reached {disc_at} frames' not in r.stdout + r.stderr: problems.append('offline reference did not finish')
    dt = time.time() - t0
    msg = (f'started from the menu (both rebooted, delay 3 agreed), both on the VS character select at frame '
           f'{BOOT_VS_READY} with equal hashes (boot shortcut, {BOOT_VS_SAVES} frames = {BOOT_VS_SAVES / 60:.1f} s '
           f'sooner), {len(both)} final frames equal on both peers and offline, disconnected near frame {disc_at}, '
           f'both continued offline to {frames}')
    return not problems, '; '.join(problems) if problems else msg, dt, None, None


# Join Game input the player might type or paste, and the alert each must give (netplay_ui.c)
BAD_JOINS = [('127.0.0.1:abc', '"abc" is not a port'), ('127.0.0.1', 'Add the port after the address'),
             ('my host:7000', '"my host" is not an address'), ('fe80::1:7000', 'looks like an IPv6 address'),
             ('127.0.0.1:70000', '"70000" is not a port'), ('127.0.0.1:7000,99', 'input delay is a number of frames')]


def run_menu_fail(name, port, pace):
    """Menu sessions that cannot start must alert and keep playing offline: a host with other
    settings refuses the joiner (plain-language reason), and joining a port nobody hosts times out.
    Also: Copy My Address is refused when not hosting and copies the home-network address while
    hosting without a router; mistyped Join Game addresses get a plain alert, and pasted whitespace
    is ignored. At most two game processes run at once (the lone joiner starts when the joiner is done)."""
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    t0 = time.time()
    common = [BIN, '--headless', '--no-interp', '--net-pace', str(pace)]
    host = common + ['--frames', '9000', '--net-window', '6', '--menu-action', f'100:netplay-host={port}',
                     '--menu-action', '50:netplay-copy-address', '--menu-action', '200:netplay-copy-address']
    join = common + ['--frames', '600', '--menu-action', f'300:netplay-join=  127.0.0.1:{port}\n']
    join += [x for i, (t, _) in enumerate(BAD_JOINS) for x in ('--menu-action', f'{150 + 10 * i}:netplay-join={t}')]
    # the join attempt gives up after --net-timeout + 5 s (20 s by default): 0.5 s here
    lone = common + ['--frames', '3000', '--net-timeout', '-4.5', '--menu-action', f'100:netplay-join=127.0.0.1:{port + 1}']
    logs = {}
    procs = {}
    for who, cmd in (('host', host), ('join', join), ('lone', lone)):
        if who == 'lone': procs['join'].wait(timeout=900)
        logs[who] = os.path.join(d, who + '.log')
        procs[who] = subprocess.Popen(cmd, stdout=open(logs[who], 'w'), stderr=subprocess.STDOUT, cwd=ROOT)
    rcs = {w: p.wait(timeout=900) for w, p in procs.items()}
    L = {w: open(f).read() for w, f in logs.items()}
    problems = [f'{w} exit code {rc}' for w, rc in rcs.items() if rc]
    if 'alert: The host could not accept you: the two players chose different netplay settings' not in L['join']:
        problems.append('join: no "different settings" refusal alert')
    if 'refused a player' not in L['host']: problems.append('host: did not refuse')
    if 'alert: No answer from 127.0.0.1' not in L['lone']: problems.append('lone joiner: no timeout alert')
    for t, want in BAD_JOINS:
        if not re.search(r'alert: [^\n]*' + re.escape(want), L['join']): problems.append(f'join {t!r}: no alert "{want}"')
    if L['join'].count('netplay: joining ') != 1: problems.append('join: a mistyped address was used, or the padded one was not')
    if 'menu: REFUSED netplay-copy-address: not hosting a game' not in L['host']: problems.append('host: copy not refused before hosting')
    c = re.search(r'netplay: copy address: (\S+) \(your home-network address', L['host'])
    if not (c and c.group(1).endswith(f':{port}')) and 'This computer has no network address' not in L['host']:
        problems.append('host: Copy My Address did not copy the home-network address')
    if any('portmap:' in v for v in L.values()): problems.append('a headless test tried to map a port on the real router')
    for w, n in (('host', 9000), ('join', 600), ('lone', 3000)):
        if f'headless: reached {n} frames' not in L[w]: problems.append(f'{w}: did not keep playing offline')
        if 'restarting for the session' in L[w]: problems.append(f'{w}: restarted although nothing was agreed')
    msg = ('settings mismatch refused with an alert, unanswered join timed out with an alert, all kept playing offline; '
           f'copy address refused / home network ({c.group(1) if c else "none"}), {len(BAD_JOINS)} mistyped joins alerted')
    return not problems, '; '.join(problems) if problems else msg, time.time() - t0, None, None


def run_ping(name, port, pace):
    """The ping shown to the players is the network round trip: packets are timestamped when they
    arrive (net.c receiver thread, kernel timestamps on macOS / Linux), not when the frame loop gets
    to them. Two short sessions over loopback: no simulated delay (must be well under a frame:
    < 3 ms on average, where it used to read 16-21 ms) and 20 ms each way (~40 ms)."""
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    t0 = time.time()
    links = (('loopback', None, 0.0, 3.0), ('sim20', '20:0:0:0:', 38.0, 47.0))
    procs = []
    for i, (tag, sim, lo, hi) in enumerate(links):
        common = [BIN, '--headless', '--no-interp', '--no-memcard', '--frames', '1500', '--net-pace', str(pace)]
        p = port + 100 + 2 * i
        h = common + ['--host', str(p)] + (['--net-sim', sim + '41'] if sim else [])
        j = common + ['--join', f'127.0.0.1:{p}'] + (['--net-sim', sim + '42'] if sim else [])
        for who, cmd in (('host', h), ('join', j)):
            f = os.path.join(d, f'{tag}-{who}.log')
            procs.append((tag, who, f, lo, hi, subprocess.Popen(cmd, stdout=open(f, 'w'), stderr=subprocess.STDOUT, cwd=ROOT)))
    problems, got = [], []
    for tag, who, f, lo, hi, pr in procs:
        rc = pr.wait(timeout=600)
        log = open(f).read()
        m = re.search(r'netplay: ping ([\d.]+) ms average network round trip \(median >?([\d.]+), min ([\d.]+), max ([\d.]+), (\d+) samples\), '
                      r'([\d.]+) ms frame-to-frame', log)
        if rc: problems.append(f'{tag} {who}: exit code {rc}')
        if not m: problems.append(f'{tag} {who}: no ping summary'); continue
        avg, n, frame = float(m.group(1)), int(m.group(5)), float(m.group(6))
        got.append(f'{tag} {who} {avg:.2f} ms (frame-to-frame {frame:.1f})')
        if n < 100: problems.append(f'{tag} {who}: only {n} ping samples')
        if not lo <= avg <= hi: problems.append(f'{tag} {who}: average network ping {avg:.2f} ms, expected {lo:g}..{hi:g}')
    msg = 'network ping ' + ', '.join(got)
    return not problems, '; '.join(problems) if problems else msg, time.time() - t0, None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cases', nargs='*')
    ap.add_argument('-j', type=int, default=3)
    ap.add_argument('--pace', type=float, default=240)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        for n, c in CASES.items(): print(f'{n:8s} {c[0]} delay {c[1]} sim {c[2]} / {c[3]}')
        print('menu_session  host / join / disconnect through --menu-action mid-run')
        print('menu_fail     refused / unanswered menu sessions')
        print('ping          the shown ping is the network round trip (loopback, 20 ms each way)')
        return 0
    names = a.cases or list(CASES) + ['menu_session', 'menu_fail', 'ping']
    base = 47100 + (os.getpid() % 50) * 20
    t0 = time.time()
    fail = 0
    with cf.ThreadPoolExecutor(a.j) as ex:
        fn = {'menu_session': run_menu_session, 'menu_fail': run_menu_fail, 'ping': run_ping}
        futs = [ex.submit(fn.get(n, run_case), n, base + 2 * i, a.pace)
                for i, n in enumerate(names)]
        for n, fu in zip(names, futs):
            ok, msg, dt, sh, sj = fu.result()
            print(f'{"ok  " if ok else "FAIL"} {n:8s} {dt:5.1f}s {msg}')
            fail += not ok
    print(f'netplay: {len(names) - fail}/{len(names)} ok ({time.time() - t0:.0f}s, pace {a.pace:g} Hz)')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
