#!/usr/bin/env python3
"""HLR phase 0 probes on raw RAM snapshots (`--ram-snap DIR --ram-snap-every 1 --ram-snap-from F`,
or one `--ram-dump`). Read-only analysis; nothing here touches the game.

  hlr_probe.py coords SNAP...           GsCOORDINATE2 layout check (experiment C): for both
                                        fighters and every bone part, does the +0x48 (super) chain
                                        of local matrices reproduce workm exactly?
  hlr_probe.py jitter DIR [--box x0,y0,x1,y1] [--fighter 0|1]
                                        experiment B: head screen position per game update three
                                        ways (game workm / float chain / float chain from the
                                        param angles) plus smoothing; step size, step cosine, lag.

Layout (analysis/NOTES.md "Character bones (GsCOORDINATE2)"): fighter 800c9938 (P1) / 800c9b14
(P2), part count +0xf0, parts +0xf4 (0x70 each), part word 0 -> coord. coord: +0 flg, +4 local
MATRIX (m 3x3 s16, t at +0x18), +0x24 workm (m, t at +0x38), +0x44 param (GsCOORD2PARAM: scale
VECTOR, rotate SVECTOR at +0x10, trans VECTOR at +0x18), +0x48 super. Screen matrix = WS (800ed4c8)
x workm; H (follow camera) halfword 800c0394.
"""
import argparse, glob, math, os, struct, sys

FIGHTERS = (0x800c9938, 0x800c9b14)
WS = 0x800ed4c8
H_ADDR = 0x800c0394


class Ram:
    def __init__(self, path):
        self.d = open(path, 'rb').read()

    def u32(self, a): return struct.unpack_from('<I', self.d, a & 0x1fffff)[0]
    def s32(self, a): return struct.unpack_from('<i', self.d, a & 0x1fffff)[0]
    def s16(self, a): return struct.unpack_from('<h', self.d, a & 0x1fffff)[0]
    def u16(self, a): return struct.unpack_from('<H', self.d, a & 0x1fffff)[0]

    def mat(self, a):
        return [[self.s16(a + 2 * (3 * i + j)) for j in range(3)] for i in range(3)], [self.s32(a + 0x14 + 4 * k) for k in range(3)]

    def parts(self, f):
        n, arr = self.u32(f + 0xf0), self.u32(f + 0xf4)
        return [arr + 0x70 * i for i in range(n)] if n < 64 else []

    def chain(self, c):
        out = []
        while c and (c & 0xff000000) == 0x80000000 and len(out) < 64:
            out.append(c)
            c = self.u32(c + 0x48)
        return out          # leaf .. root


def imul(P, C):
    """CompMatrix-style integer product: R = P.R*C.R >> 12, T = P.R*C.T >> 12 + P.T."""
    PR, PT = P
    R, T = C
    return ([[sum(PR[i][k] * R[k][j] for k in range(3)) >> 12 for j in range(3)] for i in range(3)],
            [(sum(PR[i][k] * T[k] for k in range(3)) >> 12) + PT[i] for i in range(3)])


def fmul(P, C):
    PR, PT = P
    R, T = C
    return ([[sum(PR[i][k] * R[k][j] for k in range(3)) for j in range(3)] for i in range(3)],
            [sum(PR[i][k] * T[k] for k in range(3)) + PT[i] for i in range(3)])


def fmat(m):
    R, T = m
    return [[x / 4096.0 for x in r] for r in R], [float(t) for t in T]


def rot_from_angles(ax, ay, az, order):
    """Float rotation from 12-bit angles (4096 = one turn)."""
    def rx(a): c, s = math.cos(a), math.sin(a); return [[1, 0, 0], [0, c, -s], [0, s, c]]
    def ry(a): c, s = math.cos(a), math.sin(a); return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    def rz(a): c, s = math.cos(a), math.sin(a); return [[c, -s, 0], [s, c, 0], [0, 0, 1]]
    k = 2 * math.pi / 4096
    m = {'x': rx(ax * k), 'y': ry(ay * k), 'z': rz(az * k)}
    def mm(a, b): return [[sum(a[i][t] * b[t][j] for t in range(3)) for j in range(3)] for i in range(3)]
    out = m[order[0]]
    for o in order[1:]: out = mm(out, m[o])
    return out


ORDERS = ('xyz', 'xzy', 'yxz', 'yzx', 'zxy', 'zyx')


def param_local(ram, c, order):
    p = ram.u32(c + 0x44)
    sc = [ram.s32(p + 4 * k) / 4096.0 for k in range(3)]
    ang = [ram.s16(p + 0x10 + 2 * k) for k in range(3)]
    tr = [float(ram.s32(p + 0x18 + 4 * k)) for k in range(3)]
    R = rot_from_angles(*ang, order)
    R = [[R[i][j] * sc[j] for j in range(3)] for i in range(3)]
    return R, tr


def cmd_coords(a):
    worst = [0, 0]
    nparts = nchk = 0
    orders = {o: 0.0 for o in ORDERS}
    for path in a.snaps:
        ram = Ram(path)
        for f in FIGHTERS:
            for part in ram.parts(f):
                c = ram.u32(part)
                ch = ram.chain(c)
                W = ram.mat(ch[-1] + 4)
                for x in reversed(ch[:-1]):
                    W = imul(W, ram.mat(x + 4))
                    nchk += 1
                WR, WT = ram.mat(c + 0x24)
                worst[0] = max(worst[0], max(abs(WR[i][j] - W[0][i][j]) for i in range(3) for j in range(3)))
                worst[1] = max(worst[1], max(abs(WT[i] - W[1][i]) for i in range(3)))
                nparts += 1
                # local matrix vs param angles (which rotation order does RotMatrix use?)
                for o in ORDERS:
                    R, _ = param_local(ram, c, o)
                    L, _ = ram.mat(c + 4)
                    orders[o] = max(orders[o], max(abs(L[i][j] - R[i][j] * 4096) for i in range(3) for j in range(3)))
    print(f'coords: {len(a.snaps)} snapshots, {nparts} parts ({nchk} chain links): '
          f'workm vs integer chain product max |dR| {worst[0]}, max |dT| {worst[1]}')
    print('local matrix vs param angles, max |d| (4096 = 1.0) per rotation order: ' +
          ', '.join(f'{o} {v:.1f}' for o, v in orders.items()))
    return 0 if worst == [0, 0] else 1


def project(H, RT, TR, v):
    X = TR[0] + sum(RT[0][k] * v[k] for k in range(3))
    Y = TR[1] + sum(RT[1][k] * v[k] for k in range(3))
    Z = TR[2] + sum(RT[2][k] * v[k] for k in range(3))
    return (H * X / Z, H * Y / Z) if Z > 0 else None


def head_positions(ram, f, H, mode, order, sel):
    """Screen positions (centred 1x pixels) of the selected vertices, {(part, i): (x, y)}."""
    WSm = ram.mat(WS)
    out = {}
    for pi, part in enumerate(ram.parts(f)):
        c = ram.u32(part)
        if mode == 'game':
            W = ram.mat(c + 0x24)
            W = ([r[:] for r in W[0]], [((t + 0x8000) & 0xffff) - 0x8000 for t in W[1]])   # t used as 16 bits
            S = imul(WSm, W)
            RT, TR = [[x / 4096.0 for x in r] for r in S[0]], [float(t) for t in S[1]]
        else:
            ch = ram.chain(c)
            if mode == 'float':
                W = fmat(ram.mat(ch[-1] + 4))
                for x in reversed(ch[:-1]): W = fmul(W, fmat(ram.mat(x + 4)))
            else:     # 'param': local rotations from the angles, root as stored
                W = fmat(ram.mat(ch[-1] + 4))
                for x in reversed(ch[:-1]):
                    p = ram.u32(x + 0x44)
                    if (p & 0xff000000) == 0x80000000:
                        R, T = param_local(ram, x, order)
                        T = [float(t) for t in ram.mat(x + 4)[1]]
                        W = fmul(W, (R, T))
                    else:
                        W = fmul(W, fmat(ram.mat(x + 4)))
            RT, TR = fmul(fmat(WSm), W)
        vp, nv = ram.u32(part + 0x64), ram.s16(part + 0x6c)
        for i in range(max(0, nv)):
            if sel is not None and (pi, i) not in sel: continue
            v = [ram.s16(vp + 8 * i + 2 * k) for k in range(3)]
            p = project(H, RT, TR, v)
            if p: out[(pi, i)] = p
    return out


def stats(seq):
    """seq: list of {key: (x, y)} per update -> mean per-vertex step, centroid step cosines."""
    keys = set(seq[0])
    for s in seq[1:]: keys &= set(s)
    keys = sorted(keys)
    steps, cents = [], []
    for s in seq:
        cents.append((sum(s[k][0] for k in keys) / len(keys), sum(s[k][1] for k in keys) / len(keys)))
    for a, b in zip(seq, seq[1:]):
        steps.append(sum(math.hypot(b[k][0] - a[k][0], b[k][1] - a[k][1]) for k in keys) / len(keys))
    d = [(c2[0] - c1[0], c2[1] - c1[1]) for c1, c2 in zip(cents, cents[1:])]
    cos = []
    for u, v in zip(d, d[1:]):
        nu, nv = math.hypot(*u), math.hypot(*v)
        if nu > 1e-9 and nv > 1e-9: cos.append((u[0] * v[0] + u[1] * v[1]) / (nu * nv))
    cstep = [math.hypot(*x) for x in d]
    return dict(n=len(keys), step=sum(steps) / len(steps), cstep=sum(cstep) / len(cstep),
                cos=sum(cos) / len(cos) if cos else 0, cosmin=min(cos), cosmax=max(cos), cents=cents)


def smooth(seq, kind, k):
    """Per-vertex smoothing of the 'game' positions: causal EMA (alpha k) or centred box (2k+1)."""
    out = []
    keys = set(seq[0])
    for s in seq[1:]: keys &= set(s)
    if kind == 'ema':
        cur = dict((key, seq[0][key]) for key in keys)
        for s in seq:
            cur = {key: (cur[key][0] + k * (s[key][0] - cur[key][0]), cur[key][1] + k * (s[key][1] - cur[key][1])) for key in keys}
            out.append(cur)
    else:
        for i in range(len(seq)):
            lo, hi = max(0, i - k), min(len(seq), i + k + 1)
            out.append({key: (sum(seq[j][key][0] for j in range(lo, hi)) / (hi - lo),
                              sum(seq[j][key][1] for j in range(lo, hi)) / (hi - lo)) for key in keys})
    return out


def predict(seq, t):
    """Linear extrapolation t updates ahead from the last two samples (what 'predict ahead' does)."""
    keys = set(seq[0])
    for s in seq[1:]: keys &= set(s)
    out = [seq[0]]
    for a, b in zip(seq, seq[1:]):
        out.append({k: (b[k][0] + t * (b[k][0] - a[k][0]), b[k][1] + t * (b[k][1] - a[k][1])) for k in keys})
    return out


def lag(ref, test):
    """Lag (updates) of test's centroid behind ref's: argmin of the mean distance over shifts."""
    best = None
    for s in range(0, 6):
        e = [math.hypot(test[i][0] - ref[i - s][0], test[i][1] - ref[i - s][1]) for i in range(s, len(ref))]
        m = sum(e) / len(e)
        if best is None or m < best[1]: best = (s, m)
    return best


def cmd_jitter(a):
    files = sorted(glob.glob(os.path.join(a.dir, 'ram_*.bin')))
    f = FIGHTERS[a.fighter]
    x0, y0, x1, y1 = [int(v) for v in a.box.split(',')]
    # one sample per game update: keep a snapshot when the fighter's workm changed
    upd, last = [], None
    for p in files:
        ram = Ram(p)
        key = tuple(ram.u32(ram.u32(part) + 0x24 + 4 * k) for part in ram.parts(f)[:4] for k in range(8))
        if key != last: upd.append(p); last = key
    upd = upd[1:]                                                  # first one may be mid-update
    first = Ram(upd[0])
    H0 = first.u16(H_ADDR)
    pos0 = head_positions(first, f, H0, 'game', None, None)
    sel = {k for k, (x, y) in pos0.items() if x0 <= x + 256 <= x1 and y0 <= y + 120 <= y1}
    order = a.order
    res = {}
    for mode in ('game', 'float'):
        seq = []
        for p in upd:
            ram = Ram(p)
            seq.append(head_positions(ram, f, ram.u16(H_ADDR), mode, order, sel))
        res[mode] = seq
    Hs = [Ram(p).u16(H_ADDR) for p in upd]
    print(f'jitter: {len(upd)} updates ({os.path.basename(upd[0])} .. {os.path.basename(upd[-1])}), '
          f'{len(sel)} head vertices in box {a.box}, H {min(Hs)}..{max(Hs)}, param rotation order {order}')
    print(f'{"source":28s} {"vtx step px":>11s} {"centroid step":>13s} {"cos mean":>8s} {"cos min..max":>14s} {"lag":>4s}')
    ref = stats(res['game'])
    for name, seq in [('1 game (workm, integer)', res['game']), ('2 float chain (locals)', res['float'])] + \
                     [(f'3 smoothed EMA a={k}', smooth(res['game'], 'ema', k)) for k in (0.5, 0.3)] + \
                     [(f'3 smoothed centred box {2*k+1}', smooth(res['game'], 'box', k)) for k in (1, 2)] + \
                     [(f'4 predicted {t} update ahead', predict(res['game'], t)) for t in (0.5, 1.0)] + \
                     [('4 EMA 0.3 + predict 1', predict(smooth(res['game'], 'ema', 0.3), 1.0))]:
        s = stats(seq)
        lg = lag(ref['cents'], s['cents'])
        print(f'{name:28s} {s["step"]:11.3f} {s["cstep"]:13.3f} {s["cos"]:8.2f} {s["cosmin"]:6.2f}..{s["cosmax"]:5.2f} {lg[0]:4d}')
    # how far the float chain is from the game per update
    dif = []
    for g, fl in zip(res['game'], res['float']):
        ks = set(g) & set(fl)
        dif.append(sum(math.hypot(g[k][0] - fl[k][0], g[k][1] - fl[k][1]) for k in ks) / len(ks))
    print(f'game vs float chain: mean per-vertex distance {sum(dif)/len(dif):.3f} px (max per update {max(dif):.3f})')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    c = sp.add_parser('coords'); c.add_argument('snaps', nargs='+')
    j = sp.add_parser('jitter'); j.add_argument('dir'); j.add_argument('--box', default='205,112,250,152')
    j.add_argument('--fighter', type=int, default=0); j.add_argument('--order', default='zyx')
    a = ap.parse_args()
    return cmd_coords(a) if a.cmd == 'coords' else cmd_jitter(a)


if __name__ == '__main__':
    sys.exit(main())
