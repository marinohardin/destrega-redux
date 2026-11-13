#!/usr/bin/env python3
"""Native high-level renderer (runtime/hlr/, docs/HLR_PLAN.md) checks, phases 2a, 2b, 3 and 4. Needs the disc.

  tools/test_hlr.py [CASE...] [-j N]        CASES: audit chars hash exact seams rewind (default: all)
  CASES="hash exact" STAGES="castle1" ...    environment selection (tools/pr_check.sh group hlr)

  audit   --hlr audit on all 12 VS stages, practice and story: every stage packet the game links is
          predicted exactly (0 mismatched, 0 unexplained, 0 missing, 0 slot heads, 0 bad walks).
  hash    --renderer gpu --no-interp --verify-overrides, --hlr on vs off: per-frame --state-hash
          byte-identical (vs_hills 4:3 4x6, vs_castle1 16:9 4x6, story 2x3) and the stage drawn in
          native 3D (hlr-native-draw: polygons drawn, no GPU pass failed), and the characters too
          (hlr-native-chars: faces drawn in 3D, every model attached). The key frames differ by
          design since 2b (tools/hlr_diff.py compares them); the count is printed.
  exact   every VS stage, --hlr exact vs off on Metal and (if it loads) Vulkan: one run each that
          dumps the stage at 4:3 2x3, 16:9 2x3, 16:9 4x6 and 4:3 4x6 (View-menu actions switch
          between frames); the dumps and state hashes must be byte-identical.
          API="metal" / API="metal vulkan" picks the APIs, STAGES="hills castle1" the stages (also
          for audit; AUDIT_MODES="practice story" the other modes audited, HASH="vs_hills story" the
          hash runs).
  chars   --hlr audit on the 12 practice_<char> replays (tools/replays/, every character performs
          the whole command battery): the character audit reproduces every vertex, colour and face
          the game builds (hlr-chars: 0 mismatched, 0 linked differently, 0 other words, 0 bad, 0
          rejected). CHARS="milena reyus" picks the characters. (audit also checks hlr-chars.)
          Both audit and chars also check the fighter shadows (phase 4, hlr-shadows: every vertex's
          RTPS, triangle word, grey and OT slot as the game builds them; 0 mismatched, 0 rejected),
          and hash requires them drawn natively (hlr-native-shadows) on every run.
  seams   vs_gate (Milena vs Doyle) in the user configuration (GPU fast precision, 4x6 16:9, xbr, full
          colour, PGXP on, the HLR's default 4x MSAA) with a pause (Start) in the fight: --hlr on vs
          off, tools/check_hlr_seams.py on the fight and pause frames finds no thin bright line that
          only the HLR draws (MSAA passes that restarted from the resolve drew the floor's pass
          boundaries around the fighters as such lines). Negative check: DESTREGA_HLR_TEST_NOKEEP=1
          brings the old behaviour back and the check must find seams in every frame.
  rewind  --savestate-rewind 37:23 with --hlr on (GPU, 2x3) vs a plain run with the HLR off: equal
          per-frame complete-state hashes, the HLR dropped its captures on every load and drew the
          stage natively.

Runs under nice, at most JOBS (default 2) game processes. Output in build/hlr_test/.
"""
import os, re, subprocess, sys, filecmp, concurrent.futures as cf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import scenarios  # noqa: E402

BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))
OUT = os.path.join(ROOT, 'build', 'hlr_test')
STAGES = ['hills', 'cave', 'villa', 'canyon', 'gate', 'castle1', 'castle2', 'throne', 'remains', 'hamlet',
          'shrine', 'fortress']


def run(tag, scn, frames, extra, env=None, press=None):
    """One headless run of scenario scn (press: its pad-1 script with more taps); returns (rc, log text)."""
    s = scenarios.SCENARIOS[scn]
    cmd = ['nice', '-n', '10', BIN, '--headless', '--frames', str(frames), '--press', press or s.press()]
    if s.dsl2:
        cmd += ['--press2', s.press2()]
    cmd += extra
    log = os.path.join(OUT, tag + '.log')
    with open(log, 'w') as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT,
                             env=dict(os.environ, **(env or {})))
    return rc, open(log, errors='replace').read()


def hlr_line(log):
    m = re.search(r'hlr: mode \S+, parts \S+; .*?(\d+) packets, (\d+) drawn by the HLR, (\d+) stale', log)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def native_line(log):
    """(polygons per frame, frames, failed GPU passes) of the hlr-native-draw line, or None"""
    m = re.search(r'hlr-native-draw: (\d+) frames, [\d.]+ passes per frame, (\d+) polygons per frame.*?(\d+) failed', log)
    return (int(m.group(2)), int(m.group(1)), int(m.group(3))) if m else None


CHARS_RE = re.compile(r'hlr-chars: (\d+) models drawn by the game, (\d+) audited; vertices (\d+), (\d+) mismatched; '
                      r'colours (\d+), (\d+) mismatched; faces (\d+) \((\d+) kept by the game\), (\d+) linked '
                      r'differently, (\d+) with other words; (\d+) bad models, (\d+) rejected; .*?mean ([\d.]+) px')


def chars_audit(log):
    """(ok, text) of the hlr-chars audit line; ok None when the run drew no character"""
    m = CHARS_RE.search(log)
    if not m:
        return None, 'no character audit'
    calls, aud, v, vb, c, cb, fa, kept, lb, wb, bad, rej = map(int, m.groups()[:12])
    ok = calls > 0 and aud == calls and vb == cb == lb == wb == bad == rej == 0
    return ok, (f'characters: {aud}/{calls} models audited, {v} vertices ({vb} wrong), {c} colours ({cb} wrong), '
                f'{fa} faces ({kept} kept; {lb} linked differently, {wb} other words), {bad} bad, {rej} rejected; '
                f'float bones vs SXY {m.group(13)} px')


SHADOWS_RE = re.compile(r'hlr-shadows: (\d+) shadows drawn by the game, (\d+) audited; vertices (\d+), (\d+) mismatched; '
                        r'triangles (\d+), (\d+) with other words, (\d+) with another grey, (\d+) linked elsewhere, '
                        r'(\d+) bad indices; (\d+) with another camera matrix, (\d+) rejected')


def shadows_audit(log):
    """(ok, text) of the hlr-shadows audit line (phase 4); ok None when the run drew no shadow"""
    m = SHADOWS_RE.search(log)
    if not m:
        return None, 'no shadow audit'
    calls, aud, v, vb, t, wb, gb, lb, ib, ws, rej = map(int, m.groups())
    ok = calls > 0 and aud == calls and vb == wb == gb == lb == ib == ws == rej == 0
    return ok, (f'shadows: {aud}/{calls} audited, {v} vertices ({vb} wrong), {t} triangles ({wb} other words, '
                f'{gb} other grey, {lb} linked elsewhere, {ib} bad indices), {ws} other matrix, {rej} rejected')


def shadows_native(log):
    """(triangles per frame drawn in 3D, attached, without a frame, other camera) or None"""
    m = re.search(r'hlr-native-shadows: (\d+) shadow triangles per frame drawn in 3D', log)
    a = re.search(r'hlr-shadows: (\d+) attached to a native frame, (\d+) without a frame, (\d+) with another camera', log)
    return (int(m.group(1)), int(a.group(1)), int(a.group(2)), int(a.group(3))) if m and a else None


def chars_native(log):
    """(faces per frame drawn in 3D, models attached, without a frame, other camera) or None"""
    m = re.search(r'hlr-native-chars: (\d+) character faces per frame drawn in 3D', log)
    a = re.search(r'hlr-chars: (\d+) models attached to a native frame, (\d+) without a frame, (\d+) with another camera', log)
    return (int(m.group(1)), int(a.group(1)), int(a.group(2)), int(a.group(3))) if m and a else None


FX_RE = re.compile(r'hlr-fx: (\d+) effect polygons built by the game, (\d+) audited, (\d+) facing away \(not drawn\); '
                   r'vertices (\d+), (\d+) mismatched; (\d+) with other words, (\d+) linked elsewhere, '
                   r'(\d+) without a draw-mode word, (\d+) rejected')


def fx_audit(log):
    """(ok, text) of the hlr-fx audit line (phase 5); ok None when the run built no effect"""
    m = FX_RE.search(log)
    if not m:
        return None, 'no effect audit'
    calls, aud, away, v, vb, wb, lb, mb, rej = map(int, m.groups())
    ok = calls > 0 and aud + away == calls and vb == wb == lb == mb == rej == 0
    return ok, (f'effects: {aud}/{calls} audited ({away} facing away), {v} vertices ({vb} wrong), {wb} other words, '
                f'{lb} linked elsewhere, {mb} without a draw mode, {rej} rejected')


def fx_native(log):
    """(effect polygons per frame drawn in 3D, attached, without a frame, other camera) or None"""
    m = re.search(r'hlr-native-fx: ([\d.]+) effect polygons per frame drawn in 3D', log)
    a = re.search(r'hlr-fx: (\d+) attached to a native frame, (\d+) without a frame, (\d+) with another camera', log)
    return (float(m.group(1)), int(a.group(1)), int(a.group(2)), int(a.group(3))) if m and a else None


def same_files(a, b):
    return os.path.exists(a) and os.path.exists(b) and filecmp.cmp(a, b, shallow=False)


# ---- audit ------------------------------------------------------------------------------------------
def audit_one(scn):
    s = scenarios.SCENARIOS[scn]
    rc, log = run('audit_' + scn, scn, s.frames, ['--no-interp', '--hlr', 'audit'])
    m = re.search(r'hlr-audit: \d+ items, (\d+) packets, (\d+) mismatched, (\d+) unexplained packets \((\d+) frames;'
                  r'.*?; (\d+) missing; (\d+) slot heads differ; .*?; (\d+) bad walks', log)
    alt = re.search(r'hlr-audit-alt: (.*)', log)
    if rc or not m:
        return False, f'{scn}: exit {rc}, no audit line' if not m else f'{scn}: exit {rc}'
    pk, mis, un, fr, miss, heads, bad = map(int, m.groups())
    cok, ctext = chars_audit(log)
    sok, stext = shadows_audit(log)
    fok, ftext = fx_audit(log)
    ok = (fr > 0 and pk > 0 and mis == un == miss == heads == bad == 0 and cok is not False and sok is not False
          and fok is not False)
    return ok, (f'{scn}: {fr} frames, {pk} packets, {mis} mismatched, {un} unexplained, {miss} missing, '
                f'{heads} heads, {bad} bad walks; {alt.group(1) if alt else "?"}; {ctext}; {stext}; {ftext}')


def chars_one(char):
    import replays
    rp = replays.REPLAYS[f'practice_{char}']
    log_path = os.path.join(OUT, f'chars_{char}.log')
    cmd = ['nice', '-n', '10', BIN, '--headless', '--no-interp', '--hlr', 'audit', '--replay', rp.path]
    with open(log_path, 'w') as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
    log = open(log_path, errors='replace').read()
    cok, ctext = chars_audit(log)
    sok, stext = shadows_audit(log)
    fok, ftext = fx_audit(log)
    return bool(cok) and bool(sok) and bool(fok) and rc == 0, f'practice_{char}: exit {rc}; {ctext}; {stext}; {ftext}'


def case_chars(pool):
    import scenarios as S
    chars = os.environ.get('CHARS', ' '.join(S.ROSTER)).split()
    return list(pool.map(chars_one, chars))


def case_audit(pool):
    stages = os.environ.get('STAGES', ' '.join(STAGES)).split()
    names = [f'vs_{st}' for st in stages] + os.environ.get('AUDIT_MODES', 'practice story').split()
    return list(pool.map(audit_one, names))


# ---- hash -------------------------------------------------------------------------------------------
def hash_pair(item):
    scn, extra, frames = item
    s = scenarios.SCENARIOS[scn]
    res = {}
    for mode in ('off', 'on'):
        tag = f'hash_{scn}_{mode}'
        d = os.path.join(OUT, tag)
        os.makedirs(d, exist_ok=True)
        args = ['--renderer', 'gpu', '--no-interp', '--verify-overrides', '--hlr', mode,
                '--state-hash', os.path.join(OUT, tag + '.hash'), '--dump', d, '--dump-every', '100',
                '--dump-from', str(s.key)] + extra
        res[mode] = run(tag, scn, frames, args)
    (rc0, _), (rc1, log1) = res['off'], res['on']
    if rc0 or rc1:
        return False, f'{scn}: exit {rc0} / {rc1}'
    nl = native_line(log1)
    same = same_files(os.path.join(OUT, f'hash_{scn}_off.hash'), os.path.join(OUT, f'hash_{scn}_on.hash'))
    d0, d1 = os.path.join(OUT, f'hash_{scn}_off'), os.path.join(OUT, f'hash_{scn}_on')
    dumps = sorted(f for f in os.listdir(d0) if f.startswith('frame_0'))
    ndiff = sum(not same_files(os.path.join(d0, f), os.path.join(d1, f)) for f in dumps)
    cn = chars_native(log1)
    sn = shadows_native(log1)
    fn = fx_native(log1)
    ok = (same and nl and nl[0] > 0 and nl[2] == 0 and dumps and cn and cn[0] > 0 and cn[1] > 0 and cn[3] == 0
          and sn and sn[0] > 0 and sn[1] > 0 and sn[3] == 0 and (fn is None or (fn[2] == 0 and fn[3] == 0)))
    return ok, (f'{scn}: state hash {"identical" if same else "DIFFERS"}; native 3D stage in '
                f'{nl[1] if nl else "?"} frames, {nl[0] if nl else "?"} polygons per frame, '
                f'{nl[2] if nl else "?"} failed GPU passes; characters: {cn[0] if cn else "?"} faces per frame in 3D, '
                f'{cn[1] if cn else "?"} models attached, {cn[2] if cn else "?"} without a frame, '
                f'{cn[3] if cn else "?"} with another camera; shadows: {sn[0] if sn else "?"} triangles per frame in 3D, '
                f'{sn[1] if sn else "?"} attached, {sn[3] if sn else "?"} with another camera; '
                f'effects: {f"{fn[0]} polygons per frame in 3D, {fn[1]} attached, {fn[2]} without a frame, {fn[3]} with another camera" if fn else "none drawn"}; '
                f'{ndiff} of {len(dumps)} key frames differ from off')


def case_hash(pool):
    items = [('vs_hills', ['--scale', '4x6'], 3100),
             ('vs_castle1', ['--scale', '4x6', '--widescreen', '--window', '1280x720'], 3500),
             ('story', ['--scale', '2x3'], 15600)]
    want = os.environ.get('HASH')
    if want: items = [i for i in items if i[0] in want.split()]
    return list(pool.map(hash_pair, items))


# ---- exact ------------------------------------------------------------------------------------------
def exact_args(k, d):
    """Four configurations in one run: hi-res from k-20, dumps at k, k+10, k+20, k+30."""
    acts = [f'{k - 20}:resolution=2x3', f'{k + 5}:widescreen=on', f'{k + 15}:resolution=4x6',
            f'{k + 25}:widescreen=off']
    a = ['--window', '1280x720', '--dump', d, '--dump-from', str(k), '--dump-every', '10']
    for x in acts:
        a += ['--menu-action', x]
    return a


def exact_one(item):
    stage, api = item[:2]
    drop = item[2] if len(item) > 2 else 0     # negative check: the HLR leaves out every Nth packet
    scn = f'vs_{stage}'
    s = scenarios.SCENARIOS[scn]
    k, frames = s.key, s.key + 30
    out = {}
    for mode in ('off', 'exact'):
        tag = f'exact_{api}_{stage}_{mode}'
        if drop:
            tag = f'neg_{api}_{stage}_{mode}'
        d = os.path.join(OUT, tag)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        args = ['--renderer', api, '--hlr', mode, '--state-hash', os.path.join(OUT, tag + '.hash')]
        out[mode] = run(tag, scn, frames, args + exact_args(k, d),
                        env={'DESTREGA_HLR_TEST_DROP': str(drop)} if drop and mode == 'exact' else None)
    (rc0, log0), (rc1, log1) = out['off'], out['exact']
    if 'GPU renderer unavailable' in log0 or 'is not offered' in log0 or (rc0 and api == 'vulkan'):
        return None, f'{api} {stage}: skipped (no {api} device)'
    if rc0 or rc1:
        return False, f'{api} {stage}: exit {rc0} / {rc1}'
    pre = 'neg' if drop else 'exact'
    d0, d1 = (os.path.join(OUT, f'{pre}_{api}_{stage}_{m}') for m in ('off', 'exact'))
    frames_ = [f'frame_{k + 10 * i:06d}.png' for i in range(4)]
    diff = [f for f in frames_ if not same_files(os.path.join(d0, f), os.path.join(d1, f))]
    hsame = same_files(os.path.join(OUT, f'{pre}_{api}_{stage}_off.hash'),
                       os.path.join(OUT, f'{pre}_{api}_{stage}_exact.hash'))
    hl = hlr_line(log1)
    if drop:
        ok = bool(diff) and hsame
        return ok, (f'{api} {stage} with every {drop}th packet left out: {len(diff)} of 4 dumps differ '
                    f'({"noticed" if diff else "NOT NOTICED"}), state hash {"identical" if hsame else "DIFFERS"}')
    ok = not diff and hsame and hl and hl[1] > 0 and hl[2] == 0
    return ok, (f'{api} {stage}: dumps {"identical" if not diff else "DIFFER: " + " ".join(diff)} '
                f'(4:3 2x3, 16:9 2x3, 16:9 4x6, 4:3 4x6), state hash {"identical" if hsame else "DIFFERS"}; '
                f'HLR drew {hl[1] if hl else "?"} packets, {hl[2] if hl else "?"} stale')


def case_exact(pool):
    apis = os.environ.get('API', 'metal vulkan').split()
    stages = os.environ.get('STAGES', ' '.join(STAGES)).split()
    items = [(st, a) for a in apis for st in stages] + [(stages[0], apis[0], 50)]
    return list(pool.map(exact_one, items))


# ---- seams ------------------------------------------------------------------------------------------
SEAM_FRAMES = list(range(3320, 3441, 40))     # dumps fall on multiples of 40; Start at 3350: paused from ~3356


def seams_run(item):
    mode, nokeep = item
    tag = f'seams_{mode}' + ('_nokeep' if nokeep else '')
    # the negative check keeps the shadows on the original path: native shadows (phase 4) no longer
    # split the passes around the fighters, so without them the old restart has few seams left to show
    parts = ['--hlr-parts', 'stage,chars'] if nokeep else []
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    s = scenarios.SCENARIOS['vs_gate']
    args = ['--renderer', 'gpu', '--gpu-precision', 'fast', '--scale', '4x6',
            '--widescreen', '--window', '1280x720', '--tex-filter', 'xbr', '--hires-colour', 'full',
            '--pgxp', 'on', '--hlr', mode, '--dump', d, '--dump-from', str(SEAM_FRAMES[0]),
            '--dump-every', '40'] + parts
    rc, log = run(tag, 'vs_gate', SEAM_FRAMES[-1] + 1, args,
                  env={'DESTREGA_HLR_TEST_NOKEEP': '1'} if nokeep else None, press=s.press() + ',3350:8')
    return rc, log, d


def case_seams(pool):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import check_hlr_seams
    res = dict(zip(['off', 'on', 'nokeep'], pool.map(seams_run, [('off', 0), ('on', 0), ('on', 1)])))
    if any(r[0] for r in res.values()):
        return [(False, 'seams: exit ' + ' / '.join(str(r[0]) for r in res.values()))]
    nl = native_line(res['on'][1])
    out = []
    for k in ('on', 'nokeep'):
        counts = []
        for fr in SEAM_FRAMES:
            f = f'frame_{fr:06d}.png'
            counts.append(len(check_hlr_seams.seams(os.path.join(res[k][2], f), os.path.join(res['off'][2], f))))
        if k == 'on':
            ok = not any(counts) and nl and nl[0] > 0 and nl[2] == 0
            out.append((ok, f'seams: vs_gate fight + pause, 4x6 16:9 fast: {sum(counts)} seams in '
                            f'{len(counts)} frames ({" ".join(map(str, counts))}); native stage in '
                            f'{nl[1] if nl else "?"} frames'))
        else:
            ok = all(counts)
            out.append((ok, f'seams: with every MSAA pass restarted from the resolve (old behaviour, shadows original): '
                            f'{" ".join(map(str, counts))} seams per frame '
                            f'({"noticed" if ok else "NOT NOTICED in every frame"})'))
    return out


# ---- rewind -----------------------------------------------------------------------------------------
def case_rewind(pool):
    scn = 'vs_castle1'
    frames = 3500
    base = ['--renderer', 'gpu', '--scale', '2x3', '--no-interp']
    jobs = {
        'plain': base + ['--hlr', 'off', '--ss-hash', os.path.join(OUT, 'rewind_plain.txt')],
        'rewind': base + ['--hlr', 'on', '--savestate-rewind', '37:23',
                          '--ss-hash', os.path.join(OUT, 'rewind_hlr.txt')],
    }
    res = dict(zip(jobs, pool.map(lambda kv: run('rewind_' + kv[0], scn, frames, kv[1]), jobs.items())))
    (rc0, _), (rc1, log1) = res['plain'], res['rewind']
    if rc0 or rc1:
        return [(False, f'rewind: exit {rc0} / {rc1}')]
    same = same_files(os.path.join(OUT, 'rewind_plain.txt'), os.path.join(OUT, 'rewind_hlr.txt'))
    m = re.search(r'(\d+) dropped \((\d+) save-state loads\)', log1)
    nl = native_line(log1)
    ok = same and m and int(m.group(2)) > 0 and nl and nl[0] > 0 and nl[2] == 0
    return [(ok, f'rewind: per-frame state hashes {"identical" if same else "DIFFER"}; '
                 f'{m.group(2) if m else "?"} loads, {m.group(1) if m else "?"} captures dropped, '
                 f'native stage in {nl[1] if nl else "?"} frames ({nl[0] if nl else "?"} polygons per frame)')]


CASES = {'audit': case_audit, 'chars': case_chars, 'hash': case_hash, 'exact': case_exact, 'seams': case_seams, 'rewind': case_rewind}


def main():
    args = sys.argv[1:]
    jobs = int(os.environ.get('JOBS', '2'))
    if '-j' in args:
        i = args.index('-j')
        jobs = int(args[i + 1])
        del args[i:i + 2]
    names = args or os.environ.get('CASES', 'audit chars hash exact seams rewind').split()
    for n in names:
        if n not in CASES:
            sys.exit(f'test_hlr: unknown case {n} ({" ".join(CASES)})')
    if not os.path.exists(BIN):
        sys.exit(f'test_hlr: {BIN} missing')
    os.makedirs(OUT, exist_ok=True)
    fail = 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        for n in names:
            for ok, msg in CASES[n](pool):
                print(f'{n:6s} {"PASS" if ok else ("skip" if ok is None else "FAIL")}  {msg}', flush=True)
                fail += ok is False
    print(f'test_hlr: {"FAIL (" + str(fail) + ")" if fail else "PASS"}')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
