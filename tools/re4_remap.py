#!/usr/bin/env python3
"""Move runtime hooks from Basara addresses to RE4 (RB4E08) addresses.

1. Direct match: the Basara function at the hook address (bounds from the Basara MAP) is hashed with Dolphin's
   code checksum (immediates and branch targets masked) and looked up among RE4 MAP starts (same length prefix).
2. Fallback: Dolphin totaldb.dsy SDK name of the Basara address -> unique RE4 address with that name; else the
   candidate nearest the address predicted from the closest uniquely matched neighbour; else, when the matched
   neighbours on both sides share one offset, the same offset.
3. --apply: rewrite matched addresses; wrap the rest (and every hook in hle/basara_*.cpp) in "#if 0".
Writes generated/b/re4_hooks.csv.

Usage: python -I tools/re4_remap.py [--apply]
"""
import argparse
import csv
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import w1  # noqa: E402
import sdk_hooks as br  # noqa: E402

BASARA = ROOT.parent / 'b2hwii-android'
B_DOL = BASARA / 'assets' / 'basara-heroes' / 'sys' / 'main.dol'
B_MAP = BASARA / 'config' / 'basara' / 'MAP.txt'
R_DOL = ROOT / 'assets' / 're4' / 'sys' / 'main.dol'
R_MAP = ROOT / 'config' / 're4' / 'MAP.txt'
br.DSY = BASARA / 'refs' / 'NWiiRecomp' / 'third_party' / 'dolphin-sigdb' / 'totaldb.dsy'
RT = ROOT / 'runtime' / 'src'
OUT = ROOT / 'generated' / 'b'
CAP = 0x8000


def load(dol, mp):
    data = dol.read_bytes()
    secs, *_ = w1.parse_dol(data)
    text = [(o, a, s) for o, a, s, t in secs if t]
    starts = sorted(int(l.split()[0], 16) for l in mp.read_text().splitlines() if l.strip())
    return data, text, starts


def prefix_sums(data, text, starts, want=None):
    """Yield (start, size, checksum) for every prefix of every function (or only sizes in want[start])."""
    for idx, start in enumerate(starts):
        seg = next(((o, a, s) for o, a, s in text if a <= start < a + s), None)
        if seg is None:
            continue
        o, a, s = seg
        limit = min(starts[idx + 1] if idx + 1 < len(starts) else a + s, a + s, start + CAP)
        p, sm = o + start - a, 0
        for k in range((limit - start) // 4):
            sm = br.checksum_step(sm, struct.unpack_from('>I', data, p + 4 * k)[0])
            yield start, 4 * (k + 1), sm, limit


def dsy_names(data, text, starts, db):
    names = defaultdict(set)
    for start, size, sm, _ in prefix_sums(data, text, starts):
        for nm in db.get((sm, size), ()):
            names[nm].add(start)
    return names


def inactive_lines(lines):
    """0-based indexes of lines inside #if 0 (#if 0 / #else / #endif nesting)."""
    dead, depth = set(), []
    for i, line in enumerate(lines):
        t = line.strip()
        if t.startswith('#if'):
            depth.append(t.startswith('#if 0') or (bool(depth) and depth[-1]))
        elif t.startswith('#else'):
            if depth:
                depth[-1] = True if (len(depth) > 1 and depth[-2]) else not depth[-1]
        elif t.startswith('#endif'):
            if depth:
                depth.pop()
        elif depth and depth[-1]:
            dead.add(i)
    return dead


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    bd, bt, bs = load(B_DOL, B_MAP)
    rd, rt, rs = load(R_DOL, R_MAP)
    br.RT = RT
    inv = br.inventory()
    hooked = {addr for *_, addr in inv}
    # A hook may sit on a start the Basara MAP lacks (the translator found it later): treat it as a start.
    bs = sorted(set(bs) | {x for x in hooked if any(a <= x < a + s for _, a, s in bt)})

    # Basara: full-function checksum of each hooked start.
    bset = set(bs)
    bsig = {}
    for start, size, sm, limit in prefix_sums(bd, bt, bs):
        if start + size == limit:
            bsig[start] = (sm, size)
    want = defaultdict(list)
    for addr, key in bsig.items():
        want[key].append(addr)
    direct = defaultdict(set)
    for start, size, sm, _ in prefix_sums(rd, rt, rs):
        for addr in want.get((sm, size), ()):
            direct[addr].add(start)

    # Anchors: Basara functions with exactly one RE4 match; an ambiguous hook takes the candidate nearest to the
    # address predicted from the closest anchor (the SDK libraries are laid out in the same order).
    anchors = sorted((b, next(iter(c))) for b, c in direct.items() if len(c) == 1)
    akeys = [b for b, _ in anchors]

    def by_neighbour(addr, cand):
        import bisect
        i = bisect.bisect_left(akeys, addr)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(anchors) and abs(anchors[j][0] - addr) < 0x4000:
                pred = anchors[j][1] + (addr - anchors[j][0])
                c = min(cand, key=lambda x: abs(x - pred))
                if abs(c - pred) <= 0x100 and (best is None or abs(anchors[j][0] - addr) < best[1]):
                    best = (c, abs(anchors[j][0] - addr))
        return best[0] if best else None

    rset = set(rs)
    rsize = {x: (rs[i + 1] if i + 1 < len(rs) else x) - x for i, x in enumerate(rs)}
    bsize = {x: (bs[i + 1] if i + 1 < len(bs) else x) - x for i, x in enumerate(bs)}

    def predicted(addr):
        """Anchors on both sides with the same offset (no layout change in between) -> same offset here."""
        import bisect
        i = bisect.bisect_left(akeys, addr)
        if 0 < i < len(anchors):
            (b0, r0), (b1, r1) = anchors[i - 1], anchors[i]
            if r0 - b0 == r1 - b1 and b1 - b0 < 0x2000:
                return addr + (r0 - b0)
        return None

    db = br.load_dsy()
    bnames = dsy_names(bd, bt, bs, db)
    rnames = dsy_names(rd, rt, rs, db)
    b_by_addr = defaultdict(set)
    for nm, adrs in bnames.items():
        for x in adrs:
            b_by_addr[x].add(nm)

    verdicts, stats = {}, defaultdict(int)
    with (OUT / 're4_hooks.csv').open('w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['file', 'line', 'macro', 'basara', 'name', 'verdict', 're4'])
        for f, ln, macro, addr in inv:
            nm = ','.join(sorted(b_by_addr.get(addr, ())))
            new, v = None, 'absent'
            if f.name.startswith('basara_'):
                v = 'game'
            elif addr not in bset and not any(a <= addr < a + s for _, a, s in bt):
                v = 'notstart'
            elif len(direct.get(addr, ())) == 1:
                new, v = next(iter(direct[addr])), 'direct'
            else:
                cand = set()
                for n in b_by_addr.get(addr, ()):
                    cand |= rnames.get(n, set())
                if len(cand) == 1:
                    new, v = next(iter(cand)), 'name'
                elif cand or len(direct.get(addr, ())) > 1:
                    new = by_neighbour(addr, direct.get(addr) or cand)
                    v = 'neighbour' if new is not None else 'ambiguous'
                if new is None and (addr in bsize):
                    new = predicted(addr)
                    if new is not None:
                        v = 'predicted'
            verdicts[(f, ln, addr)] = (v, new)
            stats[v] += 1
            w.writerow([f.relative_to(RT).as_posix(), ln, macro, '%08X' % addr, nm, v,
                        '%08X' % new if new is not None else ''])
    print('hooks: %d %s -> %s' % (len(inv), dict(stats), (OUT / 're4_hooks.csv').relative_to(ROOT)))
    if not a.apply:
        return

    by_file = defaultdict(list)
    for (f, ln, addr), vn in verdicts.items():
        by_file[f].append((ln, addr) + vn)
    changed = defaultdict(int)
    for f, items in by_file.items():
        lines = f.read_text(encoding='utf-8', errors='surrogateescape').splitlines(keepends=True)
        disable = []
        dead = inactive_lines(lines)
        for ln, addr, v, new in items:
            i = ln - 1
            if i in dead:
                continue
            for m in br.HOOK.finditer(lines[i]):
                if int(m.group(3), 16) != addr:
                    continue
                if new is not None:
                    tok = '%08X' % new
                    if m.group(3) == m.group(3).lower() and any(c.isalpha() for c in m.group(3)):
                        tok = tok.lower()
                    lines[i] = lines[i][:m.start(3)] + tok + lines[i][m.end(3):]
                    changed['moved'] += 1
                else:
                    disable.append((i, br.statement_end(lines, i, m.start())))
                break
        for s, e in sorted(set(disable), reverse=True):
            ind = re.match(r'\s*', lines[s]).group(0)
            lines.insert(e + 1, ind + '#endif  // re4-remap: no RE4 match\n')
            lines.insert(s, ind + '#if 0  // re4-remap: no RE4 match\n')
            changed['disabled'] += 1
        f.write_text(''.join(lines), encoding='utf-8', errors='surrogateescape')
    print('apply:', dict(changed))


if __name__ == '__main__':
    main()
