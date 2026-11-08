#!/usr/bin/env python3
"""Destrega static recompiler driver.

Usage: recomp.py [--out gen]

Inputs (relative to the project root):
  disc/SLUS_007.32, disc/OVL/*.BIN           extracted by tools/extract_disc.py
  analysis/rea_list_procedures_main.json     REA list_procedures export (main)
  analysis/overlays/<NAME>.procs.json        REA procedure exports (overlays)
  analysis/extra_seeds.txt                   optional extra entries: "<hex addr> [overlay]"
Outputs:
  gen/main_XX.c, gen/ovl_<NAME>.c, gen/table.c, gen/report.txt
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from image import Image                    # noqa: E402
from analyze import Analyzer               # noqa: E402
from codegen import FuncEmitter            # noqa: E402
import codegen                             # noqa: E402
from overlays import overlay_list          # noqa: E402

MAIN_CODE = (0x80026000, 0x800aa000)
OVERLAY_SLOTS = (0x80010000, 0x80026000)   # P1 slot 0x80010000, P2 slot 0x8001a268
FUNCS_PER_FILE = 80


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def rea_seeds(path):
    if not os.path.exists(path):
        return []
    d = json.load(open(path))
    items = d.get('normalized_result', d.get('result', d))
    if isinstance(items, dict):
        for k in ('procedures', 'items', 'matches', 'results'):
            if k in items:
                items = items[k]
                break
    out = []
    for p in items:
        a = p.get('address') if isinstance(p, dict) else None
        if a and a.startswith('0x'):
            out.append(int(a, 16))
    return out


def extra_seeds():
    path = 'analysis/extra_seeds.txt'
    res = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].split()
            if not line:
                continue
            res.setdefault(line[1] if len(line) > 1 else 'main', []).append(int(line[0], 16))
    return res


def overrides():
    """Main-exe functions replaced by hand-written C (analysis/overrides.txt).

    The generated body is kept as `f_<addr>_orig` (for differential checks);
    `f_<addr>` itself is then defined by the runtime (runtime/overrides/).
    """
    path = 'analysis/overrides.txt'
    res = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].split()
            if line:
                res.add(int(line[0], 16))
    return res


def widescreen_patches(img):
    """analysis/widescreen_cull.txt: `<hex addr> <orig imm> <wide imm>` per main-exe `slti`
    whose immediate is a screen-space culling bound; checked against the instruction."""
    path = 'analysis/widescreen_cull.txt'
    res = {}
    if os.path.exists(path):
        for n, line in enumerate(open(path), 1):
            f = line.split('#')[0].split()
            if not f:
                continue
            a, orig, wide = int(f[0], 16), int(f[1], 0), int(f[2], 0)
            i = img.ins(a)
            if i.op != 'slti' or i.imm != orig:
                raise SystemExit(f'{path}:{n}: {a:08x} is {i.op} imm {i.imm}, expected slti {orig}')
            res[a] = wide
    return res


def prologue_seeds(img, lo, hi):
    """Heuristic entries: `addiu sp,sp,-N` right after a `jr ra` + delay slot."""
    out = []
    for a in range(lo, hi, 4):
        i = img.ins(a)
        if i.op == 'addiu' and i.rs == 29 and i.rt == 29 and i.imm < 0:
            p = img.ins(a - 8) if a - 8 >= lo else None
            if a == lo or (p is not None and p.op == 'jr' and p.rs == 31):
                out.append(a)
    return out


def pointer_seeds(img, code_lo, code_hi, known):
    """Code addresses referenced as data: function-pointer tables and
    lui/addiu-built constants. Only accepted where the target looks like a
    function start (after `jr ra` + delay slot, or a stack-frame prologue)."""
    def plausible(t):
        if not (code_lo <= t < code_hi) or t & 3:
            return False
        if t in known:
            return True
        p = img.ins(t - 8)
        i = img.ins(t)
        if i.op == 'invalid':
            return False
        return (p.op == 'jr' and p.rs == 31) or (i.op == 'addiu' and i.rs == 29 and i.rt == 29 and i.imm < 0)
    out = set()
    for a in range(img.base, img.end - 3, 4):
        w = img.word(a)
        if plausible(w):
            out.add(w)
    for a in range(code_lo, code_hi - 4, 4):
        i = img.ins(a)
        if i.op != 'lui':
            continue
        for b in range(a + 4, min(a + 40, code_hi), 4):
            j = img.ins(b)
            if j.op == 'addiu' and j.rs == i.rt:
                t = ((i.imm << 16) + j.imm) & 0xffffffff
                if plausible(t):
                    out.add(t)
                break
    return out


def emit_funcs(funcs, image_funcs, name_of, tag):
    chunks = []
    for e in sorted(funcs):
        em = FuncEmitter(funcs[e], image_funcs, name_of, tag)
        chunks.append('\n'.join(em.emit()))
    return chunks


def func_range(f):
    lo = min(f.insns)
    hi = max(f.insns) + 4
    return lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='gen')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    report = []
    xs = extra_seeds()

    main_img = Image.from_psxexe('disc/SLUS_007.32')
    seeds = rea_seeds('analysis/rea_list_procedures_main.json') + [main_img.entry] + xs.get('main', [])
    ps = pointer_seeds(main_img, *MAIN_CODE, set(seeds))
    # Overlays call into the main executable (jal) and pass main-code
    # pointers around as data; both must be recompiled entry points.
    ovl_refs = set()   # overlay-slot addresses referenced from any overlay (jal or data word)
    for name, load in overlay_list():
        oimg = Image('o', load, open(f'disc/{name}', 'rb').read())
        for a in range(oimg.base, oimg.end - 3, 4):
            i = oimg.ins(a)
            t = i.target if i.op == 'jal' else oimg.word(a)
            if OVERLAY_SLOTS[0] <= t < OVERLAY_SLOTS[1] and t & 3 == 0:
                ovl_refs.add(t)
            if MAIN_CODE[0] <= t < MAIN_CODE[1] and t & 3 == 0:
                p, j = main_img.ins(t - 8), main_img.ins(t)
                if t in seeds or (p.op == 'jr' and p.rs == 31) or \
                        (j.op == 'addiu' and j.rs == 29 and j.rt == 29 and j.imm < 0):
                    ps.add(t)
    report.append(f'main: {len(ps - set(seeds))} extra seeds from code pointers')
    seeds += sorted(ps)
    an = Analyzer(main_img, *MAIN_CODE, seeds)
    main_funcs = an.run()
    codegen.WS_PATCHES = widescreen_patches(main_img)
    main_names = {e: f'f_{e:08x}' for e in main_funcs}
    report.append(f'main: {len(main_funcs)} functions, '
                  f'{sum(len(f.insns) for f in main_funcs.values())} instructions')

    # main -> overlay call targets (used as overlay seeds)
    main_calls = set()
    for f in main_funcs.values():
        main_calls |= f.calls | f.tailcalls

    table = []  # (addr, cname, ovl_id, lo, hi, hash)
    protos = []
    files = []

    def write_file(path, chunks, extra_protos=()):
        with open(path, 'w') as o:
            o.write('/* Generated by recomp/recomp.py - do not edit. */\n')
            o.write('#include "recomp.h"\n#include "gen_protos.h"\n\n')
            for c in chunks:
                o.write(c + '\n\n')
        files.append(os.path.basename(path))

    # ---- main executable --------------------------------------------------
    chunks = emit_funcs(main_funcs, set(main_funcs), lambda a: main_names[a], '')
    for a in sorted(overrides()):
        if a not in main_funcs:
            raise SystemExit(f'analysis/overrides.txt: {a:08x} is not a recompiled main function')
        head = f'void {main_names[a]}(void) {{'
        idx = [k for k, c in enumerate(chunks) if c.startswith(head)]
        if len(idx) != 1:
            raise SystemExit(f'override {a:08x}: cannot locate generated body')
        chunks[idx[0]] = f'void {main_names[a]}_orig(void) {{' + chunks[idx[0]][len(head):]
        protos.append(f'void {main_names[a]}_orig(void);')
    missing = set(codegen.WS_PATCHES) - codegen.WS_EMITTED
    if missing:
        raise SystemExit('analysis/widescreen_cull.txt: not in recompiled code: ' +
                         ' '.join(f'{a:08x}' for a in sorted(missing)))
    report.append(f'main: {len(codegen.WS_EMITTED)} widescreen culling patch points')
    for n in range(0, len(chunks), FUNCS_PER_FILE):
        write_file(f'{args.out}/main_{n // FUNCS_PER_FILE:02d}.c', chunks[n:n + FUNCS_PER_FILE])
    for e, f in sorted(main_funcs.items()):
        lo, hi = func_range(f)
        table.append((e, main_names[e], 0, lo, hi, 0))
        protos.append(f'void {main_names[e]}(void);')
        for b in f.bad:
            report.append(f'  main {e:08x}: {b[0]:08x} {b[1]}')

    # ---- overlays -----------------------------------------------------------
    ovl_infos = []
    for ovl_id, (name, load) in enumerate(overlay_list(), 1):
        data = open(f'disc/{name}', 'rb').read()
        base = os.path.basename(name).replace('.BIN', '')
        img = Image(base, load, data)
        lo, hi = load + 4, load + len(data)
        s = rea_seeds(f'analysis/overlays/{base}.procs.json')
        def looks_like_entry(t, img=img, lo=lo):
            i, p = img.ins(t), img.ins(t - 8) if t - 8 >= lo else None
            return t == lo or (p is not None and p.op == 'jr' and p.rs == 31) or \
                (i.op == 'addiu' and i.rs == 29 and i.rt == 29 and i.imm < 0)
        s += [t for t in main_calls if lo <= t < hi and looks_like_entry(t)]
        # Overlays also call into each other (the P2 event overlay calls helpers
        # in the P1 slot): any overlay's jal/data pointer into this slot range
        # that looks like a function start is an entry. Entries for an overlay
        # that is not loaded never match (table lookups verify the code hash).
        s += [t for t in ovl_refs if lo <= t < hi and looks_like_entry(t)]
        s += prologue_seeds(img, lo, hi)
        s += xs.get(base, [])
        s = [x for x in s if lo <= x < hi and x & 3 == 0]
        # Skip seeds that decode as invalid immediately (header/data).
        s = [x for x in s if img.ins(x).op != 'invalid']
        oan = Analyzer(img, lo, hi, s, external_funcs=set(main_funcs))
        ofuncs = oan.run()
        tag = f'_{base}'
        onames = {e: f'f_{e:08x}{tag}' for e in ofuncs}

        def name_of(a, onames=onames):
            return onames[a] if a in onames else main_names[a]

        callable_ = set(ofuncs) | set(main_funcs)
        chunks = emit_funcs(ofuncs, callable_, name_of, tag)
        write_file(f'{args.out}/ovl_{base}.c', chunks)
        for e, f in sorted(ofuncs.items()):
            flo, fhi = func_range(f)
            h = fnv1a(data[flo - load:fhi - load])
            table.append((e, onames[e], ovl_id, flo, fhi, h))
            protos.append(f'void {onames[e]}(void);')
            for b in f.bad:
                report.append(f'  {base} {e:08x}: {b[0]:08x} {b[1]}')
        ovl_infos.append((name, load, len(data)))
        report.append(f'{base}: {len(ofuncs)} functions (REA seeds {len(rea_seeds(f"analysis/overlays/{base}.procs.json"))})')

    with open(f'{args.out}/gen_protos.h', 'w') as o:
        o.write('/* Generated - do not edit. */\n#pragma once\n')
        o.write('\n'.join(protos) + '\n')

    table.sort(key=lambda t: (t[0], t[2]))
    with open(f'{args.out}/table.c', 'w') as o:
        o.write('/* Generated - do not edit. */\n#include "psx.h"\n#include "gen_protos.h"\n\n')
        o.write('const RecompEntry g_recomp_table[] = {\n')
        for a, n, ov, lo, hi, h in table:
            o.write(f'    {{0x{a:08x}, {n}, {ov}, 0x{lo:08x}, 0x{hi:08x}, 0x{h:08x}}},\n')
        o.write('};\n')
        o.write(f'const u32 g_recomp_count = {len(table)};\n\n')
        o.write('const OverlayInfo g_overlays[] = {\n')
        for name, load, size in ovl_infos:
            o.write(f'    {{"{name}", 0x{load:08x}, {size}}},\n')
        o.write('};\n')
        o.write(f'const u32 g_overlay_count = {len(ovl_infos)};\n')
    files.append('table.c')
    with open(f'{args.out}/files.cmake', 'w') as o:
        o.write('set(GEN_SOURCES\n' + '\n'.join(f'  ${{GEN_DIR}}/{f}' for f in files) + '\n)\n')
    with open(f'{args.out}/report.txt', 'w') as o:
        o.write('\n'.join(report) + '\n')
    print('\n'.join(r for r in report if not r.startswith('  ')))
    print(f'total table entries: {len(table)}; files: {len(files)}')


if __name__ == '__main__':
    main()
