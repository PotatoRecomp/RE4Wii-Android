#!/usr/bin/env python3
"""Build config/re4/MAP.txt (function starts) for the RE4 main.dol.

Seeds: bl targets, data words pointing into code that look like a function start (prologue, bl target,
or right after a blr, unless part of a switch jump table), and lis+addi/ori pairs in code with the same test.
Usage: python -I tools/re4-map.py
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import w1  # noqa: E402

DOL = ROOT / 'assets' / 're4' / 'sys' / 'main.dol'
OUT = ROOT / 'config' / 're4' / 'MAP.txt'


def main():
    data = DOL.read_bytes()
    secs, entry, _, _ = w1.parse_dol(data)
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

    bl = set()
    pairs = set()
    for o, a, s in text:
        hi = {}
        for i in range(0, s, 4):
            w = struct.unpack('>I', data[o + i:o + i + 4])[0]
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 18 and (w & 3) == 1:
                t = (a + i + w1.sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF
                if in_text(t):
                    bl.add(t)
            if op == 15 and ra == 0:
                hi[rd] = imm << 16
            elif op == 14 and ra in hi:
                pairs.add((hi[ra] + w1.sext(imm, 16)) & 0xFFFFFFFF)
            elif op == 24 and rd in hi:
                pairs.add(hi[rd] | imm)
            if w == 0x4E800020:
                hi.clear()

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
            if in_text(v):
                run.append(v)
                continue
            # A tight run is a switch jump table unless most targets look like starts (a callback table).
            starts_like = sum(1 for x in run if x in bl or is_prolog(x) or after_end(x))
            jt = len(run) >= 3 and max(run) - min(run) < 0x1000 and starts_like * 2 < len(run)
            for x in run:
                if (x in bl or is_prolog(x)) or (not jt and after_end(x)):
                    ptr.add(x)
            run = []
    code_refs = {v for v in pairs if ok(v)}
    starts = sorted(bl | ptr | code_refs | {entry})
    OUT.write_text(''.join('%08X sub_%08X\n' % (x, x) for x in starts), encoding='ascii')
    print('MAP: %d bl, %d data ptr, %d code refs -> %d starts -> %s'
          % (len(bl), len(ptr), len(code_refs), len(starts), OUT.relative_to(ROOT)))


if __name__ == '__main__':
    main()
