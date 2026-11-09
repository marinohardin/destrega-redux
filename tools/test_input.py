#!/usr/bin/env python3
"""Input layer + settings file, end to end, headless (also run by tools/run_tests.sh).

1. equivalence: boot -> 1P battle -> fight played twice:
     A: the --press / --press2 script (pad masks directly);
     B: a settings file with custom bindings (keys and controller inputs moved around, one binding
        added mid-run by --menu-action controls-bind) and the same presses as synthetic host events
        (--input-event): keys through SDL's event queue, buttons / sticks / triggers on SDL virtual
        controllers (a PS4-type one for player 1, an Xbox-type one for player 2), plus inputs that
        must NOT reach a pad (a stick tilt inside the deadzone, keys whose default binding was moved,
        Tab, an unbound key).
   Both runs must produce the same pad masks every frame (--input-log) and the same complete
   emulated-state hash every frame (--state-hash). The settings file must afterwards hold the binding
   added by the menu action and still hold everything else.
2. hot-plug: unplugging player 1's controller leaves player 2's where it is.
3. settings persistence: View actions are saved, a key given on the command line is not.
4. a malformed / binary settings file is ignored safely and not rewritten.
5. headless without --settings reads and writes no settings file.
usage: tools/test_input.py [--frames N]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build/cmake/destrega")
OUT = os.path.join(ROOT, "build/input")
FRAMES = 6000
if "--frames" in sys.argv:
    FRAMES = int(sys.argv[sys.argv.index("--frames") + 1])

UP, DOWN, LEFT, RIGHT = 0x10, 0x40, 0x80, 0x20
CROSS, CIRCLE, SQUARE, TRIANGLE = 0x4000, 0x2000, 0x8000, 0x1000
L1, R1, L2, R2, START, SELECT = 0x400, 0x800, 0x100, 0x200, 0x8, 0x1

SETTINGS = """# test bindings (tools/test_input.py)
[display]
scale = off

[player1]
device = auto
stick = on
deadzone = 40
key.start = P
key.cross = Comma
pad.cross = east
pad.circle = none
key.circle = Semicolon
pad.square = north
pad.triangle = lefttrigger
key.up = W
key.r1 = none

[player2]
device = auto
pad.circle = south
"""


def fail(msg):
    print("input: FAIL: " + msg)
    sys.exit(1)


def run(name, args, check=True):
    log = os.path.join(OUT, name + ".log")
    with open(log, "w") as f:
        rc = subprocess.call([BIN, "--headless", "--no-interp"] + args, stdout=f, stderr=subprocess.STDOUT, cwd=OUT)
    text = open(log).read()
    if check and (rc != 0 or "FATAL" in text or "dispatch: no recompiled" in text):
        fail("%s: exit %d (see %s)" % (name, rc, log))
    return rc, text


def script():
    """(frame, mask, pad) presses: fight_script.py's boot and fight, plus some pad 2 presses."""
    steps = [(1200, START), (1500, START), (1800, START), (2100, START)]
    steps += [(f, CROSS) for f in (2400, 2700, 3000, 3300, 3600)]
    cycle = [CROSS, SQUARE | RIGHT, CIRCLE, TRIANGLE | LEFT, CROSS | UP, SQUARE | DOWN, CIRCLE | RIGHT,
             RIGHT, CROSS | LEFT, TRIANGLE, L1, R1 | CROSS, RIGHT | DOWN, UP | LEFT]
    out = [(f, m, 0) for f, m in steps]
    k = 0
    for f in range(3800, FRAMES - 20, 24):
        out.append((f, cycle[k % len(cycle)], 0))
        if k % 9 == 4:
            out.append((f + 12, CIRCLE, 1))   # player 2 (Xbox-type controller, circle = south)
        k += 1
    return out


def events(presses):
    """The same presses as host events through the custom bindings."""
    ev = ["50:pad-add:0:ps4", "60:pad-add:1:xbox"]
    for i, (f, m, pad) in enumerate(presses):
        up = f + 6
        def both(dn, upe):
            ev.append("%d:%s" % (f, dn)); ev.append("%d:%s" % (up, upe))
        if pad == 1:
            both("pad-down:1:south", "pad-up:1:south")
            continue
        if m & START: both("key-down:P", "key-up:P")
        if m & CROSS:
            if i % 2: both("pad-down:0:east", "pad-up:0:east")
            else: both("key-down:Comma", "key-up:Comma")
        if m & CIRCLE: both("key-down:Semicolon", "key-up:Semicolon")
        if m & SQUARE: both("pad-down:0:north", "pad-up:0:north")
        if m & TRIANGLE: both("pad-axis:0:lefttrigger:32767", "pad-axis:0:lefttrigger:0")
        if m & UP: both("key-down:W", "key-up:W")
        if m & LEFT: both("pad-down:0:dpleft", "pad-up:0:dpleft")
        if m & RIGHT: both("pad-axis:0:leftx:32767", "pad-axis:0:leftx:0")
        if m & DOWN: both("pad-axis:0:lefty:30000", "pad-axis:0:lefty:0")
        if m & L1: both("key-down:Left Shift", "key-up:Left Shift")   # bound by --menu-action at frame 100
        if m & R1: both("pad-down:0:rightshoulder", "pad-up:0:rightshoulder")
        # noise between presses: none of it may reach a pad
        if f >= 3800 and i % 5 == 0:
            n = f + 12
            ev += ["%d:pad-axis:0:lefty:-12000" % n, "%d:pad-axis:0:lefty:0" % (n + 4)]   # inside the 40 % deadzone
            ev += ["%d:key-down:Z" % n, "%d:key-up:Z" % (n + 3)]           # Z: cross moved to Comma
            ev += ["%d:key-down:Return" % n, "%d:key-up:Return" % (n + 3)] # Return: start moved to P
            ev += ["%d:key-down:Tab" % n, "%d:key-up:Tab" % (n + 3)]       # reserved (fast forward)
            ev += ["%d:key-down:F1" % n, "%d:key-up:F1" % (n + 3)]         # unbound
            ev += ["%d:pad-down:0:south" % n, "%d:pad-up:0:south" % (n + 3)]   # south: cross moved to east
    return ev


def main():
    if not os.path.exists(BIN):
        fail("no %s" % BIN)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    presses = script()
    p1 = ",".join("%d:%x" % (f, m) for f, m, pad in presses if pad == 0)
    p2 = ",".join("%d:%x" % (f, m) for f, m, pad in presses if pad == 1)
    sfile = os.path.join(OUT, "custom.ini")
    with open(sfile, "w") as f:
        f.write(SETTINGS)
    ev = events(presses)
    common = ["--frames", str(FRAMES)]
    procs = []
    for name, args in (("press", common + ["--press", p1, "--press2", p2, "--state-hash", "press.hash", "--input-log", "press.masks"]),
                       ("host", common + ["--settings", sfile, "--input-event", ",".join(ev),
                                          "--menu-action", "100:controls-bind=P1:l1=key:Left Shift",
                                          "--menu-action", "110:controls-dump",
                                          "--state-hash", "host.hash", "--input-log", "host.masks"])):
        log = open(os.path.join(OUT, name + ".log"), "w")
        procs.append((name, subprocess.Popen([BIN, "--headless", "--no-interp"] + args, stdout=log, stderr=subprocess.STDOUT, cwd=OUT), log))
        if int(os.environ.get("JOBS", "2")) < 2:   # JOBS=1: one game at a time
            procs[-1][1].wait()
    for name, pr, log in procs:
        rc = pr.wait()
        log.close()
        text = open(os.path.join(OUT, name + ".log")).read()
        if rc != 0 or "FATAL" in text or "dispatch: no recompiled" in text or "reached %d frames" % FRAMES not in text:
            fail("%s run: exit %d (see build/input/%s.log)" % (name, rc, name))
    host_log = open(os.path.join(OUT, "host.log")).read()
    for need in ("controller connected: Virtual PS4 Controller", "player 2: Virtual Xbox Controller",
                 "controls-bind P1 l1=key:Left Shift: done"):
        if need not in host_log:
            fail("host run log lacks %r" % need)
    ma = open(os.path.join(OUT, "press.masks")).read().splitlines()
    mb = open(os.path.join(OUT, "host.masks")).read().splitlines()
    if ma != mb:
        for i, (a, b) in enumerate(zip(ma, mb)):
            if a != b:
                fail("pad masks differ: --press %r vs host events %r (line %d of build/input/*.masks)" % (a, b, i + 1))
        fail("pad masks differ in length: %d vs %d lines" % (len(ma), len(mb)))
    presses_seen = sum(1 for l in ma if l.split()[1] != "0000" or l.split()[2] != "0000")
    ha = open(os.path.join(OUT, "press.hash")).read().splitlines()
    hb = open(os.path.join(OUT, "host.hash")).read().splitlines()
    if len(ha) < FRAMES - 10 or ha != hb:
        bad = next((i for i, (a, b) in enumerate(zip(ha, hb)) if a != b), min(len(ha), len(hb)))
        fail("state hashes differ from frame line %d (%d vs %d lines)" % (bad + 1, len(ha), len(hb)))
    saved = open(sfile).read()
    if "key.l1 = Left Shift" not in saved or "key.cross = Comma" not in saved or "pad.square = north" not in saved \
            or "scale = off" not in saved:
        fail("settings file after the run lost bindings:\n" + saved)
    print("input: equivalence OK: %d frames, %d mask changes (%d with buttons held), %d host events; "
          "masks and state hashes identical to the --press run" % (FRAMES, len(ma), presses_seen, len(ev)))

    # 2. hot-plug: player 2 keeps its controller when player 1's is unplugged
    run("hotplug", ["--frames", "60", "--input-event",
                    "10:pad-add:0,20:pad-add:1,30:pad-remove:0,40:pad-down:1:south,46:pad-up:1:south,50:pad-add:0,52:pad-down:0:west",
                    "--input-log", "hotplug.masks"])
    hm = open(os.path.join(OUT, "hotplug.masks")).read().split()
    want = ["1", "0000", "0000", "40", "0000", "4000", "46", "0000", "0000", "52", "8000", "0000"]
    if hm != want:
        fail("hot-plug masks %s, want %s" % (hm, want))
    print("input: hot-plug OK (player 2 kept its controller; a re-plugged one went back to player 1)")

    # 3. persistence: View actions saved, command-line keys not written
    s2 = os.path.join(OUT, "persist.ini")
    with open(s2, "w") as f:
        f.write("[display]\nscale = 3\nwidescreen = off\n")
    run("persist", ["--frames", "40", "--settings", s2, "--scale", "2", "--menu-action", "10:widescreen=on",
                    "--menu-action", "12:renderer=software", "--menu-action", "14:controls-deadzone=P2:30",
                    "--menu-action", "16:controls-device=P2:keyboard", "--menu-action", "18:controls-bind=P1:cross=key:F9"])
    t = open(s2).read()
    for need in ("scale = 3", "widescreen = on", "renderer = software", "deadzone = 30", "device = keyboard"):
        if need not in t:
            fail("persist: settings file lacks %r:\n%s" % (need, t))
    if "key.cross = F9" in t:
        fail("persist: a reserved key was bound")
    print("input: settings persistence OK (View actions + controls saved, --scale not written, F9 refused)")

    # 4. malformed / binary file: ignored, game runs, file untouched
    s3 = os.path.join(OUT, "garbage.ini")
    junk = bytes((i * 7919 + 13) & 0xff for i in range(4000)) + b"\n[player1]\nkey.cross = F9\ndeadzone = 9000\n"
    with open(s3, "wb") as f:
        f.write(junk)
    _, text = run("garbage", ["--frames", "1300", "--settings", s3, "--press", "1200:8",
                              "--menu-action", "1250:controls-dump"])
    if open(s3, "rb").read() != junk:
        fail("garbage: the settings file was rewritten although nothing changed")
    if "deadzone=50" not in text or "cross=Z/south" not in text:
        fail("garbage: bad values did not fall back to the defaults")
    print("input: malformed settings file OK (ignored lines logged, defaults kept, file untouched)")

    # 5. headless without --settings: no settings file at all
    _, text = run("hermetic", ["--frames", "20", "--menu-action", "10:widescreen=on"])
    if "settings:" in text:
        fail("hermetic: a headless run without --settings used a settings file:\n" + text)
    print("input: headless without --settings uses no settings file")
    print("input: PASS")


if __name__ == "__main__":
    main()
