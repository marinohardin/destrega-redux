#!/usr/bin/env python3
"""Native 3D stage (--hlr on) against the original renderer (--hlr off), every VS stage: the key frames
dumped by both at 4x6 16:9, contact sheets and a difference table. Needs the disc; Pillow + NumPy for
the sheets and the table. Development tool (docs/HLR_PLAN.md 2b), not part of any test suite.

  tools/hlr_diff.py [--stages "hills castle1"] [--api metal|vulkan] [--out DIR] [--keep] [--no-run]
                    [-j N] [EXTRA ARGS after --]

  Per stage: <out>/<stage>_off/ and _on/ hold the dumps (deleted after the sheets unless --keep),
  <out>/<stage>.png the sheet (off | on | |difference| x 4 for the key frame and key + 20),
  <out>/all_on.png every stage's native key frame, <out>/table.txt:
    >1 / >4     % of pixels off by more than 1 / 4 15-bit steps (1 step = 8 of 255) in any channel
    mae         mean absolute error per channel, 0..255
    guard       % of the guard-band pixels (outside the 4:3 centre) that differ by > 4 steps
    black_on    % of the on frame's pixels that are pure black (gaps, cracks) vs the off frame's
    passes, polys  native passes and polygons per frame (hlr-native-draw line)
Runs under nice; -j (default 1) game processes at a time: with more than one it waits for and takes
build/heavy-test.lock in the main checkout (the shared machine's rule), removed at the end.
"""
import os, re, subprocess, sys, shutil, time, concurrent.futures as cf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import scenarios  # noqa: E402

STAGES = ['hills', 'cave', 'villa', 'canyon', 'gate', 'castle1', 'castle2', 'throne', 'remains', 'hamlet',
          'shrine', 'fortress']
BIN = os.environ.get('DESTREGA_BIN', os.path.join(ROOT, 'build/cmake/destrega'))


def lock_path():
    # the shared machine's heavy-test lock lives in the main checkout's build/ (worktrees: two levels up)
    p = ROOT
    while p != '/' and not os.path.isdir(os.path.join(p, '.git')):
        p = os.path.dirname(p)
    return os.path.join(p, 'build', 'heavy-test.lock')


def run(out, stage, mode, api, extra):
    scn = f'vs_{stage}'
    s = scenarios.SCENARIOS[scn]
    d = os.path.join(out, f'{stage}_{mode}')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = ['nice', '-n', '10', BIN, '--headless', '--frames', str(s.key + 20), '--press', s.press()]
    if s.dsl2:
        cmd += ['--press2', s.press2()]
    cmd += ['--renderer', api, '--hlr', mode, '--scale', '4x6', '--widescreen', '--window', '1280x720',
            '--dump', d, '--dump-from', str(s.key), '--dump-every', '20'] + extra
    with open(d + '.log', 'w') as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
    return rc


def stats(out, stage):
    import numpy as np
    from PIL import Image
    s = scenarios.SCENARIOS[f'vs_{stage}']
    rows, imgs = [], []
    for k in (s.key, s.key + 20):
        fn = f'frame_{k:06d}.png'
        a = np.asarray(Image.open(os.path.join(out, f'{stage}_off', fn)).convert('RGB')).astype(np.int16)
        b = np.asarray(Image.open(os.path.join(out, f'{stage}_on', fn)).convert('RGB')).astype(np.int16)
        if a.shape != b.shape:
            return None, []
        d = np.abs(a - b).max(axis=2)
        h, w = d.shape
        cw = int(round(h * 4 / 3 * (w / h) / (w / h)))          # the 4:3 centre is h * 4/3 wide
        x0 = (w - cw) // 2
        guard = np.concatenate([d[:, :x0], d[:, x0 + cw:]], axis=1) if x0 > 0 else d[:, :0]
        rows.append(dict(gt1=100.0 * (d > 8).mean(), gt4=100.0 * (d > 32).mean(), mae=float(np.abs(a - b).mean()),
                         guard=100.0 * (guard > 32).mean() if guard.size else 0.0,
                         black_on=100.0 * (b.max(axis=2) == 0).mean(), black_off=100.0 * (a.max(axis=2) == 0).mean()))
        diff = np.clip(np.abs(a - b) * 4, 0, 255).astype(np.uint8)
        imgs += [Image.fromarray(a.astype(np.uint8)), Image.fromarray(b.astype(np.uint8)), Image.fromarray(diff)]
    avg = {k: sum(r[k] for r in rows) / len(rows) for k in rows[0]}
    return avg, imgs


def sheet(path, imgs, cols, cell_w):
    from PIL import Image
    w0, h0 = imgs[0].size
    cw, ch = cell_w, int(cell_w * h0 / w0)
    rows = (len(imgs) + cols - 1) // cols
    out = Image.new('RGB', (cw * cols, ch * rows))
    for i, im in enumerate(imgs):
        out.paste(im.resize((cw, ch), Image.LANCZOS), ((i % cols) * cw, (i // cols) * ch))
    out.save(path)


def native_line(out, stage):
    try:
        log = open(os.path.join(out, f'{stage}_on.log'), errors='replace').read()
    except OSError:
        return None
    m = re.search(r'hlr-native-draw: \d+ frames, ([\d.]+) passes per frame, (\d+) polygons per frame', log)
    return (float(m.group(1)), int(m.group(2))) if m else None


def main():
    args = sys.argv[1:]
    extra = []
    if '--' in args:
        extra = args[args.index('--') + 1:]
        args = args[:args.index('--')]
    stages, api, out, keep, norun, jobs = STAGES, 'gpu', os.path.join(ROOT, 'build', 'hlr2b', 'diff'), False, False, 1
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--stages': stages = args[i + 1].split(); i += 1
        elif a == '--api': api = args[i + 1]; i += 1
        elif a == '--out': out = os.path.abspath(args[i + 1]); i += 1
        elif a == '--keep': keep = True
        elif a == '--no-run': norun = True
        elif a == '-j': jobs = int(args[i + 1]); i += 1
        else: sys.exit(__doc__)
        i += 1
    os.makedirs(out, exist_ok=True)
    lock = None
    if not norun:
        if jobs > 1:
            lock = lock_path()
            while os.path.exists(lock):
                print('waiting for', lock); time.sleep(30)
            open(lock, 'w').write('hlr')
        try:
            items = [(st, m) for st in stages for m in ('off', 'on')]
            with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
                for (st, m), rc in zip(items, pool.map(lambda x: run(out, x[0], x[1], api, extra), items)):
                    if rc:
                        print(f'{st} {m}: exit {rc}')
        finally:
            if lock and os.path.exists(lock):
                os.remove(lock)
    table = ['stage       >1%     >4%    mae   guard>4%  black_on  black_off  passes  polys']
    overview = []
    for st in stages:
        avg, imgs = stats(out, st)
        if not avg:
            table.append(f'{st:<10}  (missing or mismatched dumps)')
            continue
        nl = native_line(out, st)
        table.append(f'{st:<10} {avg["gt1"]:6.2f} {avg["gt4"]:7.2f} {avg["mae"]:6.2f} {avg["guard"]:9.2f}'
                     f' {avg["black_on"]:9.3f} {avg["black_off"]:10.3f}  {nl[0] if nl else "?":>6} {nl[1] if nl else "?":>6}')
        sheet(os.path.join(out, f'{st}.png'), imgs, 3, 960)
        overview.append(imgs[1])
        if not keep:
            for m in ('off', 'on'):
                shutil.rmtree(os.path.join(out, f'{st}_{m}'), ignore_errors=True)
    if overview:
        sheet(os.path.join(out, 'all_on.png'), overview, 3, 960)
    open(os.path.join(out, 'table.txt'), 'w').write('\n'.join(table) + '\n')
    print('\n'.join(table))
    print('sheets and table in', out)


if __name__ == '__main__':
    main()
