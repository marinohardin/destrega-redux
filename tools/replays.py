#!/usr/bin/env python3
"""Gameplay regression replays: recorded 1v1 fights replayed frame by frame (runtime/replay.c).

Each replay in tools/replays/NAME.dtr.gz holds only inputs (both pads, every frame) plus hashes
(16-bit hashes of the gameplay fields at every frame, the complete emulated-state hash every 60
frames) and the disc / build identity. No game data. `run` replays them headless and fails on the
first frame whose gameplay fields differ, printing which fighter / field and the live values.

  replays.py list                    names and what they cover
  replays.py run [NAME...] [-j N]    replay and check the default set (tools/run_tests.sh runs it); --all adds
                                     the heavy ones (story mode)
  replays.py record [NAME...]        re-record from the input scripts below (intentional changes
                                     only: look at the reason first, see AGENTS.md "Replays")
  replays.py press NAME              print the --press / --press2 scripts
  replays.py trace NAME OUT          replay with --gameplay-trace OUT (local values, never commit)

The inputs come from scripts: the menu path of tools/scenarios.py (VS character / stage select, 1P
battle, PRACTICE, story, or an option detour) followed by fight input: seeded random input for each pad
(taps, direction + button chords, quarter-circle and dragon-punch motions, charged buttons, walking and
jumping), the command battery of tools/movelab.py in PRACTICE (every character), or scripted
walk-and-hit / idle sequences for the round timer (ROUND TIME 30 s).
"""
import argparse, concurrent.futures as cf, gzip, os, random, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import scenarios as S  # noqa: E402

BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))
DIR = os.path.join(ROOT, 'tools', 'replays')
B = S.BTN
FACE = [B['square'], B['cross'], B['triangle'], B['circle']]
SHOULDER = [B['l1'], B['r1'], B['l2'], B['r2']]


def fight_input(seed, t0, t1, aggressive=1.0):
    """Seeded random fight input from frame t0 to t1 as [(frame, mask)] (each tap held 6 frames)."""
    r = random.Random(seed)
    out, t = [], t0
    while t < t1:
        f, b = (B['right'], B['left']) if r.random() < 0.5 else (B['left'], B['right'])
        btn = r.choice(FACE + FACE + SHOULDER)
        kind = r.random()
        if kind < 0.25 * aggressive: seq = [btn]                                         # tap
        elif kind < 0.40: seq = [r.choice([f, b, B['down'], B['up']]) | btn]            # chord
        elif kind < 0.52: seq = [B['down'], B['down'] | f, f | btn]                      # quarter circle
        elif kind < 0.58: seq = [f, B['down'], B['down'] | f | btn]                      # dragon punch
        elif kind < 0.64: seq = [B['down'], B['down'] | b, b | btn]                      # reverse qc
        elif kind < 0.72: seq = [btn] * r.randint(4, 12)                                 # charge / hold
        elif kind < 0.84: seq = [r.choice([f, b, f, B['down']])] * r.randint(3, 10)     # walk / crouch
        elif kind < 0.92: seq = [r.choice([B['up'], B['up'] | f, B['up'] | b])]          # jump
        else: seq = []                                                                     # idle
        for m in seq:
            out.append((t, m)); t += 6
        t += r.choice([0, 0, 6, 12, 24, 40])
    return out


def fmt(steps):
    return ','.join(f'{f}:{m:x}' for f, m in sorted(steps))


class Replay:
    def __init__(self, name, desc, p1, p2, frames, chars=(), stage=None, mode='vs', heavy=False):
        self.name, self.desc, self.p1, self.p2, self.frames = name, desc, p1, p2, frames
        self.chars, self.stage, self.mode, self.heavy = tuple(chars), stage, mode, heavy

    @property
    def path(self): return os.path.join(DIR, self.name + '.dtr.gz')


REPLAYS = {}


def add_vs(name, c1, c2, stage, seed, fight_frames, desc, heavy=False):
    d1, d2, t_end = S.vs_script(c1, c2, stage)
    t0 = t_end + 300                       # stage intro; the fight is running ~700 frames after t_end
    p1 = S.parse(d1) + fight_input(seed * 2 + 1, t0, t0 + fight_frames)
    p2 = S.parse(d2) + fight_input(seed * 2 + 2, t0, t0 + fight_frames)
    REPLAYS[name] = Replay(name, f'VS {c1} vs {c2} on {stage}: {desc}', p1, p2, t0 + fight_frames + 200, (c1, c2), stage, 'vs', heavy)


def add_1p(name, char, seed, fight_frames, desc):
    i = S.ROSTER.index(char)
    pick = 1750 + 40 * i + 40
    dsl = f'{S.BOOT} +60 start/20 +400 ' + (f'@1750 right*{i} ' if i else '') + f'@{pick} cross/100 cross/100 cross/100'
    t0 = pick + 1500
    REPLAYS[name] = Replay(name, f'1P BATTLE as {char} vs the CPU: {desc}', S.parse(dsl) + fight_input(seed, t0, t0 + fight_frames),
                           [], t0 + fight_frames + 200, (char,), None, '1p')


# GAME OPTION rows (the option menu's first page): level, round count, round time, handicap,
# quick select, life recover; Right changes a value (round time: NO LIMIT -> 30 -> 60 -> 99).
OPT_ROWS = ['level', 'rounds', 'time', 'handicap', 'quick', 'recover']


def vs_script_opts(c1, c2, stage, settings):
    """Like scenarios.vs_script but first changes GAME OPTION values: settings = {row: Right presses}.
    The VS flow after the title is shifted by however long the option detour took."""
    d1, d2, t_end = S.vs_script(c1, c2, stage)
    orig = S.parse(d1)
    f0 = max(f for f, m in orig if f < 1750)                  # the title's Start on VS
    toks = f'{S.BOOT} {S.title_pick("option")} +300 cross/100 +100 '
    row = 0
    for name, n in sorted(settings.items(), key=lambda kv: OPT_ROWS.index(kv[0])):
        r = OPT_ROWS.index(name)
        if r > row: toks += f'down*{r - row} '; row = r
        toks += ' '.join(['right/60'] * n) + ' '
    toks += '+60 circle/100 circle/200 +100 down*3 +60 start/20'
    pre = S.parse(toks)
    delta = pre[-1][0] - f0
    steps = pre + [(f + delta, m) for f, m in orig if f >= 1750]
    steps2 = [(f + delta, m) for f, m in S.parse(d2)]
    return steps, steps2, t_end + delta


def add_opt(name, c1, c2, stage, settings, p1_fn, p2_fn, desc, heavy=False):
    """VS fight with changed options; p1_fn(t0) / p2_fn(t0) -> (steps, last_frame) of fight input."""
    s1, s2, t_end = vs_script_opts(c1, c2, stage, settings)
    t0 = t_end + 800
    a, n1 = p1_fn(t0); b, n2 = p2_fn(t0)
    REPLAYS[name] = Replay(name, f'VS {c1} vs {c2} on {stage}, options {settings}: {desc}', s1 + a, s2 + b,
                           t0 + max(n1, n2) + 200, (c1, c2), stage, 'vs-options', heavy)


def _mirror(steps):
    """Swap left/right (a fighter on the right side faces left)."""
    L, Rt = B['left'], B['right']
    return [(f, (m & ~(L | Rt)) | (L if m & Rt else 0) | (Rt if m & L else 0)) for f, m in steps]


def move_battery(slice_idx, t0, gap=90):
    """Every command in tools/movelab.py (taps, directions + button, quarter circles, dragon punch,
    half circle, dashes, holds, button pairs; 96 commands) split in 2 interleaved slices."""
    import movelab as M
    names = M.battery_names()[slice_idx::2]
    steps, starts = M.schedule(names, t0, gap)
    return steps, len(names) * gap


def add_practice(name, char, desc='command battery', gap=80):
    """PRACTICE mode (no timer, no KO: the dummy stands there): P1 = char performs every command of
    tools/movelab.py's battery (taps, directions + button, qcf / qcb / dp, double taps, holds,
    button pairs) one after another."""
    i = S.ROSTER.index(char)
    pick = 1750 + 40 * i + 40
    dsl = f'{S.BOOT} {S.title_pick("practice")} +400 ' + (f'right*{i} ' if i else '') + 'cross/100 cross/100 cross/100 cross/100 cross/100 cross/100'
    t0 = 3000 + 40 * i
    import movelab as M
    steps, starts = M.schedule(M.battery_names(), t0, gap)
    REPLAYS[name] = Replay(name, f'PRACTICE as {char}: {desc} ({len(starts)} commands)', S.parse(dsl) + steps, [],
                           t0 + gap * len(starts) + 100, (char,), None, 'practice')


add_vs('match_gradd_celia_hills', 'gradd', 'celia', 'hills', 1, 9000, 'a full best-of-3 match, both pads random')
add_vs('vs_tieme_rohzen_villa', 'tieme', 'rohzen', 'villa', 2, 3000, 'random fight')
add_vs('vs_milena_cougar_fortress', 'milena', 'cougar', 'fortress', 3, 3000, 'random fight')
add_vs('vs_zauber_doyle_throne', 'zauber', 'doyle', 'throne', 4, 3000, 'random fight')
add_vs('vs_anjie_laon_canyon', 'anjie', 'laon', 'canyon', 5, 3000, 'random fight')
add_1p('1p_reyus_cpu', 'reyus', 6, 3000, 'CPU opponent (AI + RNG), P1 random')
# PRACTICE: every character performs the whole command battery
for _c in S.ROSTER:
    add_practice(f'practice_{_c}', _c)

# the remaining VS stages (short random fights; the others are covered above), characters rotate
for _c1, _c2, _st, _sd in [('falma', 'reyus', 'cave', 11), ('laon', 'gradd', 'gate', 12), ('doyle', 'anjie', 'castle1', 13),
                           ('cougar', 'tieme', 'castle2', 14), ('rohzen', 'milena', 'remains', 15),
                           ('celia', 'falma', 'hamlet', 16), ('reyus', 'zauber', 'shrine', 17)]:
    add_vs(f'vs_{_c1}_{_c2}_{_st}', _c1, _c2, _st, _sd, 1500, 'random fight')


def add_story(name, seed, fight_frames, after_frames, desc):
    """STORY mode: new game, intro, scenes, the first fight (Gradd vs a Relics soldier) with random
    input; the seed wins it. Afterwards Cross every 120 frames: SAVE DATA? (NO), the next scenes."""
    t0 = 14850
    steps = S.parse(S.SCENARIOS['story'].dsl) + fight_input(seed, t0, t0 + fight_frames, 1.5)
    steps += [(f, B['cross']) for f in range(t0 + fight_frames + 100, t0 + fight_frames + after_frames, 120)]
    REPLAYS[name] = Replay(name, f'STORY: {desc}', steps, [], t0 + fight_frames + after_frames + 200, ('gradd',), None, 'story', heavy=True)


add_story('story_win_first_fight', 33, 3500, 7500,
          'random input wins the first fight (seed 33), then SAVE DATA? NO and the next event scenes')


def _idle(t0): return [], 4200


def _walk_hit(side):
    def f(t0):
        d = B['right'] if side == 1 else B['left']
        if side == 2:       # P2 starts further away (Celia walks slower): longer walk, more swings
            st = [(t0 + i * 6, d) for i in range(60)] + [(t0 + 380 + 60 * k, B['square'] if k % 2 == 0 else B['triangle']) for k in range(6)]
            return st, 4200
        st = [(t0 + i * 6, d) for i in range(25)] + [(t0 + 160, B['square']), (t0 + 220, B['triangle']), (t0 + 280, B['square'])]
        return st, 4200
    return f


# GAME OPTION changes + round timer (ROUND TIME 30 s: the round ends when round_time reaches 1800)
add_opt('timeout_draw', 'gradd', 'celia', 'hills', {'time': 1}, _idle, _idle,
        'both fighters idle: two draws on time (round_wins 1,1 then 2,2)')
add_opt('timeout_p1_wins', 'gradd', 'celia', 'hills', {'time': 1}, _walk_hit(1), _idle,
        'P1 lands hits then both idle: P1 wins the round on time; round 2 a draw')
add_opt('timeout_p2_wins', 'gradd', 'celia', 'hills', {'time': 1}, _idle, _walk_hit(2),
        'P2 lands hits then both idle: P2 wins the round on time')
add_opt('opts_handicap_recover', 'tieme', 'anjie', 'villa', {'rounds': 1, 'handicap': 1, 'recover': 1},
        lambda t0: (fight_input(21, t0, t0 + 3000), 3000), lambda t0: (fight_input(22, t0, t0 + 3000), 3000),
        'ROUND COUNT / HANDICAP ON / LIFE RECOVER changed, random fight')



def run(rp, extra=(), out=None, tag=''):
    log_dir = os.path.join(ROOT, 'build', 'replays')
    os.makedirs(log_dir, exist_ok=True)
    cmd = [BIN, '--headless', '--no-interp'] + list(extra)
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    log = r.stdout + r.stderr
    suffix = ('.record' if '--record' in extra else '') + (('.' + tag) if tag else '')
    open(os.path.join(log_dir, rp.name + suffix + '.log'), 'w').write(log)
    return r.returncode, log, time.time() - t0


def record(rp):
    os.makedirs(DIR, exist_ok=True)
    tmp = os.path.join(ROOT, 'build', 'replays', rp.name + '.dtr')
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    trace = os.path.join(ROOT, 'build', 'replays', rp.name + '.trace')
    extra = ['--frames', str(rp.frames), '--press', fmt(rp.p1), '--record', tmp, '--gameplay-trace', trace]
    if rp.p2: extra += ['--press2', fmt(rp.p2)]
    rc, log, dt = run(rp, extra)
    if rc != 0 or 'FATAL' in log: return rc or 1, f'record failed: {log[-300:]}', dt
    with open(tmp, 'rb') as f, gzip.GzipFile(rp.path, 'wb', 9, mtime=0) as g: g.write(f.read())
    return 0, summarize_trace(trace), dt


def summarize_trace(path):
    """What the recorded fight covered (from the local gameplay trace, which is never committed)."""
    ko, gauge_drops, matches, best = 0, 0, 0, (0, 0)
    prev, pw = None, (0, 0)
    for line in open(path):
        m = re.match(r'(\d+) p1 hp=(-?\d+) gauge=(-?\d+) .* p2 hp=(-?\d+) gauge=(-?\d+) .* wins=(\d+),(\d+)', line)
        if not m: continue
        h1, g1, h2, g2, w1, w2 = (int(x) for x in m.groups()[1:])
        if prev:
            if (h1 == 0 and prev[0] > 0) or (h2 == 0 and prev[2] > 0): ko += 1
            if g1 < prev[1] - 100: gauge_drops += 1
            if g2 < prev[3] - 100: gauge_drops += 1
        prev = (h1, g1, h2, g2)
        if max(w1, w2) >= 2 and max(pw) < 2: matches += 1
        if w1 + w2 > sum(best): best = (w1, w2)
        pw = (w1, w2)
    return f'{ko} KOs, most rounds won {best[0]}-{best[1]}, {matches} matches decided, {gauge_drops} big gauge spends'


# extra runs of `run` (no names given): the replays must not care about presentation options or
# save-state rewinds, and an edited input must be reported as a divergence
VARIANTS = [
    ('match_gradd_celia_hills', 'rewind', ['--savestate-rewind', '29:11']),
    ('vs_tieme_rohzen_villa', 'hires+widescreen', ['--scale', '2x3', '--widescreen', '--window', '1280x720']),
    ('vs_zauber_doyle_throne', 'pgxp-off+4x4', ['--scale', '4', '--pgxp', 'off']),
]


def check(rp, extra=(), tag=''):
    rc, log, dt = run(rp, ['--replay', rp.path] + list(extra), tag=tag)
    m = re.search(r'replay: .*: (\d+) frames compared, (\d+) state hashes compared \((\d+) differ\): (\w+)', log)
    ok = rc == 0 and m and m.group(4) == 'ok'
    msg = m.group(0).split(': ', 2)[-1] if m else 'no replay summary'
    if not ok:
        d = re.search(r'replay: DIVERGED.*?(?=\n\[|\Z)', log, re.S)
        msg += '\n' + (d.group(0) if d else log[-600:])
    return ok, msg, dt


def selftest():
    """An edited input must make the replay fail with a divergence report naming fields."""
    rp = REPLAYS['vs_tieme_rohzen_villa']
    lines = gzip.open(rp.path, 'rt').read().splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith('i ') and int(l.split()[1]) > rp.frames - 2000)
    f, a, b = lines[i].split()[1:]
    lines[i] = f'i {f} {int(a, 16) ^ 0x4000:x} {b}'
    bad = os.path.join(ROOT, 'build', 'replays', 'selftest.dtr')
    open(bad, 'w').write('\n'.join(lines) + '\n')
    rc, log, dt = run(Replay('selftest', '', [], [], rp.frames), ['--replay', bad])
    m = re.search(r'replay: DIVERGED at frame (\d+): \d+ gameplay field', log)
    ok = rc == 7 and m and 'DIFFERS' in log
    return bool(ok), f'edited input at frame {f} -> ' + (f'divergence reported at frame {m.group(1)}' if m else 'NO divergence reported'), dt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['list', 'run', 'record', 'press', 'trace'])
    ap.add_argument('names', nargs='*')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--all', action='store_true', help='include the heavy replays (story mode ...)')
    a = ap.parse_args()
    if a.cmd == 'list':
        for n, rp in REPLAYS.items(): print(f'{n:28s} {rp.frames:6d} frames{" [heavy]" if rp.heavy else "":8s} {rp.desc}')
        return 0
    if a.cmd == 'press':
        rp = REPLAYS[a.names[0]]
        print('--press', fmt(rp.p1)); print('--press2', fmt(rp.p2)); return 0
    if a.cmd == 'trace':
        rp = REPLAYS[a.names[0]]
        rc, log, dt = run(rp, ['--replay', rp.path, '--gameplay-trace', a.names[1]])
        return rc
    names = a.names or [n for n, r in REPLAYS.items() if a.all or not r.heavy]
    t0 = time.time()
    fail = 0
    jobs = [(n, '', ()) for n in names]
    if a.cmd == 'run' and not a.names: jobs += VARIANTS + [('selftest', 'selftest', ())]

    def job(j):
        n, tag, extra = j
        if a.cmd == 'record': return record(REPLAYS[n])
        if n == 'selftest': return selftest()
        return check(REPLAYS[n], extra, tag)
    with cf.ThreadPoolExecutor(a.j) as ex:
        for (n, tag, _), res in zip(jobs, ex.map(job, jobs)):
            label = n + (f' [{tag}]' if tag and n != 'selftest' else '')
            if a.cmd == 'record':
                rc, msg, dt = res
                print(f'{"ok  " if rc == 0 else "FAIL"} {label:28s} {dt:5.1f}s recorded: {msg}')
                fail += rc != 0
            else:
                ok, msg, dt = res
                print(f'{"ok  " if ok else "FAIL"} {label:28s} {dt:5.1f}s {msg}')
                fail += not ok
    print(f'replays: {len(jobs) - fail}/{len(jobs)} ok ({time.time() - t0:.0f}s)')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
