#!/usr/bin/env python3
"""Version strings for packaging, derived from VERSION (base, e.g. 0.1.0) and git.

  version.py short       0.1.0                      CFBundleShortVersionString, file version
  version.py build       <commit count>             CFBundleVersion (monotonic integer)
  version.py describe    0.1.0+<count>.g<sha>[.dirty]   human-readable, also for file names
  version.py rc-header OUT.h    #defines for the Windows version resource; rewritten only when
                                its content changes (so Ninja does not rebuild for nothing)

Without git (a source tarball) the commit parts are left out: describe = base version.
If the exact commit is tagged vX.Y.Z, that tag wins over VERSION.
"""
import os
import subprocess
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def git(*a):
    try:
        r = subprocess.run(["git", "-C", root, *a], capture_output=True, text=True, timeout=20)
        return r.stdout.strip() if r.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def info():
    base = open(os.path.join(root, "VERSION")).read().strip()
    tag = git("describe", "--tags", "--exact-match", "--match", "v[0-9]*")
    if tag:
        base = tag[1:]
    count = git("rev-list", "--count", "HEAD") or "0"
    sha = git("rev-parse", "--short=8", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    desc = base
    if sha and not tag:
        desc = f"{base}+{count}.g{sha}"
    if dirty:
        desc += ".dirty"
    return base, count, desc


def main():
    base, count, desc = info()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "describe"
    if cmd == "short":
        print(base)
    elif cmd == "build":
        print(count)
    elif cmd == "describe":
        print(desc)
    elif cmd == "rc-header":
        parts = (base.split("-")[0].split(".") + ["0", "0", "0"])[:3]
        nums = [int(p) if p.isdigit() else 0 for p in parts] + [int(count) % 65536]
        text = (f"#define DESTREGA_VER_NUMS {nums[0]},{nums[1]},{nums[2]},{nums[3]}\n"
                f'#define DESTREGA_VER_STR "{desc}"\n')
        out = sys.argv[2]
        try:
            if open(out).read() == text:
                return
        except OSError:
            pass
        with open(out, "w") as f:
            f.write(text)
    else:
        sys.exit(__doc__)


main()
