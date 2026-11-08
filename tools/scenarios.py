#!/usr/bin/env python3
"""Scenario library: named, deterministic input scripts that drive the recompiled game to a
specific mode / stage / character pair, plus the checks that prove each one arrived.

A scenario is a short DSL string (see `parse`) giving the whole boot -> menu -> mode path, the
frame window of interest and what must be true there. `tools/scenario_test.sh` runs them all
(headless, --no-interp); `tools/test_hires_determinism.sh` re-runs a subset at several hi-res scales.

  scenarios.py list                       scenario names, tags and what they claim
  scenarios.py press NAME                 print the --press string (for --hires / ad-hoc runs)
  scenarios.py run NAME... [--out DIR] [--dump-every N] [-- extra destrega args]
  scenarios.py run --all [-j N]           run and check everything (parallel)
  scenarios.py raw "DSL" FRAMES [--out DIR] [--dump-every N]   explore: run an ad-hoc script

DSL (whitespace separated tokens, evaluated left to right with a frame cursor):
  @N        set the cursor to absolute frame N
  +N        advance the cursor N frames
  name      tap that button at the cursor (held 6 frames by the runtime), then advance by
            DEFAULT_GAP (40) frames. Names: start select up down left right l1 l2 r1 r2
            triangle circle cross square (aliases: x o t sq)
  name*K    tap it K times (each followed by the default gap)
  name/W    tap then advance W frames instead of the default gap (e.g. start/200)
  a+b       chord (several buttons at once), e.g. cross+right
Timing is frame based, not screen based, because headless runs are bit-for-bit deterministic:
the same script always lands on the same screens. The per-scenario checks below are what keep
the timing honest if the game flow ever changes.
"""
import argparse, concurrent.futures as cf, json, os, re, struct, subprocess, sys, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build/cmake/destrega')
DEFAULT_GAP = 40

BTN = {'select': 0x1, 'start': 0x8, 'up': 0x10, 'right': 0x20, 'down': 0x40, 'left': 0x80,
       'l2': 0x100, 'r2': 0x200, 'l1': 0x400, 'r1': 0x800, 'triangle': 0x1000, 'circle': 0x2000,
       'cross': 0x4000, 'square': 0x8000}
ALIAS = {'x': 'cross', 'o': 'circle', 't': 'triangle', 'sq': 'square'}


def parse(dsl):
    """DSL -> [(frame, mask)] for --press."""
    steps, t = [], 0
    for tok in dsl.split():
        if tok[0] == '@': t = int(tok[1:]); continue
        if tok[0] == '+': t += int(tok[1:]); continue
        gap = DEFAULT_GAP
        if '/' in tok: tok, g = tok.split('/'); gap = int(g)
        n = 1
        if '*' in tok: tok, k = tok.split('*'); n = int(k)
        mask = 0
        for b in tok.split('+'):
            b = ALIAS.get(b, b)
            mask |= BTN[b]
        for _ in range(n):
            steps.append((t, mask)); t += gap
    return steps


def press_arg(dsl):
    return ','.join(f'{f}:{m:x}' for f, m in parse(dsl))


# ---- boot / menu fragments ----------------------------------------------------------------------
BOOT = '@1200 start +100'          # frame 1200: Start skips the logo; title menu is up by ~1300
# The title is a circular 8-item list with the cursor in the middle; it starts on 1P BATTLE.
TITLE_ORDER = ['option', '1p battle', 'story', 'vs', 'team battle', 'time attack', 'endurance', 'practice']


def title_pick(item):
    """Tokens that move the title cursor from 1P BATTLE to `item` and press Start."""
    k = (TITLE_ORDER.index(item) - 1) % len(TITLE_ORDER)
    if k > len(TITLE_ORDER) // 2: k -= len(TITLE_ORDER); d = 'up'
    else: d = 'down'
    return (f'{d}*{abs(k)} ' if k else '') + '+60 start/20'


# ---- PNG helpers (no dependencies) ---------------------------------------------------------------
def read_png(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    pos, idat = 8, b''
    while pos < len(d):
        n, = struct.unpack('>I', d[pos:pos + 4])
        if d[pos + 4:pos + 8] == b'IDAT': idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h)]


def lit_fraction(path, thresh=40):
    w, h, rows = read_png(path)
    lit = sum(1 for r in rows for i in range(1, len(r), 4) if r[i] > thresh)
    return lit / (w * h)


def region_mean(path, box):
    """mean (r,g,b) over box=(x0,y0,x1,y1) in fractions of the frame."""
    w, h, rows = read_png(path)
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    s = [0, 0, 0]; n = 0
    for y in range(y0, y1):
        r = rows[y]
        for x in range(x0, x1):
            for c in range(3): s[c] += r[x * 4 + c]
            n += 1
    return tuple(v / max(n, 1) for v in s)


# ---- characters and stages (VS mode roster / stage select, observed) ------------------------------
# VS character select: Right from Gradd cycles through these 12 portraits (verified: dumps).
ROSTER = ['gradd', 'celia', 'tieme', 'anjie', 'milena', 'laon', 'zauber', 'rohzen', 'falma', 'doyle',
          'reyus', 'cougar']
# Character -> overlay file stem (Mxxx.BIN is the P1-side fighter, Mxxx2.BIN the P2-side one).
# gradd, tieme, anjie, milena, reyus, celia verified by CD trace; the rest follow the file names
# and are checked by the scenarios that use them.
OVERLAY = {'gradd': 'MGRAD', 'celia': 'MSEREA', 'tieme': 'MTEEM', 'anjie': 'MANZEAL', 'milena': 'MMILENA',
           'laon': 'MLAON', 'zauber': 'MZAUBER', 'rohzen': 'MROSEN', 'falma': 'MFALMA', 'doyle': 'MDOIL',
           'reyus': 'MREUS', 'cougar': 'MKUGA'}
# VS stage select: Right from the default cycles through these 12 (verified: dumps).
STAGES = ['hills', 'cave', 'villa', 'canyon', 'gate', 'castle1', 'castle2', 'throne', 'remains', 'hamlet',
          'shrine', 'fortress']


def vs_script(p1, p2, stage):
    """(p1 DSL, p2 DSL, stage_confirm_frame) for a VS match. Both pads pick on the character
    select (pad 2 needs --press2), then P1 moves the stage cursor and confirms."""
    i1, i2, k = ROSTER.index(p1), ROSTER.index(p2), STAGES.index(stage)
    pick = 1750 + 40 * max(i1, (i2 - 1) % 12) + 40          # both confirmed
    t_stage = pick + 200                         # stage select is up well before this
    d1 = f'{BOOT} {title_pick("vs")} @1750 ' + (f'right*{i1} ' if i1 else '') + f'@{pick - 40} cross '
    d1 += f'@{t_stage} ' + (f'right*{k} ' if k else '') + 'cross'
    n2 = (i2 - 1) % 12                           # the P2 cursor starts on Celia, not Gradd
    d2 = '@1750 ' + (f'right*{n2} ' if n2 else '') + f'@{pick - 40} cross'
    return d1, d2, t_stage + 40 * k + 40


# ---- scenario registry ---------------------------------------------------------------------------
class Scn:
    def __init__(self, name, dsl, frames, key, desc, tags=(), dsl2='', fight_from=None, expect_files=(),
                 min_lit=0.15, xfail=None, min_cols=40):
        self.name, self.dsl, self.dsl2, self.frames, self.key, self.desc = name, dsl, dsl2, frames, key, desc
        self.tags = set(tags)
        self.fight_from = fight_from if fight_from is not None else key - 300   # --fight-from for hi-res checks
        self.expect_files = list(expect_files)     # OVL files that must be loaded, in this order
        self.min_lit = min_lit
        self.min_cols = min_cols
        self.xfail = xfail      # known bug: substring the log must contain (the run must fail); see NOTES.md

    def press(self): return press_arg(self.dsl)
    def press2(self): return press_arg(self.dsl2) if self.dsl2 else ''


SCENARIOS = {}


def add(*a, **k):
    s = Scn(*a, **k)
    assert s.name not in SCENARIOS and s.key % 100 == 0, s.name
    SCENARIOS[s.name] = s
    return s


def add_vs(p1, p2, stage, tags=(), name=None, **kw):
    d1, d2, t_end = vs_script(p1, p2, stage)
    key = (t_end + 700) // 100 * 100 + 100    # a fight frame ~700 after the stage is confirmed
    add(name or f'vs_{stage}', d1, key + 200, key, f'VS: {p1} (P1) vs {p2} (P2) on {stage}', ('vs', 'stage') + tuple(tags),
        dsl2=d2, expect_files=[OVERLAY[p1] + '.BIN', OVERLAY[p2] + '2.BIN'], **kw)


# every stage once; every character appears on each side across the list
# (P2 Rohzen is on villa: it used to be a dispatch miss, fixed by seeding the main exe's overlay pointers)
for _k, _s in enumerate(STAGES):
    add_vs(ROSTER[_k], ROSTER[(_k + 5) % 12], _s, tags=('hires',),
           **({'min_lit': 0.04} if _s == 'cave' else {}))     # the cave is genuinely dark

# 1P BATTLE (the title cursor starts on it): pick a character, the game picks opponent and stage
for _name, _i in (('gradd', 0), ('celia', 1), ('cougar', 11)):
    _pick = 1750 + 40 * _i + 40
    add(f'1p_{_name}', f'{BOOT} +60 start/20 +400 ' + (f'@1750 right*{_i} ' if _i else '') +
        f'@{_pick} cross/100 cross/100 cross/100', _pick + 2700, (_pick + 2300) // 100 * 100, f'1P BATTLE as {_name}',
        ('1p', 'mode', 'loose') + (('hires',) if _name == 'celia' else ()), expect_files=[OVERLAY[_name] + '.BIN'])
add('team', f'{BOOT} {title_pick("team battle")} +400 ' + ' '.join(['cross/60 right/60'] * 6) +
    ' +200 cross/100 cross/100', 5400, 5000, 'TEAM BATTLE 3 on 3 (one pad picks both teams)', ('mode', 'loose'))
add('time_attack', f'{BOOT} {title_pick("time attack")} +400 ' + 'cross/100 ' * 6, 4000, 3400, 'TIME ATTACK', ('mode', 'loose'), expect_files=['MGRAD.BIN'])
add('endurance', f'{BOOT} {title_pick("endurance")} +400 ' + 'cross/100 ' * 6, 4000, 3400, 'ENDURANCE', ('mode', 'loose'), expect_files=['MGRAD.BIN'])
add('practice', f'{BOOT} {title_pick("practice")} +400 ' + 'cross/100 ' * 6, 4000, 3400,
    'PRACTICE (training room, TOTAL/DAMAGE counters)', ('mode', 'loose'), expect_files=['MGRAD.BIN'])
add('story', f'{BOOT} {title_pick("story")} +200 cross', 15600, 14900,
    'STORY: new game, FMV intro, text crawl, event scenes (EVENT_SP/EVENTOVL), first fight vs Relics',
    ('mode', 'story'), expect_files=['EVENT_SP.BIN', 'EVENTOVL.BIN', 'MGRAD.BIN', 'MSOLD2.BIN'], fight_from=14700)
add('option', f'{BOOT} {title_pick("option")} +300 ' +
    'cross/100 circle/100 down cross/100 circle/100 down cross/100 circle/100 down cross/100 circle/100 '
    'down*2 cross/100 circle/100 up*5 +100', 3600, 3000, 'OPTION menu and its sub-pages (OPTOVL)',
    ('mode',), expect_files=['OPTOVL.BIN'], fight_from=3000, min_lit=0.04, min_cols=10)


# ---- running and checking -----------------------------------------------------------------------
EXPECT_PATH = os.path.join(ROOT, 'tools', 'scenario_expect.json')
BAD_LOG = re.compile(r'FATAL|dispatch: no recompiled|unhandled|MISMATCH|interp: enter')


def disc_files():
    """[(first_lba, sectors, name)] from disc/_listing.txt (written by tools/extract_disc.py)."""
    path = os.path.join(ROOT, 'disc', '_listing.txt')
    if not os.path.exists(path):
        sys.exit('scenarios: disc/_listing.txt missing; run: python3 tools/extract_disc.py <Destrega.bin|destrega.pak> disc')
    out = []
    for l in open(path):
        p = l.split()
        if len(p) >= 3 and p[0].isdigit():
            out.append((int(p[0]), max(1, (int(p[1]) + 2047) // 2048), p[-1]))
    return out


def cd_loads(log, files):
    """Parse the --trace CD setloc commands into [(frame, file, sector_offset)] (one entry per seek)."""
    def bcd(x): return (x >> 4) * 10 + (x & 15)
    ev = []
    for l in log.splitlines():
        m = re.match(r'\[\s*(\d+)\] cd: cmd 02 params 3 \[(\w\w) (\w\w) (\w\w)\]', l)
        if not m: continue
        mm, ss, ff = (bcd(int(m.group(i), 16)) for i in (2, 3, 4))
        lba = (mm * 60 + ss) * 75 + ff - 150
        name, off = '?', lba
        for s, n, nm in files:
            if s <= lba < s + n: name, off = nm, lba - s; break
        ev.append((int(m.group(1)), name, off))
    return ev


# DATA.BNS holds one ~120-sector stage pack per stage, read at these sector offsets (verified: the
# value each vs_<stage> scenario reads; the stage cursor -> name mapping was read off the stage select
# thumbnails). Character data is read below STAGE_MIN.
STAGE_DATA = {'hamlet': 7756, 'fortress': 7861, 'villa': 7973, 'remains': 8096, 'hills': 8212, 'canyon': 8329,
              'cave': 8447, 'gate': 8580, 'castle1': 8702, 'castle2': 8831, 'throne': 8959, 'shrine': 9086}
STAGE_MIN, STAGE_MAX = 7700, 9200


def summarize(ev):
    """Overlay load order, the stage packs read (DATA.BNS offsets >= STAGE_MIN between the first
    overlay load and the first XADATA (BGM) read after it) and when that BGM starts."""
    ovl = [(f, n[4:]) for f, n, o in ev if n.startswith('OVL/')]
    first = ovl[0][0] if ovl else 0
    stages, bgm = [], None
    for f, n, o in ev:
        if n.startswith('XADATA') and f > first: bgm = f; break
        if n == 'DATA.BNS' and STAGE_MIN <= o < STAGE_MAX and o not in stages: stages.append(o)
    return {'overlays': [n for _, n in ovl], 'stage_sig': stages, 'bgm_frame': bgm}


def png_stats(path):
    w, h, rows = read_png(path)
    lit = 0; cols = set()
    for r in rows:
        for i in range(0, len(r), 4):
            if r[i + 1] > 40: lit += 1
            cols.add((r[i] >> 4, r[i + 1] >> 4, r[i + 2] >> 4))
    return lit / (w * h), len(cols)


def run_one(scn, outdir, extra=(), dump_every=100, trace=True, timeout=900, verify=True):
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.png'): os.remove(os.path.join(outdir, f))
    cmd = [BIN, '--headless', '--no-interp', '--frames', str(scn.frames), '--press', scn.press()]
    if verify: cmd += ['--verify-overrides']
    if dump_every: cmd += ['--dump', outdir, '--dump-every', str(dump_every)]
    if scn.dsl2: cmd += ['--press2', scn.press2()]
    if trace: cmd += ['--trace']
    cmd += list(extra)
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=ROOT)
    log = r.stdout + r.stderr
    open(os.path.join(outdir, 'log.txt'), 'w').write(log)
    return r.returncode, log, time.time() - t0


def check(scn, outdir, rc, log, expect):
    """Returns (problems, info)."""
    bad = []
    if rc != 0: bad.append(f'exit status {rc}')
    for l in log.splitlines():
        if BAD_LOG.search(l): bad.append('log: ' + l.strip()[:120]); break
    if f'reached {scn.frames} frames' not in log: bad.append('did not reach the last frame')
    info = summarize(cd_loads(log, disc_files()))
    # 1. expected overlay files were loaded, in order
    it = iter(info['overlays'])
    if not all(f in it for f in scn.expect_files):
        bad.append(f'expected overlays {scn.expect_files} in order, loaded {info["overlays"]}')
    # 2. recorded fingerprint (stage + loads) unchanged: catches menu drift that still loads the same overlays
    e = expect.get(scn.name)
    if 'loose' in scn.tags:
        # opponent and stage are picked by the game and shift with any change in boot timing (they flipped
        # Gate -> Villa from an unrelated PR): only require that the P1 fighter's overlay loaded first and
        # that some valid stage pack was read
        if info['overlays'][:1] != scn.expect_files[:1] and scn.expect_files:
            bad.append(f'P1 overlay {info["overlays"][:1]} != {scn.expect_files[:1]}')
        if not set(info['stage_sig']) & set(STAGE_DATA.values()):
            bad.append(f'no known stage pack read ({info["stage_sig"]})')
    elif e is None: bad.append('no recorded fingerprint (run: scenarios.py record)')
    else:
        for k in ('overlays', 'stage_sig'):
            if info[k] != e[k]: bad.append(f'{k} {info[k]} != recorded {e[k]}')
    # 2b. VS scenarios name their stage: that stage's pack (and no other) must have been read
    if scn.name.startswith('vs_'):
        want = STAGE_DATA[scn.name[3:]]
        if want not in info['stage_sig'] or not set(info['stage_sig']) <= {want, want + 49}:
            bad.append(f'stage packs read {info["stage_sig"]}, expected only {want} (+{want + 49}) for {scn.name[3:]}')
    # 3. a fight scenario must have started its BGM (XA read after the last overlay) before the key frame
    if 'fight' in scn.tags or scn.name.startswith(('vs_', '1p_', 'time', 'endur', 'practice', 'team', 'story')):
        if info['bgm_frame'] is None or info['bgm_frame'] > scn.key: bad.append(f'fight not started by key frame {scn.key} (bgm {info["bgm_frame"]})')
    # 4. the key frame is a real picture
    kp = os.path.join(outdir, f'frame_{scn.key:06d}.png')
    if not os.path.exists(kp): bad.append('no key frame dump')
    else:
        lit, ncol = png_stats(kp)
        info['lit'], info['colours'] = round(lit, 2), ncol
        if lit < scn.min_lit or ncol < scn.min_cols: bad.append(f'key frame looks black/flat (lit {lit:.2f}, {ncol} colours)')
    return bad, info


def load_expect():
    try: return json.load(open(EXPECT_PATH))
    except (OSError, ValueError): return {}


def pick(names, args):
    if args.all: return list(SCENARIOS)
    if args.tag: return [n for n, s in SCENARIOS.items() if args.tag in s.tags]
    for n in names:
        if n not in SCENARIOS: sys.exit(f'unknown scenario {n}')
    return names


def job(name, base, extra, keep):
    scn = SCENARIOS[name]
    out = os.path.join(base, name)
    rc, log, dt = run_one(scn, out, extra)
    return name, rc, log, dt, out


def hires_job(name, kind, base):
    """One hi-res run of a scenario: kind 'v1' = 1x1 target compared pixel for pixel with VRAM, 's23' = 2x3."""
    scn = SCENARIOS[name]
    frames = scn.key                 # boot + menus + 300 fight frames is enough for the hi-res checks
    out = os.path.join(base, 'hires_' + name)
    os.makedirs(out, exist_ok=True)
    extra = ['--state-hash', os.path.join(out, kind + '.hash'), '--fight-from', str(scn.fight_from),
             '--hires-min-fight', '95', '--hires-threads', '1', '--frames', str(frames)]
    # 1x1 equivalence needs PGXP off (precise vertices leave the 1x grid); 2x3 runs PGXP on, so the
    # hash comparison also proves PGXP does not change gameplay, on every stage
    extra += ['--scale', '1', '--hires-verify', '--pgxp', 'off'] if kind == 'v1' else ['--scale', '2x3', '--pgxp', 'on']
    rc, log, dt = run_one(scn, out, extra, dump_every=0, trace=False, verify=False)
    open(os.path.join(out, kind + '.log'), 'w').write(log)
    return name, kind, rc, log, out, dt


PGXP_OK = (r'pgxp-check: \d+ triangles with precise vertices; 0 with an on-screen vertex > 1.5 px from its '
           r'integer, 0 with depth ratio > 2, 0 using a value older than 2 frames, 0 split vertices')
UV_OK = r'hires-uv: \d+ textured samples, \d+ clamped to the 1x-sampled UV range, 0 out of range when fetched'


def hires_main(a, extra):
    """Hi-res determinism + 1x1 equivalence on the scenarios tagged 'hires' (every stage): the
    per-frame hash of all emulated state must be identical at 1x1 (--hires-verify) and 2x3, the 1x1
    target (PGXP off) must equal VRAM at every frame, the PGXP vertices drawn at 2x3 must pass
    pgxp-check (no cracks / false associations), no textured sample may fetch outside the 1x-sampled UV
    range and >= 95% of the fight frames must come from the hi-res target."""
    names = a.names or [n for n, s in SCENARIOS.items() if 'hires' in s.tags and not s.xfail]
    jobs = [(n, k) for n in names for k in ('v1', 's23')]
    res = {}
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.j) as ex:
        for name, kind, rc, log, out, dt in ex.map(lambda j: hires_job(j[0], j[1], a.out), jobs):
            res[(name, kind)] = (rc, log, out)
    fail = 0
    for n in names:
        scn = SCENARIOS[n]
        bad = []
        for k in ('v1', 's23'):
            rc, log, out = res[(n, k)]
            if rc != 0 or BAD_LOG.search(log): bad.append(f'{k}: run failed (exit {rc})')
            if not re.search(UV_OK, log): bad.append(f'{k}: UV samples out of range (or no UV report)')
        _, log1, out = res[(n, 'v1')]
        _, log2, _ = res[(n, 's23')]
        if not re.search(PGXP_OK, log2): bad.append('s23: PGXP consistency check failed (or no pgxp-check report)')
        if not re.search(r'hires-verify: %d frames checked \([1-9][0-9]* in fight window\), 0 mismatching' % scn.key, log1):
            bad.append('1x1 hi-res target differs from VRAM (or no fight-window frames)')
        try:
            if open(out + '/v1.hash').read() != open(out + '/s23.hash').read(): bad.append('state hash 2x3 != 1x1')
        except OSError: bad.append('missing state hash')
        m = re.search(r'hires-frames: .*', log2)
        print(f'{"FAIL" if bad else "ok  "} {n:14s} {m.group(0)[:90] if m else ""}')
        for b in bad: print('       ', b)
        fail += bool(bad)
    print(f'hires scenarios: {len(names) - fail}/{len(names)} ok ({len(jobs)} runs, {time.time() - t0:.0f}s)')
    return 1 if fail else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['list', 'press', 'press2', 'run', 'record', 'raw', 'hires'])
    ap.add_argument('names', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--tag')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'scenarios'))
    ap.add_argument('--dump-every', type=int, default=100)
    ap.add_argument('--keep', action='store_true', help='keep all dumped frames (default: only the key frame)')
    ap.add_argument('extra', nargs='*')
    a, rest = ap.parse_known_args()
    extra = rest
    if a.cmd == 'list':
        for n, s in SCENARIOS.items(): print(f'{n:14s} {s.frames:6d} key {s.key:6d}  [{",".join(sorted(s.tags))}] {s.desc}')
        return 0
    if a.cmd == 'press': print(SCENARIOS[a.names[0]].press()); return 0
    if a.cmd == 'press2': print(SCENARIOS[a.names[0]].press2()); return 0
    if a.cmd == 'raw':
        scn = Scn('raw', a.names[0], int(a.names[1]), 0, 'ad hoc', dsl2=os.environ.get('P2', ''))
        rc, log, dt = run_one(scn, a.out + '/raw', extra, a.dump_every)
        info = summarize(cd_loads(log, disc_files()))
        print(rc, f'{dt:.0f}s', info)
        return rc
    if a.cmd == 'hires':
        return hires_main(a, extra)
    names = pick(a.names, a)
    expect = load_expect()
    results = {}
    with cf.ThreadPoolExecutor(a.j) as ex:
        for name, rc, log, dt, out in ex.map(lambda n: job(n, a.out, extra, a.keep), names):
            scn = SCENARIOS[name]
            if a.cmd == 'record' and scn.xfail: continue
            if a.cmd == 'record':
                results[name] = {k: v for k, v in summarize(cd_loads(log, disc_files())).items() if k in ('overlays', 'stage_sig')}
                print(name, results[name]); continue
            if scn.xfail:
                hit = rc != 0 and scn.xfail in log
                print(f'{"xfail" if hit else "FAIL "} {name:14s} {dt:5.1f}s known bug: {scn.xfail}' + ('' if hit else '  (XPASS or different failure: update the scenario)'))
                results[name] = [] if hit else ['xfail did not fail as recorded']
                continue
            bad, info = check(scn, out, rc, log, expect)
            print(f'{"FAIL" if bad else "ok  "} {name:14s} {dt:5.1f}s lit {info.get("lit")} bgm@{info["bgm_frame"]} {" ".join(info["overlays"][-3:])}')
            for b in bad: print('       ', b)
            results[name] = bad
            if not a.keep:
                for f in os.listdir(out):
                    if f.startswith('frame_') and f != f'frame_{scn.key:06d}.png': os.remove(os.path.join(out, f))
    if a.cmd == 'record':
        old = load_expect(); old.update(results)
        json.dump(old, open(EXPECT_PATH, 'w'), indent=1, sort_keys=True); print('wrote', EXPECT_PATH)
        return 0
    failed = [n for n, b in results.items() if b]
    print(f'scenarios: {len(names) - len(failed)}/{len(names)} ok' + (f', FAILED: {" ".join(failed)}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
