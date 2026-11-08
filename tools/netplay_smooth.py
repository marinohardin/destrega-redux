#!/usr/bin/env python3
"""Netplay smoothness measurements: two windowed peers at 60 Hz playing a VS fight of a
tools/replays.py script, each printing the smoothness summary (runtime/smooth.c, "smooth-json")
and the netplay summary at exit. Prints one table row per peer.

Configurations (name: host side, join side, --net-sim each way, extra args host, extra args join):
  mm_lan   Mac <-> Mac, clean loopback
  mm_wan   Mac <-> Mac, 20 ms each way + 10 ms jitter, 2% loss (40 ms RTT)
  mw_lan   Mac host <-> Windows joiner under Wine (as the user ran it: software renderer there)
  mw_wide  the same with widescreen on the Wine side
Wine settings as in tools/test_netplay_wine.py (WINE, WINE_ARGS, WIN_EXE, PAK).

  netplay_smooth.py [CONFIG...] [--replay NAME] [--json FILE] [--extra "ARGS"]
"""
import argparse, json, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import replays as R  # noqa: E402
import test_netplay_wine as W  # noqa: E402

OUT = os.path.join(ROOT, 'build', 'netplay_smooth')
CONFIGS = {
    'solo':    ('mac', None, '', '', [], []),       # one offline window, both pads scripted: the reference
    'solo_win': ('win', None, '', '', ['--renderer', 'software'], []),
    'mm_lan':  ('mac', 'mac', '', '', [], []),
    'mm_wan':  ('mac', 'mac', '20:10:2:0:1', '20:10:2:0:2', [], []),
    'mw_lan':  ('mac', 'win', '', '', [], ['--renderer', 'software']),
    'mw_wide': ('mac', 'win', '', '', [], ['--renderer', 'software', '--widescreen']),
}


def parse(log):
    out = {}
    m = re.search(r'smooth-json: (\{.*\})', log)
    if m: out.update(json.loads(m.group(1)))
    s = W.T.summary(log)
    if s: out.update({'np_' + k: v for k, v in s.items()})
    hp = [tuple(map(float, x)) for x in re.findall(r'host per frame: emu ([\d.]+) ms, present\+convert ([\d.]+) ms, sleep ([\d.]+) ms', log)]
    if hp:
        out['emu_ms'] = sum(h[0] for h in hp) / len(hp)
        out['present_ms'] = sum(h[1] for h in hp) / len(hp)
    out['desyncs_seen'] = len(re.findall(r'DESYNC at frame', log))
    return out


def run(name, rp_name, port, extra):
    hs, js, sim_h, sim_j, xh, xj = CONFIGS[name]
    rp = R.REPLAYS[rp_name]
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    fight_from = rp.frames - 2800          # the fight is running (replays.add_vs)
    common = ['--no-interp', '--frames', str(rp.frames), '--smooth-from', str(fight_from), '--no-memcard', '--net-full-boot',
              '--net-desync-dir', ('path', d)] + extra
    host = common + xh + ['--press', R.fmt(rp.p1), '--host', str(port)] + (['--net-sim', sim_h] if sim_h else [])
    join = common + xj + ['--press', R.fmt(rp.p2), '--join', f'127.0.0.1:{port}'] + (['--net-sim', sim_j] if sim_j else [])
    logs = {w: os.path.join(d, w + '.log') for w in ('host', 'join')}
    t0 = time.time()
    if js is None:
        args = common + xh + ['--press', R.fmt(rp.p1), '--press2', R.fmt(rp.p2)]
        rc = subprocess.run(W.cmd_for(hs, args), stdout=open(logs['host'], 'w'), stderr=subprocess.STDOUT, cwd=ROOT,
                            env=W.env_for(hs)).returncode
        r = parse(open(logs['host'], errors='replace').read())
        r.update(side=hs, rc=rc)
        return {'host': r}, time.time() - t0
    procs = {}
    # the Wine side first (it starts slower; the handshake waits for it anyway)
    order = [('host', hs, host), ('join', js, join)]
    if js == 'win': order.reverse()
    for who, side, args in order:
        procs[who] = subprocess.Popen(W.cmd_for(side, args), stdout=open(logs[who], 'w'), stderr=subprocess.STDOUT,
                                      cwd=ROOT, env=W.env_for(side))
        time.sleep(1.0)
    rc = {w: p.wait(timeout=1800) for w, p in procs.items()}
    res = {}
    for who, side in (('host', hs), ('join', js)):
        r = parse(open(logs[who], errors='replace').read())
        r.update(side=side, rc=rc[who])
        res[who] = r
    return res, time.time() - t0


def row(name, who, r):
    g = lambda k, f='{:.1f}': (f.format(r[k]) if k in r else '-')
    return (f'{name:8s} {who:4s} {r["side"]:3s} emu {g("emu_ms")} pres {g("present_ms")} | iv p50 {g("iv_p50")} '
            f'p95 {g("iv_p95")} p99 {g("iv_p99")} max {g("iv_max")} sd {g("iv_sd", "{:.2f}")} | held {g("held_pct", "{:.2f}")}% '
            f'(rb {g("held_rollback", "{:.0f}")} stall {g("held_stall", "{:.0f}")} sync {g("held_sync", "{:.0f}")} '
            f'slow {g("held_slow", "{:.0f}")}) | stalls {g("stalls", "{:.0f}")} {g("stall_ms", "{:.0f}")} ms | '
            f'sync {g("sync_frames", "{:.0f}")} | rb {g("rollbacks", "{:.0f}")} resim avg {g("resim_avg", "{:.2f}")} '
            f'max {g("resim_max", "{:.1f}")} late {g("rollback_late", "{:.0f}")} | corr {g("corrections", "{:.0f}")} | '
            f'audio under {g("audio_under", "{:.0f}")} low {g("audio_low", "{:.0f}")} rep {g("audio_repeat", "{:.0f}")} | '
            f'desync {r.get("desyncs_seen", "-")} rc {r["rc"]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('configs', nargs='*')
    ap.add_argument('--replay', default='vs_tieme_rohzen_villa')
    ap.add_argument('--json')
    ap.add_argument('--extra', default='', help='extra args for both peers')
    a = ap.parse_args()
    names = a.configs or list(CONFIGS)
    base = 47900 + (os.getpid() % 30) * 4
    allres = {}
    for i, n in enumerate(names):
        if n == 'solo_win' or 'win' in CONFIGS[n][:2] and not (W.WINE and os.path.exists(W.WIN_EXE)):
            print(f'{n}: skipped (no Wine / Windows build)'); continue
        la0 = os.getloadavg()[0]
        res, dt = run(n, a.replay, base + i, a.extra.split())
        la1 = os.getloadavg()[0]
        for r in res.values(): r['load'] = [la0, la1]
        allres[n] = res
        for who in res: print(row(n, who, res[who]), flush=True)
        print(f'{n}: {dt:.0f} s, load average {la0:.1f} -> {la1:.1f} ({os.cpu_count()} CPUs)', flush=True)
    if a.json:
        old = json.load(open(a.json)) if os.path.exists(a.json) else {}
        old.update(allres)
        json.dump(old, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
