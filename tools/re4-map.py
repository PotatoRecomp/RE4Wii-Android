#!/usr/bin/env python3
"""Build config/re4/MAP.txt (function starts) for the RE4 main.dol.

Seeds: bl targets, data words pointing into code that look like a function start (prologue, bl target,
or right after a blr, unless part of a switch jump table), and lis+addi/ori pairs in code with the same test.
The RELs are scanned too, linked at their fixed homes (tools/re4-rels.py), with each module's
prolog / epilog / unresolved entry as an extra seed.
Usage: python -I tools/re4-map.py
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import w1  # noqa: E402
import importlib  # noqa: E402
rels = importlib.import_module('re4-rels')

DOL = ROOT / 'assets' / 're4' / 'sys' / 'main.dol'
OUT = ROOT / 'config' / 're4' / 'MAP.txt'


def main():
    data = DOL.read_bytes()
    secs, entry, _, _ = w1.parse_dol(data)
    mods, image, far = rels.build()
    # Reached only by a tail `b`: Link (0x8017BB58) and OSLink (0x8017BE38, li r5,0; b Link), which the
    # runtime's OSLink hook calls.
    extra = {0x8017BB58, 0x8017BE38}
    for m in mods:
        for i, (off, size, ex) in enumerate(m.sections):
            if off and size:
                secs.append((len(data) + m.base[i] - rels.REL_BASE, m.base[i], size, ex))
        for sec, off in ((m.prolog_sec, m.prolog), (m.epilog_sec, m.epilog), (m.unres_sec, m.unresolved)):
            if sec:
                extra.add(m.addr(sec, off))
    data += bytes(image)
    text = [(o, a, s) for o, a, s, t in secs if t]
    in_text = lambda x: x % 4 == 0 and any(a <= x < a + s for _, a, s in text)
    wd = lambda x: w1.word(data, secs, x) or 0

    def is_prolog(x):
        for k in range(3):
            w = wd(x + 4 * k)
            if w == 0x7C0802A6:  # mflr r0
                return True
            if w >> 26 == 37 and (w >> 21) & 31 == 1 and (w >> 16) & 31 == 1 and w & 0x8000:  # stwu r1,-N(r1)
                return True
        return False

    def after_end(x):
        p = wd(x - 4)
        return p == 0x4E800020 or p == 0x4C000064  # blr / rfi

    def after_jump(x):
        # bctr or an unconditional b also end code, but switch case labels follow them too: only
        # trusted for entries of a run already classified as a callback table.
        p = wd(x - 4)
        return p == 0x4E800420 or (p >> 26 == 18 and p & 3 == 0)

    bl = set()
    pairs = set()
    # Switch tables, recognised from the code: addi rT, rT, table@l; lwzx rX, rT, rI; mtctr rX; bctr
    # within a few words. A function-pointer call (lwz + mtctr + bctrl, or a tail bctr through a
    # pointer loaded with lwz) does not match.
    jt_bases = set()
    for o, a, s in text:
        hi = {}
        tab = {}       # register -> (table address, word index) from the last addi/ori
        loaded = {}    # register -> (table address, word index) from lwzx rX, rT, rI
        ctr = None
        for i in range(0, s, 4):
            w = struct.unpack('>I', data[o + i:o + i + 4])[0]
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            rb, xo = (w >> 11) & 31, (w >> 1) & 0x3FF
            if op == 18 and (w & 3) == 1:
                t = far.get(a + i, (a + i + w1.sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF)
                if in_text(t):
                    bl.add(t)
            if op == 15 and ra == 0:
                hi[rd] = imm << 16
            elif op == 14 and ra in hi:
                v = (hi[ra] + w1.sext(imm, 16)) & 0xFFFFFFFF
                pairs.add(v)
                tab[rd] = (v, i)
            elif op == 24 and rd in hi:
                pairs.add(hi[rd] | imm)
                tab[ra] = (hi[rd] | imm, i)
            elif op == 31 and xo == 23:  # lwzx rD, rA, rB
                src = tab.get(ra) or tab.get(rb)
                if src and i - src[1] <= 8 * 4:
                    loaded[rd] = src
            elif op == 31 and xo == 467 and ((w >> 11) & 0x3FF) == (9 << 5):  # mtctr rS
                ctr = loaded.get(rd)
            if w == 0x4E800420 and ctr and i - ctr[1] <= 10 * 4:  # bctr (no link)
                jt_bases.add(ctr[0])
            if w == 0x4E800020 or w == 0x4E800420:
                hi.clear()
                tab.clear()
                loaded.clear()
                ctr = None
    jt_words = set()
    for b in jt_bases:
        x = b
        while in_text(wd(x)) and (x == b or x not in jt_bases):
            jt_words.add(x)
            x += 4

    ok = lambda v: in_text(v) and (v in bl or is_prolog(v) or after_end(v))
    # Runs of code pointers in data; a tight run (switch jump table) is skipped unless its targets are real starts.
    ptr = set()
    text_ends = {a + s for _, a, s in text}
    for o, a, s, t in secs:
        if t:
            continue
        if a in text_ends and s <= 0x1000:  # .ctors / .dtors right after .text: every entry is a function
            for i in range(0, s - 3, 4):
                v = struct.unpack('>I', data[o + i:o + i + 4])[0]
                if in_text(v):
                    ptr.add(v)
            continue
        run = []
        for i in list(range(0, s - 3, 4)) + [None]:
            v = struct.unpack('>I', data[o + i:o + i + 4])[0] if i is not None else 0
            if i is not None and a + i in jt_words:  # a known switch table entry: never a function
                if v in bl or is_prolog(v):
                    ptr.add(v)
                v = 0
            if in_text(v):
                run.append(v)
                continue
            # A tight run is a switch jump table unless most targets look like starts (a callback table).
            starts_like = sum(1 for x in run if x in bl or is_prolog(x) or after_end(x))
            # Switch tables found from their code are already cut out (jt_words); this fallback only
            # catches tight runs where no entry looks like a start.
            jt = len(run) >= 3 and max(run) - min(run) < 0x1000 and starts_like == 0
            for x in run:
                # back-to-back unrecognised jump tables fail this; small pointer tables pass
                cb = len(run) >= 3 and (starts_like * 2 >= len(run) or (len(run) <= 8 and starts_like >= 1))
                if (x in bl or is_prolog(x)) or (not jt and (after_end(x) or (cb and after_jump(x)))):
                    ptr.add(x)
            run = []
    code_refs = {v for v in pairs if ok(v)}
    starts = sorted(bl | ptr | code_refs | extra | {entry})
    OUT.write_text(''.join('%08X sub_%08X\n' % (x, x) for x in starts), encoding='ascii')
    print('MAP: %d bl, %d data ptr, %d code refs -> %d starts -> %s'
          % (len(bl), len(ptr), len(code_refs), len(starts), OUT.relative_to(ROOT)))


if __name__ == '__main__':
    main()
