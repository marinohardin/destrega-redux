#!/usr/bin/env python3
"""Fail if a launcher package (a directory, .app, .zip or AppImage directory) holds game code.

    tools/check_no_game_code.py PATH [PATH...] [--disc DIR]

A launcher package must contain the runtime and the build tools only: the game module is built
on the player's machine from the player's own disc. Checked:

  1. no file that belongs to the generated code: destrega_game.{dylib,dll,so}, gen/, *.o / *.obj,
     recomp_*.c, gen_protos.h, recomp_table.c, game.key;
  2. no recompiled-function symbol or string in any file: names f_8XXXXXXX(_orig|_OVL) other than
     the few that make up the module interface (runtime/game_abi.h; a static build or a module
     has ~2000 of them);
  3. with --disc DIR (or ./disc/SLUS_007.32 when present): none of 64 windows of 64 bytes taken
     from the game executable's code appears in any file (catches raw game code or data).

Exit 0 when clean, 1 with a list of findings.
"""
import os
import re
import sys
import zipfile

# The names of the module interface (runtime/game_abi.h: the overrides the executable defines and
# the generated functions it imports by name); they appear in the executable and in the embedded
# module SDK as text. Anything beyond them is a recompiled function.
def interface_names(root):
    with open(os.path.join(root, "runtime", "game_abi.h"), "rb") as fh:
        return set(SYM.findall(fh.read()))


SYM = re.compile(rb"f_8[0-9a-f]{7}(?:_orig|_[A-Z0-9]{2,12})?")
BAD_NAME = re.compile(r"(^|/)(destrega_game\.(dylib|dll|so)|gen_protos\.h|recomp_table\.c|"
                      r"recomp_[0-9a-zA-Z_]+\.c|game\.key)$|(^|/)gen/|\.(o|obj)$")


def files_of(path):
    """yield (name, bytes) for every regular file under path (directory or zip)"""
    if os.path.isdir(path):
        for dp, dns, fns in os.walk(path):
            for f in sorted(fns):
                p = os.path.join(dp, f)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                with open(p, "rb") as fh:
                    yield os.path.relpath(p, path).replace(os.sep, "/"), fh.read()
    elif zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if not info.is_dir():
                    yield info.filename, z.read(info)
    else:
        with open(path, "rb") as fh:
            yield os.path.basename(path), fh.read()


def code_windows(disc):
    exe = os.path.join(disc, "SLUS_007.32")
    if not os.path.isfile(exe):
        return []
    with open(exe, "rb") as fh:
        data = fh.read()
    # code starts after the 2 KiB PS-X EXE header plus the data at the start of .text
    lo, hi = 0x800 + 0x2200, min(len(data), 0x800 + 0x85000)
    step = (hi - lo - 64) // 64
    out = []
    for i in range(64):
        w = data[lo + i * step: lo + i * step + 64]
        if len(set(w)) > 16:  # skip runs of zeros / padding
            out.append(w)
    return out


def main(argv):
    disc = None
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--disc":
            disc = argv[i + 1]
            i += 2
            continue
        paths.append(argv[i])
        i += 1
    if not paths:
        print(__doc__.strip().splitlines()[2])
        return 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if disc is None and os.path.isfile(os.path.join(root, "disc", "SLUS_007.32")):
        disc = os.path.join(root, "disc")
    windows = code_windows(disc) if disc else []
    allowed = interface_names(root)
    findings = []
    nfiles = nbytes = 0
    for p in paths:
        for name, data in files_of(p):
            nfiles += 1
            nbytes += len(data)
            if BAD_NAME.search(name):
                findings.append(f"{p}: {name}: generated-code file")
            syms = set(SYM.findall(data)) - allowed
            if syms:
                ex = b", ".join(sorted(syms)[:5]).decode()
                findings.append(f"{p}: {name}: {len(syms)} recompiled-function names ({ex} ...)")
            for w in windows:
                if w in data:
                    findings.append(f"{p}: {name}: contains bytes of the game executable")
                    break
    what = f"{nfiles} files, {nbytes / 1e6:.1f} MB"
    what += f", {len(windows)} code windows from {disc}" if windows else ", no disc to compare with"
    if findings:
        print("check_no_game_code: FAILED (" + what + ")")
        for f in findings[:50]:
            print("  " + f)
        return 1
    print("check_no_game_code: clean (" + what + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
