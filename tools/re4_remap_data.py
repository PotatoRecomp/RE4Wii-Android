#!/usr/bin/env python3
"""Map Basara guest addresses used as constants in runtime/src (SDK globals, callbacks) to RE4 addresses.

Functions whose code matches 1:1 (tools/re4_remap.py checksum) are walked in parallel; every data address one
side references (lis+addi/ori, lis base + load/store displacement, r13/r2 SDA offsets) and every bl target is paired
with the other side's (and an initialised pointer at a paired address pairs its value). A constant with no direct pair takes the offset of the nearest paired addresses around it
when both agree (same object, same layout).
--apply rewrites 0x80XXXXXX tokens outside comments; writes generated/b/re4_data_map.csv.

Usage: python -I tools/re4_remap_data.py [--apply]
"""
import argparse
import bisect
import csv
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import w1  # noqa: E402
import re4_remap as rm  # noqa: E402

B_SDA, B_SDA2 = 0x80667C80, 0x8066B100
R_SDA, R_SDA2 = 0x80444A20, 0x80446B60
TOKEN = re.compile(r'0x(80[0-9A-Fa-f]{6})(u?)')
LOAD_STORE = set(range(32, 56))
STORES = {36, 37, 38, 39, 44, 45, 52, 53, 54, 55}
LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 46}


def refs(data, secs, start, size, sda, sda2):
    """Per instruction index: list of (kind, address) it references."""
    hi, out = {}, []
    for k in range(size // 4):
        a = start + 4 * k
        w = w1.word(data, secs, a) or 0
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        s = w1.sext(imm, 16)
        r = []
        if op == 18 and (w & 1):
            r.append(('code', (a + w1.sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF))
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
        elif op == 14 and ra in hi:
            r.append(('data', (hi[ra] + s) & 0xFFFFFFFF))
        elif op == 24 and rd in hi:
            r.append(('data', hi[rd] | imm))
        elif op in LOAD_STORE or op == 14:
            if ra == 13:
                r.append(('data', (sda + s) & 0xFFFFFFFF))
            elif ra == 2:
                r.append(('data', (sda2 + s) & 0xFFFFFFFF))
            elif ra in hi and op != 14:
                r.append(('data', (hi[ra] + s) & 0xFFFFFFFF))
        # Forget a lis value once its register is overwritten.
        if op == 15 and ra == 0:
            pass
        elif op in STORES or op in (10, 11, 16, 18, 19, 47, 48, 50):
            pass
        elif op == 14 or op in LOADS:
            hi.pop(rd, None)
        elif op == 24 or op in (21, 23, 25, 26, 27, 28, 29):
            hi.pop(ra, None)
        else:
            hi.pop(rd, None)
            hi.pop(ra, None)
        out.append(r)
    return out


def build_map():
    bd, bt, bs = rm.load(rm.B_DOL, rm.B_MAP)
    rd, rt, rs = rm.load(rm.R_DOL, rm.R_MAP)
    bsecs, *_ = w1.parse_dol(bd)
    rsecs, *_ = w1.parse_dol(rd)
    bsig = {}
    for start, size, sm, limit in rm.prefix_sums(bd, bt, bs):
        if start + size == limit:
            bsig[start] = (sm, size)
    want = defaultdict(list)
    for addr, key in bsig.items():
        want[key].append(addr)
    direct = defaultdict(set)
    for start, size, sm, _ in rm.prefix_sums(rd, rt, rs):
        for addr in want.get((sm, size), ()):
            direct[addr].add(start)
    votes = defaultdict(Counter)
    pairs = 0
    for b, c in direct.items():
        if len(c) != 1:
            continue
        r = next(iter(c))
        size = bsig[b][1]
        if size < 16:  # too short to trust
            continue
        votes[b][r] += 4
        for x, y in zip(refs(bd, bsecs, b, size, B_SDA, B_SDA2), refs(rd, rsecs, r, size, R_SDA, R_SDA2)):
            for (k1, a1), (k2, a2) in zip(x, y):
                if k1 == k2:
                    votes[a1][a2] += 1
        pairs += 1
    m = {}
    for a, c in votes.items():
        (best, n), = c.most_common(1)
        if n * 2 > sum(c.values()):
            m[a] = best
    # Initialised pointers: a paired word that holds an address on both sides pairs those addresses too.
    for a, b in list(m.items()):
        x, y = w1.word(bd, bsecs, a), w1.word(rd, rsecs, b)
        if x and y and a % 4 == 0 and 0x80004000 <= x < 0x81800000 and 0x80004000 <= y < 0x81800000:
            m.setdefault(x, y)
    return m, pairs


def resolve(m, keys, addr):
    if addr in m:
        return m[addr], 'direct'
    i = bisect.bisect_left(keys, addr)
    if 0 < i < len(keys):
        b0, b1 = keys[i - 1], keys[i]
        if m[b0] - b0 == m[b1] - b1 and b1 - b0 <= 0x400:
            return addr + (m[b0] - b0), 'between'
    return None, 'none'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    m, pairs = build_map()
    keys = sorted(m)
    print('matched functions walked: %d, address pairs: %d' % (pairs, len(m)))
    stats, rows, changed = Counter(), [], Counter()
    for f in sorted(rm.RT.rglob('*')):
        if f.suffix not in ('.cpp', '.h', '.hpp', '.inc'):
            continue
        lines = f.read_text(encoding='utf-8', errors='surrogateescape').splitlines(keepends=True)
        dead = rm.inactive_lines(lines)
        dirty = False
        for i, line in enumerate(lines):
            if i in dead or line.lstrip().startswith(('//', '*', '#define')):
                continue
            code, sep, comment = line.partition('//')
            if rm.br.HOOK.search(code):
                continue

            def sub(mt):
                nonlocal dirty
                addr = int(mt.group(1), 16)
                if addr < 0x80004000:  # OS low memory globals: same on every title
                    return mt.group(0)
                new, how = resolve(m, keys, addr)
                stats[how] += 1
                rows.append((f.relative_to(rm.RT).as_posix(), i + 1, '%08X' % addr, how,
                             '%08X' % new if new is not None else ''))
                if new is None or not a.apply:
                    return mt.group(0)
                dirty = True
                changed[how] += 1
                tok = '%08X' % new
                if mt.group(1).islower() or mt.group(1) == mt.group(1).lower():
                    tok = tok.lower() if any(ch.isalpha() for ch in mt.group(1)) else tok
                return '0x' + tok + mt.group(2)
            lines[i] = TOKEN.sub(sub, code) + sep + comment
        if dirty:
            f.write_text(''.join(lines), encoding='utf-8', errors='surrogateescape')
    rm.OUT.mkdir(parents=True, exist_ok=True)
    with (rm.OUT / 're4_data_map.csv').open('w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['file', 'line', 'basara', 'how', 're4'])
        w.writerows(rows)
    print('constants:', dict(stats), '-> generated/b/re4_data_map.csv')
    if a.apply:
        print('apply:', dict(changed))


if __name__ == '__main__':
    main()
