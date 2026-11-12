#!/usr/bin/env python3
"""The in-window menu bar (runtime/menu_overlay.c; Linux's menus, --overlay-menu elsewhere), headless.

Every check drives the overlay through its test hook (--menu-action F:menu-test=CMD;CMD...), which
works headless on every platform; at most two game processes run at once.

1. dump: every item is listed with the same greying / check marks as the native menus.
2. state: a run that opens menus and submenus, moves around them with keys, the mouse and a
   controller, opens and cancels the Host / Join forms and the Controls form, a right-click menu,
   the netplay panel, takes screenshots and chooses presentation-only items (Smooth Colours,
   Resolution, Texture Filtering, Widescreen) has the same per-frame --state-hash as a plain run.
3. block: a key pressed while a menu is open never reaches a pad; the same key pressed without the
   menu, or after it closed, does (--input-log).
4. actions: clicks queue the item's action, greyed items refuse, the Join form sends what was typed
   (netplay-join ADDRESS:PORT,DELAY), Escape closes.
5. shots: --menu-action shot=PATH writes the picture with the overlay (PNG of the --window size).
usage: tools/test_overlay_menu.py [--keep]
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("DESTREGA_BIN", os.path.join(ROOT, "build/cmake/destrega"))
OUT = os.path.join(ROOT, "build/overlay_menu")
CROSS = 0x4000
FAILS = []


def fail(msg):
    print("overlay-menu: FAIL: " + msg)
    FAILS.append(msg)


def run(name, args):
    log = os.path.join(OUT, name + ".log")
    with open(log, "w") as f:
        rc = subprocess.call(["nice", "-n", "10", BIN, "--headless", "--no-interp"] + args,
                             stdout=f, stderr=subprocess.STDOUT, cwd=OUT)
    text = open(log).read()
    if rc != 0 or "FATAL" in text or "dispatch: no recompiled" in text:
        fail("%s: exit %d (see %s)" % (name, rc, log))
    return text


def menu(frame, cmds):
    return ["--menu-action", "%d:menu-test=%s" % (frame, ";".join(cmds))]


def check_dump():
    t = run("dump", ["--frames", "40"] + menu(20, ["dump"]))
    items = [l.split("menu: item ", 1)[1] for l in t.splitlines() if "menu: item " in l]
    want = ["File/Load Game Image...", "File/Show Game Files", "File/Save Games/Show Save Games",
            "File/Save Games/Import Save Games...", "File/Save Games/Export Save Games...",
            "File/Save Games/Delete Save Game...", "File/Quit", "View/Widescreen (16:9)",
            "View/Renderer/Software [checked]", "View/Renderer/Exact [checked]", "View/Renderer/Fast (Approximate)",
            "View/Resolution/Auto [checked]", "View/Resolution/4x6 (Sharpest)", "View/Resolution/2x3 (Balanced)",
            "View/Resolution/1x1 (Original)", "View/Texture Filtering/Off (Sharp Pixels) [checked]",
            "View/Texture Filtering/Bilinear", "View/Texture Filtering/Smooth 2D Edges + Bilinear 3D",
            "View/Smooth Colours", "View/Anti-aliasing/Off [checked]", "View/Anti-aliasing/4x",
            "View/Enter Full Screen", "View/Controls...", "Netplay/Host Game...", "Netplay/Join Game...",
            "Netplay/Disconnect [greyed]", "Netplay/Not connected [greyed]",
            "Netplay/Copy Out-of-Sync Report Location [greyed]"]
    for w in want:
        if w not in items:
            fail("dump: missing '%s'" % w)
    if not any(i.startswith("Netplay/Copy My") and i.endswith("[greyed]") for i in items):
        fail("dump: no greyed Copy My Address item while not hosting")
    if any("emory card" in i for i in items):
        fail("dump: an item says memory card")
    print("overlay-menu: dump: %d items" % len(items))


def state_runs():
    frames = 2400
    shots = os.path.join(OUT, "shots")
    os.makedirs(shots, exist_ok=True)
    steps = []
    steps += menu(300, ["open=View", "key=Down", "key=Down", "key=Right", "state", "shot=%s/view.png" % shots, "key=Escape", "key=Escape"])
    steps += menu(400, ["click=View/Smooth Colours", "click=View/Texture Filtering/Bilinear"])
    steps += menu(500, ["click=View/Resolution/2x3 (Balanced)"])
    steps += menu(600, ["open=Netplay", "key=Return", "type=7123", "key=Tab", "key=Right", "shot=%s/host.png" % shots, "key=Escape"])
    steps += menu(700, ["open=Netplay", "key=Down", "key=Return", "type=192.168.1.20", "shot=%s/join.png" % shots, "pad=east"])
    steps += menu(800, ["controls", "key=Down", "key=Down", "key=Right", "shot=%s/controls.png" % shots, "key=Escape"])
    steps += menu(900, ["rclick=500,300", "mouse=520,312", "shot=%s/context.png" % shots, "mclick=5,700"])
    steps += menu(1000, ["pad=guide", "pad=dpright", "pad=dpdown", "pad=dpdown", "state", "pad=east", "pad=east", "state"])
    steps += menu(1100, ["panel=Waiting for an opponent on port 7000", "shot=%s/panel.png" % shots, "panel="])
    steps += menu(1200, ["key=F10", "key=Left", "state", "key=F10", "bar", "shot=%s/bar.png" % shots, "close"])
    steps += menu(1300, ["click=View/Widescreen (16:9)"])
    steps += menu(1600, ["open=File/Save Games", "shot=%s/file.png" % shots, "close"])
    common = ["--frames", str(frames), "--window", "1280x720", "--press", "1200:8,1500:8,1800:8,2100:8"]
    with ThreadPoolExecutor(2) as ex:
        a = ex.submit(run, "plain", common + ["--state-hash", "plain.hash"])
        b = ex.submit(run, "overlay", common + ["--state-hash", "overlay.hash"] + steps)
        a.result()
        t = b.result()
    ha = open(os.path.join(OUT, "plain.hash")).read()
    hb = open(os.path.join(OUT, "overlay.hash")).read()
    n = len(ha.splitlines())
    if n < frames - 10:
        fail("state: only %d frames hashed" % n)
    if ha != hb:
        la, lb = ha.splitlines(), hb.splitlines()
        first = next((x for x, y in zip(la, lb) if x != y), "length")
        fail("state: per-frame state hash differs (first: %s)" % first)
    else:
        print("overlay-menu: state: %d frames, per-frame state hash identical to a plain run" % n)
    for a in ("menu: action smooth-colours", "menu: action tex-filter bilinear", "menu: action resolution 2x3",
              "menu: action widescreen", "menu: Host Game form cancelled", "menu: Join Game form cancelled",
              "menu: Controls open", "menu View/Resolution, selected Auto"):
        if a not in t:
            fail("state: log lacks '%s'" % a)
    for s in ("view", "host", "join", "controls", "context", "panel", "bar", "file"):
        p = os.path.join(shots, s + ".png")
        if not os.path.exists(p) or os.path.getsize(p) < 1000:
            fail("shots: %s.png missing" % s)
        else:
            with open(p, "rb") as f:
                h = f.read(24)
            w, ht = int.from_bytes(h[16:20], "big"), int.from_bytes(h[20:24], "big")
            if (w, ht) != (1280, 720):
                fail("shots: %s.png is %dx%d" % (s, w, ht))
    print("overlay-menu: shots in %s" % shots)


def pressed(log, lo, hi):
    """frames in [lo, hi) where pad 1 had Cross, from an --input-log (changes only)"""
    rows = []
    for l in open(log).read().split("\n"):
        p = l.split()
        if len(p) >= 2:
            rows.append((int(p[0]), int(p[1], 16)))
    out, cur, i = set(), 0, 0
    for f in range(0, hi):
        while i < len(rows) and rows[i][0] <= f:
            cur = rows[i][1]
            i += 1
        if f >= lo and cur & CROSS:
            out.add(f)
    return out


def block_runs():
    common = ["--frames", "500", "--input-event", "1:key-up:Z"]
    later = menu(300, ["open=View", "close", "key-down=Z"]) + menu(360, ["key-up=Z"])
    a_args = common + ["--input-log", "a.inlog"] + menu(200, ["open=View", "key=Down", "key-down=Z", "state", "close"]) + menu(260, ["key-up=Z"]) + later
    b_args = common + ["--input-log", "b.inlog"] + menu(200, ["key-down=Z"]) + menu(260, ["key-up=Z"]) + later
    with ThreadPoolExecutor(2) as ex:
        ta = ex.submit(run, "block_menu", a_args)
        tb = ex.submit(run, "block_plain", b_args)
        ta.result()
        tb.result()
    a1, b1 = pressed(os.path.join(OUT, "a.inlog"), 195, 290), pressed(os.path.join(OUT, "b.inlog"), 195, 290)
    a2, b2 = pressed(os.path.join(OUT, "a.inlog"), 295, 400), pressed(os.path.join(OUT, "b.inlog"), 295, 400)
    if not b1:
        fail("block: without the menu, Z never reached pad 1 (test hook broken?)")
    if a1:
        fail("block: Z pressed while the menu was open reached pad 1 on %d frames" % len(a1))
    if not a2 or a2 != b2:
        fail("block: Z pressed after the menu closed: %d frames vs %d without the menu" % (len(a2), len(b2)))
    if not FAILS:
        print("overlay-menu: block: Z held %d frames without the menu, 0 with it open, %d after it closed" % (len(b1), len(a2)))


def action_run():
    t = run("actions", ["--frames", "60"] +
            menu(20, ["click=Netplay/Disconnect", "click=View/Nothing", "open=View", "key=Escape", "state"]) +
            menu(30, ["click=View/Anti-aliasing/2x", "click=View/Renderer/Fast (Approximate)"]) +
            menu(40, ["open=Netplay", "key=Down", "key=Return", "type=127.0.0.1:7999", "key=Down", "key=Right",
                      "key=Return"]))
    for w in ("menu: test: Netplay/Disconnect is greyed out", "menu: test: no item View/Nothing",
              "menu: overlay state: closed", "menu: action ssaa 2", "menu: action gpu-precision fast",
              "menu: Join Game form accepted", "menu: action netplay-join 127.0.0.1:7999,3"):
        if w not in t:
            fail("actions: log lacks '%s'" % w)
    if "menu: overlay still open" in t:
        fail("actions: a menu stayed open")


def main():
    if not os.path.exists(BIN):
        print("overlay-menu: no binary at %s" % BIN)
        return 1
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    check_dump()
    action_run()
    block_runs()
    state_runs()
    if FAILS:
        print("overlay-menu: %d FAILED" % len(FAILS))
        return 1
    print("overlay-menu: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
