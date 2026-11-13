#!/usr/bin/env python3
"""Detect a stuck audio loop (the same fraction of a second replayed over and over) in a --wav file.

  xa_loop_check.py OUT.wav [--from SEC] [--to SEC] [--min-run SEC] [-v]

The BGM bug this guards against (NOTES.md "CD: GetlocL and the BGM loop"): after a track's end
the game re-seeked to its start every 9 vblanks forever, so the music became a ~0.15 s fragment
repeated. Music itself is never that periodic: for each 2 s window (hop 1 s, mono, decimated to
11025 Hz) the normalised autocorrelation is taken at lags 0.06 .. 0.5 s; a window is "looped" when
its best value is >= THRESH (0.9) and the window is not silent. Exit 1 when looped windows cover
at least --min-run seconds in a row (default 6), printing where. Sound effects mixed on top lower
the correlation, which is why the threshold is not closer to 1 (measured: stuck fight and menu BGM
0.99 .. 1.0 in every window; normal fight / menu BGM at most ~0.8; one window of the boot jingle
0.91, which is why a run of windows is required; -v prints every window).
"""
import argparse, sys, wave

try:
    import numpy as np
except ImportError:
    sys.exit('xa_loop_check: needs numpy')

THRESH = 0.9


def load(path):
    with wave.open(path, 'rb') as w:
        assert w.getsampwidth() == 2, 'expects 16-bit PCM'
        rate, ch = w.getframerate(), w.getnchannels()
        a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float32)
    a = a.reshape(-1, ch).mean(axis=1)
    step = max(1, rate // 11025)
    return a[: len(a) // step * step].reshape(-1, step).mean(axis=1), rate / step


def windows(x, rate, t0, t1, win=2.0, hop=1.0):
    n, h = int(win * rate), int(hop * rate)
    lo, hi = int(0.06 * rate), int(0.5 * rate)
    t = t0
    while t + win <= t1:
        seg = x[int(t * rate): int(t * rate) + n]
        seg = seg - seg.mean()
        e = float((seg * seg).sum())
        if e / n < 25.0:       # silence (rms < 5 of 32768): nothing to judge
            yield t, None, None
        else:
            f = np.fft.rfft(seg, 2 * n)
            ac = np.fft.irfft(f * np.conj(f))[:hi + 1]
            # normalise by the overlap length so long lags are not penalised
            norm = ac[lo:hi + 1] / (n - np.arange(lo, hi + 1)) / (e / n)
            k = int(np.argmax(norm))
            yield t, float(norm[k]), (lo + k) / rate
        t += hop


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    ap.add_argument('--min-run', type=float, default=6.0)
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    x, rate = load(a.wav)
    t1 = min(a.t1, len(x) / rate)
    run, run_start, worst, best = 0.0, None, (0.0, 0.0), None
    looped = []
    for t, c, lag in windows(x, rate, a.t0, t1):
        if a.v: print(f'{t:7.1f}s ' + ('silent' if c is None else f'{c:.3f} at {lag * 1000:.0f} ms'))
        if c is not None and c > worst[0]: worst = (c, t)
        if c is not None and c >= THRESH:
            if run_start is None: run_start = t
            run = t - run_start + 1.0
            if run >= a.min_run and (best is None or run > best[1] - best[0]): best = (run_start, t + 1.0, lag)
        else:
            run, run_start = 0.0, None
    span = f'{a.t0:.0f}..{t1:.0f} s'
    if best:
        print(f'xa_loop_check: STUCK LOOP in {a.wav}: {best[0]:.0f}..{best[1]:.0f} s repeats every '
              f'{best[2] * 1000:.0f} ms (checked {span})')
        return 1
    print(f'xa_loop_check: ok, no stuck loop in {span} (highest window correlation {worst[0]:.2f} at {worst[1]:.0f} s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
