#!/usr/bin/env python3
"""Render resolution (View > Resolution) and the scale governor, headless and deterministic (also run
by tools/run_tests.sh, ~1 min).

The governor (runtime/smooth.c) normally measures this computer's frame time in a window. Headless
runs have no meaningful frame time, so the test hook --fake-load F-G:PCT[@SxxSy] gives it a load
(% of the 16.7 ms budget) per frame, optionally only at one render scale. --max-scale 2x3 keeps the
runs cheap: Auto's ceiling is then 2x3 and the steps are 2x3 -> 2x2 -> 1x2 -> 1x1.

Cases:
  auto4x6    --scale auto aims for 4x6 whatever the window size (a 640x480 window too).
  downup     load 300% during the first 170 frames (warm-up: ignored), 30% (no change), 120% (steps
             down to 1x1), 20% (steps back up to 2x3, one step at a time, after the back-off).
  oscillate  2x3 costs 130%, 2x2 30%: down, up, down, ... with the wait before each retry doubling
             (10 s, 20 s, 40 s), so at most a few round trips in 9000 frames.
  offline92  92% offline: below the conservative 95% offline line, nothing changes.
  netplay92  the same 92% on a netplay host (loopback, headless): above the 90% netplay line, it
             steps down "for the netplay session"; the session stays in sync.
  fixed      a fixed resolution (menu action, and --scale 2x3) is never changed, even at 200%.
  menu       resolution=4x6 / 2x3 / 1x1 / auto: the log and the --dump frame sizes show the scale;
             settings.ini round trip (saved, restored at startup, --scale locks it).
  Every run's per-frame --state-hash equals a plain run's (presentation only).
usage: tools/test_governor.py [-j N]
"""
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("DESTREGA_BIN", os.path.join(ROOT, "build/cmake/destrega"))
OUT = os.path.join(ROOT, "build/governor")
PAK = [os.environ["DESTREGA_PAK"]] if os.environ.get("DESTREGA_PAK") else []
JOBS = int(sys.argv[sys.argv.index("-j") + 1]) if "-j" in sys.argv else 6
GOV = ["--scale", "auto", "--max-scale", "2x3"]
STEP = re.compile(r"smooth: (?:this computer is too slow for render scale (\d+)x(\d+) .*?lowering it to (\d+)x(\d+).*?"
                  r"at frame (\d+) \(\S+ not tried again for (\d+) s\)|render scale (\d+)x(\d+) uses .*?raising it to "
                  r"(\d+)x(\d+) at frame (\d+))")
failures = []


def fail(msg):
    failures.append(msg)
    print("governor: FAIL: " + msg)


def run(name, args, frames):
    log = os.path.join(OUT, name + ".log")
    h = os.path.join(OUT, name + ".hash")
    with open(log, "w") as f:
        rc = subprocess.call([BIN, "--headless", "--no-interp", "--no-memcard", "--frames", str(frames),
                              "--state-hash", h] + args + PAK, stdout=f, stderr=subprocess.STDOUT, cwd=OUT)
    text = open(log).read()
    if rc != 0 or "FATAL" in text or "dispatch: no recompiled" in text:
        fail("%s: exit %d (see %s)" % (name, rc, log))
    return text, h


def steps(text):
    """[(kind, from, to, frame, wait_s)] in order"""
    out = []
    for m in STEP.finditer(text):
        if m.group(1):
            out.append(("down", (int(m.group(1)), int(m.group(2))), (int(m.group(3)), int(m.group(4))),
                        int(m.group(5)), int(m.group(6))))
        else:
            out.append(("up", (int(m.group(7)), int(m.group(8))), (int(m.group(9)), int(m.group(10))),
                        int(m.group(11)), 0))
    return out


def netplay92():
    import random
    port = random.randint(20000, 40000)
    common = ["--headless", "--no-interp", "--no-memcard", "--frames", "2400", "--net-pace", "600"] + PAK
    logs = [os.path.join(OUT, "netplay92_%s.log" % w) for w in ("host", "join")]
    cmds = [[BIN] + common + GOV + ["--fake-load", "1-2400:92", "--host", str(port)],
            [BIN] + common + ["--join", "127.0.0.1:%d" % port]]
    procs = [subprocess.Popen(c, stdout=open(l, "w"), stderr=subprocess.STDOUT, cwd=OUT) for c, l in zip(cmds, logs)]
    rcs = [p.wait(timeout=300) for p in procs]
    th, tj = (open(l).read() for l in logs)
    st = steps(th)
    if rcs != [0, 0] or "DESYNC" in th + tj:
        fail("netplay92: exit %s or desync (see %s)" % (rcs, logs[0]))
    elif not st or st[0][:3] != ("down", (2, 3), (2, 2)) or "for the netplay session" not in th or steps(tj):
        fail("netplay92: the host did not step down for the session (or the joiner did): %s" % st)
    else:
        print("governor: netplay92 OK (host at 92%%: %s; in sync)" %
              ", ".join("%dx%d@%d" % (b + (fr,)) for k, a, b, fr, _ in st))


def png_size(p):
    with open(p, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])


def same_hash(name, h, ref):
    if open(h).read() != open(ref).read():
        fail("%s: state hash differs from the plain run (the governor or resolution touched emulation)" % name)


def main():
    os.makedirs(OUT, exist_ok=True)
    F = 9000
    S = "1300:8"   # start into the title menu (fight_script.py's first press)
    runs = {
        "plain": ([], F),
        "plain_short": (["--press", S], 1700),
        "auto4x6": (["--scale", "auto", "--window", "640x480"], 120),
        "downup": (GOV + ["--fake-load", "1-170:300,171-899:30,900-1500:120,1501-9000:20"], F),
        "oscillate": (GOV + ["--fake-load", "1-9000:130@2x3,1-9000:30@2x2,1-9000:30@1x2,1-9000:30@1x1"], F),
        "offline92": (GOV + ["--fake-load", "1-9000:92"], F),
        "fixed": (GOV + ["--fake-load", "1-9000:200", "--menu-action", "100:resolution=2x3"], F),
        "fixed_flag": (["--scale", "2x3", "--fake-load", "1-9000:200"], F),
        "menu": (["--press", S, "--settings", os.path.join(OUT, "menu.ini"), "--dump", os.path.join(OUT, "menu_dump"),
                  "--dump-every", "100", "--dump-from", "1400",
                  "--menu-action", "1350:resolution=4x6", "--menu-action", "1450:resolution=2x3",
                  "--menu-action", "1550:resolution=1x1", "--menu-action", "1650:resolution=auto"], 1700),
    }
    for p in (os.path.join(OUT, "menu.ini"), os.path.join(OUT, "restore.ini")):
        if os.path.exists(p):
            os.remove(p)
    os.makedirs(os.path.join(OUT, "menu_dump"), exist_ok=True)
    for n in os.listdir(os.path.join(OUT, "menu_dump")):
        os.remove(os.path.join(OUT, "menu_dump", n))
    with ThreadPoolExecutor(JOBS) as ex:
        net = ex.submit(netplay92)
        res = dict(zip(runs, ex.map(lambda k: run(k, *runs[k]), runs)))
        net.result()
    ref, ref_short = res["plain"][1], res["plain_short"][1]

    t = res["auto4x6"][0]
    if "render scale 4x6" not in t:
        fail("auto4x6: --scale auto in a 640x480 window did not render at 4x6")
    else:
        print("governor: auto4x6 OK (Auto renders 4x6 in a 640x480 window)")

    t, h = res["downup"]
    same_hash("downup", h, ref)
    st = steps(t)
    want = [("down", (2, 3), (2, 2)), ("down", (2, 2), (1, 2)), ("down", (1, 2), (1, 1)),
            ("up", (1, 1), (1, 2)), ("up", (1, 2), (2, 2)), ("up", (2, 2), (2, 3))]
    got = [(k, a, b) for k, a, b, _, _ in st]
    if got != want:
        fail("downup: steps %s, want %s" % (got, want))
    elif any(fr < 900 for _, _, _, fr, _ in st):
        fail("downup: a step before frame 900 (warm-up load not ignored, or 30%% load changed it): %s" % st)
    elif not all(900 <= fr <= 1500 for k, _, _, fr, _ in st if k == "down"):
        fail("downup: steps down outside the loaded frames 900-1500: %s" % st)
    else:
        print("governor: downup OK (warm-up ignored; down %s; up %s)" %
              (", ".join("%dx%d@%d" % (b + (fr,)) for k, a, b, fr, _ in st if k == "down"),
               ", ".join("%dx%d@%d" % (b + (fr,)) for k, a, b, fr, _ in st if k == "up")))

    t, h = res["oscillate"]
    same_hash("oscillate", h, ref)
    st = steps(t)
    downs = [s for s in st if s[0] == "down"]
    waits = [w for _, _, _, _, w in downs]
    if not downs or any(s[1] != (2, 3) for s in downs) or any(s[0] == "up" and s[2] != (2, 3) for s in st):
        fail("oscillate: expected only 2x3 <-> 2x2: %s" % st)
    elif waits[:3] != [10, 20, 40] or len(st) > 8:
        fail("oscillate: back-off waits %s (want 10, 20, 40 s ...), %d changes in %d frames" % (waits, len(st), F))
    else:
        print("governor: oscillate OK (%d changes in %d frames, back-off %s s)" % (len(st), F, waits))

    t, h = res["offline92"]
    same_hash("offline92", h, ref)
    if steps(t):
        fail("offline92: 92%% offline changed the scale: %s" % steps(t))
    else:
        print("governor: offline92 OK (92% offline: no change)")

    for n in ("fixed", "fixed_flag"):
        t, h = res[n]
        same_hash(n, h, ref)
        if steps(t) or "smooth:" in t:
            fail("%s: a fixed resolution was changed: %s" % (n, steps(t)))
        else:
            print("governor: %s OK (fixed 2x3 at 200%% load: untouched)" % n)

    t, h = res["menu"]
    same_hash("menu", h, ref_short)
    for r, scale in (("4x6", "4x6"), ("2x3", "2x3"), ("1x1", "1x1"), ("auto", "4x6")):
        if "hires: resolution %s at frame" % r not in t or not re.search(r"hires: resolution %s at frame \d+ \(render scale %s\)" % (r, scale), t):
            fail("menu: resolution=%s not logged with render scale %s" % (r, scale))
    d = os.path.join(OUT, "menu_dump")
    sz = {f: png_size(os.path.join(d, "frame_%06d.png" % f)) for f in (1400, 1500, 1600, 1700)
          if os.path.exists(os.path.join(d, "frame_%06d.png" % f))}
    if len(sz) != 4:
        fail("menu: dumps missing: %s" % sorted(sz))
    else:
        (w4, h4), (w2, h2), (w1, h1), (wa, ha) = sz[1400], sz[1500], sz[1600], sz[1700]
        if not (w4 == 4 * w1 and h4 == 6 * h1 and w2 == 2 * w1 and h2 == 3 * h1 and (wa, ha) == (w4, h4)):
            fail("menu: dump sizes %s (want 4x6, 2x3, 1x1, auto = 4x6 of the 1x size)" % sz)
        else:
            print("governor: menu OK (dumps %dx%d, %dx%d, %dx%d, auto %dx%d)" % (w4, h4, w2, h2, w1, h1, wa, ha))
    ini = open(os.path.join(OUT, "menu.ini")).read() if os.path.exists(os.path.join(OUT, "menu.ini")) else ""
    if not re.search(r"^resolution = auto$", ini, re.M):
        fail("menu: settings file lacks resolution = auto:\n" + ini)
    # restore: a saved fixed choice comes back at startup; --scale overrides it and is not written
    rp = os.path.join(OUT, "restore.ini")
    with open(rp, "w") as f:
        f.write("[display]\nresolution = 2x3\n")
    t, _ = run("restore", ["--settings", rp], 30)
    if "render scale 2x3" not in t:
        fail("restore: resolution = 2x3 from the settings file not applied")
    t, _ = run("restore_flag", ["--settings", rp, "--scale", "1", "--menu-action", "10:resolution=4x6"], 30)
    if "render scale 1x1" not in t or "resolution = 2x3" not in open(rp).read():
        fail("restore_flag: --scale did not win or the settings file was rewritten:\n" + open(rp).read())
    else:
        print("governor: settings OK (resolution saved, restored, --scale locks it)")

    if failures:
        print("governor: %d FAILED" % len(failures))
        sys.exit(1)
    print("governor: all OK")


if __name__ == "__main__":
    main()
