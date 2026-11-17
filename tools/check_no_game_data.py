#!/usr/bin/env python3
"""Fail if a release package (a directory, .app, .zip, .tar.gz or single file) holds game data.

    tools/check_no_game_data.py PATH [PATH...] [--disc DIR]

A release holds the program: the runtime and the code translated from the game's executable.
It must never hold the game's data: disc images, the data pack, the files on the disc (stages,
characters, textures, sound, music, films, overlays), memory cards, or anything taken from them.
Checked:

  1. no file whose name is a game-data file: *.pak, *.bin, *.cue, *.iso, *.img, *.mcd, *.gme,
     *.BNS, *.XA, *.STR, SLUS_*, SYSTEM.CNF, anything under a disc/ folder, and every name in
     the disc listing (disc/_listing.txt);
  2. with the extracted disc (--disc DIR, default ./disc when present; required unless
     --no-disc is given): none of the byte windows sampled from every file on the disc appears
     in any packaged file: 64 windows of 64 bytes from each file (256 from the large ones),
     skipping windows of padding. This catches raw game data and the game executable's own
     bytes (the program holds only code translated to C, compiled for the host CPU).

Exit 0 when clean, 1 with a list of findings, 2 on bad usage.
"""
import io
import os
import re
import sys
import tarfile
import zipfile

BAD_NAME = re.compile(
    r"(^|/)disc/|\.(pak|bin|cue|iso|img|mcd|gme|bns|xa|str|raw)$|(^|/)(slus_[0-9.]+|system\.cnf)$",
    re.IGNORECASE)


def files_of(path):
    """yield (name, bytes) for every regular file under path (directory, zip, tar or file)"""
    if os.path.isdir(path):
        for dp, dns, fns in os.walk(path):
            dns.sort()
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
    elif tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            for m in t.getmembers():
                if m.isfile():
                    yield m.name, t.extractfile(m).read()
    else:
        with open(path, "rb") as fh:
            yield os.path.basename(path), fh.read()


def disc_windows(disc):
    """byte windows sampled from every file of the extracted disc"""
    out = []
    for dp, dns, fns in os.walk(disc):
        dns.sort()
        for f in sorted(fns):
            if f.startswith("_"):          # _listing.txt: written by extract_disc.py
                continue
            p = os.path.join(dp, f)
            size = os.path.getsize(p)
            if size < 256:
                continue
            n = 256 if size > (1 << 20) else 64
            step = max(1, (size - 64) // n)
            with open(p, "rb") as fh:
                for i in range(n):
                    fh.seek(i * step)
                    w = fh.read(64)
                    if len(w) == 64 and len(set(w)) > 16:   # skip padding / silence
                        out.append(w)
    return out


def listing_names(disc):
    names = set()
    lst = os.path.join(disc, "_listing.txt")
    if os.path.isfile(lst):
        with open(lst, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                for tok in line.split():
                    base = tok.rsplit("/", 1)[-1].split(";")[0]
                    if re.fullmatch(r"[A-Z0-9_]+\.[A-Z0-9]{1,3}", base):
                        names.add(base.upper())
    return names


def main(argv):
    disc = None
    no_disc = False
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--disc":
            disc = argv[i + 1]
            i += 2
            continue
        if argv[i] == "--no-disc":
            no_disc = True
            i += 1
            continue
        paths.append(argv[i])
        i += 1
    if not paths:
        print(__doc__.strip().splitlines()[2])
        return 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if disc is None and os.path.isfile(os.path.join(root, "disc", "SLUS_007.32")):
        disc = os.path.join(root, "disc")
    if disc is None and not no_disc:
        print("check_no_game_data: no extracted disc to compare with (--disc DIR, or --no-disc "
              "to check file names only)")
        return 2
    windows = disc_windows(disc) if disc else []
    names = listing_names(disc) if disc else set()
    findings = []
    nfiles = nbytes = 0
    for p in paths:
        for name, data in files_of(p):
            nfiles += 1
            nbytes += len(data)
            base = name.rsplit("/", 1)[-1].upper()
            if BAD_NAME.search(name) or base in names:
                findings.append(f"{p}: {name}: game-data file name")
            # a packaged archive inside the package is checked as well
            if name.endswith((".zip", ".tar.gz", ".tgz")):
                findings.append(f"{p}: {name}: archive inside the package (unpack and check it)")
            for w in windows:
                if w in data:
                    findings.append(f"{p}: {name}: contains bytes of a file on the game disc")
                    break
    what = f"{nfiles} files, {nbytes / 1e6:.1f} MB"
    what += f", {len(windows)} windows from {disc}" if windows else ", file names only"
    if findings:
        print("check_no_game_data: FAILED (" + what + ")")
        for f in findings[:50]:
            print("  " + f)
        return 1
    print("check_no_game_data: clean (" + what + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
