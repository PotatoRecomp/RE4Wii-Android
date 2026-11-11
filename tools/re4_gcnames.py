#!/usr/bin/env python3
"""Name RE4 Wii main.dol functions from the GameCube debug decompilation (refs/re4-gc, read only).

The GC game code was built with SN ProDG (GCC) and the Wii one with CodeWarrior, so bytes do not match.
Matching is structural instead:
1. Source side: refs/re4-gc/src/{game,lib} is parsed into functions (qualified name, string literals,
   called names in order); names are tied to config/G4BE08/sym_map.tsv entries of the same unit.
2. Wii side: function starts from config/re4/MAP.txt, string references (lis/addi, r13/r2 SDA) and bl
   callees in order; Dolphin totaldb.dsy SDK names as extra anchors.
3. Seeds: a string used by exactly one source function and one Wii function; SDK names.
4. Propagation: for every matched pair, callees are aligned by order between already matched anchors;
   a gap holding exactly one unmatched callee on each side (same unit or unique name) becomes a pair.
   Proposals must keep the debug order of the unit; call propagation is limited to game/ code because
   the GC SDK and the RVL SDK differ.
5. Gap fill: between two matched functions of one unit, equal function counts on both sides are paired
   in order. Matches out of debug order inside a unit are dropped at the end.
Writes generated/b/re4_gcnames.csv (address, name, demangled, unit, method, gc_address).

Usage: python -I tools/re4_gcnames.py
"""
import csv
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import w1  # noqa: E402
import sdk_hooks as br  # noqa: E402
import re4_remap as rm  # noqa: E402

GC = ROOT / 'refs' / 're4-gc'
SRC = GC / 'src'
SYMMAP = GC / 'config' / 'G4BE08' / 'sym_map.tsv'
DOL = ROOT / 'assets' / 're4' / 'sys' / 'main.dol'
MAP = ROOT / 'config' / 're4' / 'MAP.txt'
OUT = ROOT / 'generated' / 'b' / 're4_gcnames.csv'

KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'do', 'else', 'case', 'new', 'delete',
            'defined', 'asm', '__asm', 'catch', 'throw', 'static_cast', 'reinterpret_cast', 'const_cast'}
ESC = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'", 'a': '\a', 'b': '\b',
       'f': '\f', 'v': '\v'}


# ---------------------------------------------------------------- source side
def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x':
                m = re.match(r'[0-9a-fA-F]{1,2}', s[i + 2:])
                out.append(chr(int(m[0], 16)) if m else 'x')
                i += 2 + (len(m[0]) if m else 0)
                continue
            if n in '01234567':
                m = re.match(r'[0-7]{1,3}', s[i + 1:])
                out.append(chr(int(m[0], 8)))
                i += 1 + len(m[0])
                continue
            out.append(ESC.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def tokenize(text):
    """Strip comments and preprocessor lines; replace string literals by placeholders."""
    strs, out, i, n = [], [], 0, len(text)
    bol = True
    while i < n:
        c = text[i]
        if bol and c == '#':
            j = i
            while j < n and (text[j] != '\n' or text[j - 1] == '\\'):
                j += 1
            i, bol = j, False
            continue
        if c == '/' and text.startswith('//', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
            continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            lit = ''.join(buf)
            if out and out[-1].startswith('\x01') and out[-1].endswith('\x01'):   # "a" "b" concatenation
                strs[-1] += unescape(lit)
            else:
                strs.append(unescape(lit))
                out.append('\x01%d\x01' % (len(strs) - 1))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == '\\' else 1
            out.append('0')
            i = j + 1
            continue
        if c == '\n':
            bol = True
        elif not c.isspace():
            bol = False
        if c.isspace() and out and out[-1].startswith('\x01'):
            i += 1          # keep adjacent literals adjacent
            continue
        out.append(c)
        i += 1
    return ''.join(out), strs


NAME_RE = re.compile(r'((?:[A-Za-z_]\w*\s*(?:<[^<>()]*>)?\s*::\s*)*~?(?:operator\s*\S{1,3}|[A-Za-z_]\w*))\s*$')
CALL_RE = re.compile(r'([A-Za-z_~][\w]*(?:\s*::\s*~?[A-Za-z_]\w*)*)\s*\(')


def parse_file(path):
    code, strs = tokenize(path.read_text(encoding='latin-1'))
    funcs, depth, i, n = [], 0, 0, len(code)
    head_start, transparent = 0, []
    while i < n:
        c = code[i]
        if c == '{':
            head = code[head_start:i].strip()
            if depth == len(transparent) and re.search(r'(^|\s)(extern\s*\x01\d+\x01|namespace(\s+\w+)?)$', head):
                transparent.append(depth)
                depth += 1
                head_start = i + 1
                i += 1
                continue
            if depth == len(transparent) and ')' in head and not re.search(r'=\s*$', head):
                # find the parameter list: last top-level "(...)" before an optional ": init-list" / const
                m = None
                h = re.sub(r'\)\s*(const|throw\s*\([^)]*\))?\s*(:[^{;]*)?$', ')', head)
                pd, k = 0, len(h) - 1
                while k >= 0:
                    if h[k] == ')':
                        pd += 1
                    elif h[k] == '(':
                        pd -= 1
                        if pd == 0:
                            m = NAME_RE.search(h[:k])
                            break
                    k -= 1
                end, bd = i + 1, 1
                while end < n and bd:
                    bd += {'{': 1, '}': -1}.get(code[end], 0)
                    end += 1
                if m:
                    name = re.sub(r'\s+', '', m[1])
                    name = re.sub(r'<[^<>]*>', '', name)
                    body = code[i:end]
                    lits = [strs[int(x)] for x in re.findall(r'\x01(\d+)\x01', body)]
                    calls = [re.sub(r'\s+', '', x) for x in CALL_RE.findall(body)]
                    calls = [x for x in calls if x.split('::')[-1] not in KEYWORDS]
                    funcs.append((name, lits, calls))
                i = end
                head_start = i
                continue
            depth += 1
            head_start = i + 1
        elif c == '}':
            depth -= 1
            if transparent and transparent[-1] == depth:
                transparent.pop()
            head_start = i + 1
        elif c == ';' and depth == len(transparent):
            head_start = i + 1
        i += 1
    return funcs


def load_source():
    """Return list of dicts: sym, dem, unit, lits, calls (main.dol units only)."""
    bykey = defaultdict(list)
    for row in csv.DictReader(SYMMAP.open(encoding='utf-8'), delimiter='\t'):
        if row['section'] != '.text':
            continue
        bykey[(row['unit'], row['demangled'])].append((int(row['address'], 16), row['name']))
    for v in bykey.values():
        v.sort()
    units = {u for u, _ in bykey}
    out = []
    for unit in sorted(units):
        p = SRC / unit
        if not p.exists():
            continue
        seen = Counter()
        for name, lits, calls in parse_file(p):
            cands = bykey.get((unit, name))
            if not cands:
                continue
            k = seen[name]
            seen[name] += 1
            if k >= len(cands):
                continue
            out.append({'sym': cands[k][1], 'gc': cands[k][0], 'dem': name, 'unit': unit, 'lits': lits, 'calls': calls})
    return out


# ---------------------------------------------------------------- Wii side
def load_wii():
    data = DOL.read_bytes()
    secs, *_ = w1.parse_dol(data)
    text = [(o, a, s) for o, a, s, t in secs if t]
    starts = sorted(int(l.split()[0], 16) for l in MAP.read_text().splitlines() if l.strip())
    cfg = (ROOT / 'config' / 're4' / 'recomp.yml').read_text()
    r13 = int(re.search(r'sda_base\D*0x([0-9A-Fa-f]+)', cfg)[1], 16) if 'sda_base' in cfg else 0x80444A20
    r2 = int(re.search(r'sda2_base\D*0x([0-9A-Fa-f]+)', cfg)[1], 16) if 'sda2_base' in cfg else 0x80446B60

    def rd(addr, n):
        for o, a, s, _ in secs:
            if a <= addr and addr + n <= a + s:
                return data[o + addr - a:o + addr - a + n]
        return None

    def cstr(addr):
        b = rd(addr, 1)
        if b is None:
            return None
        out = bytearray()
        while len(out) < 512:
            b = rd(addr + len(out), 1)
            if not b or b == b'\0':
                break
            out += b
        if len(out) < 4 or not b:
            return None
        if sum(1 for x in out if x < 9 or 13 < x < 32) > 0:
            return None
        return out.decode('latin-1')

    funcs = {}
    tset = set(starts)
    for idx, start in enumerate(starts):
        seg = next(((o, a, s) for o, a, s in text if a <= start < a + s), None)
        if seg is None:
            continue
        o, a, s = seg
        end = min(starts[idx + 1] if idx + 1 < len(starts) else a + s, a + s)
        hi, strs, calls = {}, [], []
        for pc in range(start, end, 4):
            x = struct.unpack_from('>I', data, o + pc - a)[0]
            op, rD, rA, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 18:
                t = x & 0x3FFFFFC
                t = (t - 0x4000000 if t & 0x2000000 else t) + (0 if x & 2 else pc)
                if x & 1 and t in tset:
                    calls.append(t)
                if x & 1:                                 # calls clobber volatile registers
                    for r in range(0, 13):
                        hi.pop(r, None)
                continue
            # hi[] holds the full known value of a register (lis, then addi/ori, mr), so string pools
            # addressed as base + offset (CodeWarrior @stringBase0) resolve too.
            target = None
            if op == 15 and rA == 0:                      # lis
                hi[rD] = (imm << 16) & 0xFFFFFFFF
                continue
            if op == 14 and rA in (2, 13):
                target = ((r13 if rA == 13 else r2) + simm) & 0xFFFFFFFF
                hi[rD] = target
            elif op == 14 and rA in hi:                   # addi
                target = hi[rD] = (hi[rA] + simm) & 0xFFFFFFFF
            elif op == 24 and rD in hi:                   # ori rA, rS, imm
                target = hi[rA] = hi[rD] | imm
            elif op == 31 and (x >> 1) & 0x3FF == 444 and rD == (x >> 11) & 31 and rD in hi:   # mr
                hi[rA] = hi[rD]
            elif 24 <= op <= 29 or op == 31:              # logical immediates / X-form write rA (approx.)
                hi.pop(rA, None)
                if op == 31:
                    hi.pop(rD, None)
            elif op not in (16, 19, 36, 37, 38, 39, 44, 45, 52, 53, 54, 55):   # stores/branches write nothing
                hi.pop(rD, None)
            if target is not None:
                sv = cstr(target)
                if sv:
                    strs.append(sv)
        funcs[start] = {'lits': strs, 'calls': calls}
    names = rm.dsy_names(data, text, starts, br.load_dsy())
    return funcs, names, starts


# ---------------------------------------------------------------- matching
def lis_keep(items):
    """items: (key, value) sorted by key; return indexes of one longest strictly increasing run of value."""
    import bisect
    tails, tidx, prev = [], [], [-1] * len(items)
    for i, (_, v) in enumerate(items):
        k = bisect.bisect_left(tails, v)
        if k == len(tails):
            tails.append(v)
            tidx.append(i)
        else:
            tails[k], tidx[k] = v, i
        prev[i] = tidx[k - 1] if k else -1
    keep, i = set(), tidx[-1] if tidx else -1
    while i >= 0:
        keep.add(i)
        i = prev[i]
    return keep


def main():
    src = load_source()
    wii, sdk, starts = load_wii()
    print(f'source functions {len(src)}, wii functions {len(wii)}, sdk names {len(sdk)}')
    sdk_at = defaultdict(set)
    for nm, addrs in sdk.items():
        for a in addrs:
            sdk_at[a].add(nm)
    by_last, by_unit = defaultdict(list), defaultdict(list)
    for i, f in enumerate(src):
        by_last[f['dem'].split('::')[-1]].append(i)
        by_unit[f['unit']].append(i)
    for v in by_unit.values():
        v.sort(key=lambda i: src[i]['gc'])
    upos = {i: k for v in by_unit.values() for k, i in enumerate(v)}
    widx = {a: k for k, a in enumerate(starts)}
    s2w, w2s, method = {}, {}, {}

    def order_ok(si, wa):
        """wa must sit between the Wii addresses of the nearest matched neighbours of si in its unit."""
        v, k = by_unit[src[si]['unit']], upos[si]
        lo = next((s2w[v[j]] for j in range(k - 1, -1, -1) if v[j] in s2w), None)
        hi = next((s2w[v[j]] for j in range(k + 1, len(v)) if v[j] in s2w), None)
        return (lo is None or lo < wa) and (hi is None or wa < hi)

    def propose(si, wa, how, check=True):
        if si in s2w or wa in w2s or (check and not order_ok(si, wa)):
            return False
        s2w[si], w2s[wa], method[si] = wa, si, how
        return True

    def order_filter():
        dropped = 0
        for v in by_unit.values():
            m = [(src[i]['gc'], s2w[i], i) for i in v if i in s2w]
            keep = lis_keep([(g, w) for g, w, _ in m])
            for k, (_, w, i) in enumerate(m):
                if k not in keep and method[i] != 'sdk':
                    del w2s[w], s2w[i], method[i]
                    dropped += 1
        return dropped

    # seeds: SDK names (Dolphin db) that exist exactly once on both sides
    for nm, addrs in sdk.items():
        c = [i for i in by_last.get(nm, []) if '::' not in src[i]['dem']]
        if len(addrs) == 1 and len(c) == 1:
            propose(c[0], next(iter(addrs)), 'sdk', check=False)

    # seeds: unique strings (votes, mutual best)
    s_by, w_by = defaultdict(set), defaultdict(set)
    for i, f in enumerate(src):
        for s in f['lits']:
            if len(s) >= 4:
                s_by[s].add(i)
    for a, f in wii.items():
        for s in f['lits']:
            w_by[s].add(a)
    votes = Counter()
    for s, ss in s_by.items():
        ws = w_by.get(s)
        if ws and len(ss) == 1 and len(ws) == 1:
            votes[(next(iter(ss)), next(iter(ws)))] += 1
    best_s, best_w = {}, {}
    for (si, wa), v in votes.items():
        if v > best_s.get(si, (0, None))[0]:
            best_s[si] = (v, wa)
        if v > best_w.get(wa, (0, None))[0]:
            best_w[wa] = (v, si)
    for si, (v, wa) in best_s.items():
        if best_w[wa][1] == si and not sdk_at.get(wa):
            propose(si, wa, 'string', check=False)
    print(f'seeds {len(s2w)}, order filter dropped {order_filter()}')

    def resolve(caller, name):
        last = name.split('::')[-1]
        c = by_last.get(last, [])
        if len(c) > 1 and '::' in name:
            c = [i for i in c if src[i]['dem'].endswith(name)] or c
        if len(c) > 1:
            same = [i for i in c if src[i]['unit'] == src[caller]['unit']]
            c = same if len(same) == 1 else c
        return c[0] if len(c) == 1 else None

    def gap_fill():
        added = 0
        for v in by_unit.values():
            m = [k for k, i in enumerate(v) if i in s2w]
            for k0, k1 in zip(m, m[1:]):
                w0, w1 = widx[s2w[v[k0]]], widx[s2w[v[k1]]]
                ds, ws = v[k0 + 1:k1], starts[w0 + 1:w1]
                if ds and len(ds) == len(ws) and not any(a in w2s or sdk_at.get(a) for a in ws):
                    for si, wa in zip(ds, ws):
                        added += propose(si, wa, 'gap', check=False)
        return added

    for rnd in range(30):
        added = 0
        for si, wa in list(s2w.items()):
            if not src[si]['unit'].startswith('game/'):
                continue
            sc = [resolve(si, x) for x in src[si]['calls']]
            sc = [x for x in dict.fromkeys(sc) if x is not None and src[x]['unit'].startswith('game/')]
            wc = [a for a in dict.fromkeys(wii[wa]['calls']) if not sdk_at.get(a) or a in w2s]
            pos_w = {a: k for k, a in enumerate(wc)}
            anchors = sorted((k, pos_w[s2w[x]]) for k, x in enumerate(sc) if x in s2w and s2w[x] in pos_w)
            anchors = [anchors[k] for k in sorted(lis_keep(anchors))] if anchors else []
            anchors = [(-1, -1)] + anchors + [(len(sc), len(wc))]
            for (a0, b0), (a1, b1) in zip(anchors, anchors[1:]):
                gs = [x for x in sc[a0 + 1:a1] if x not in s2w]
                gw = [x for x in wc[b0 + 1:b1] if x not in w2s]
                if len(gs) == 1 and len(gw) == 1:
                    added += propose(gs[0], gw[0], 'call')
        added += gap_fill()
        print(f'round {rnd}: +{added} (total {len(s2w)})')
        if not added:
            break
    print(f'final order filter dropped {order_filter()}')

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['address', 'name', 'demangled', 'unit', 'method', 'gc_address'])
        for si, wa in sorted(s2w.items(), key=lambda kv: kv[1]):
            f = src[si]
            w.writerow([f'0x{wa:08X}', f['sym'], f['dem'], f['unit'], method[si], f'0x{f["gc"]:08X}'])
    m = Counter(method.values())
    game = sum(1 for si in s2w if src[si]['unit'].startswith('game/'))
    print(f'named {len(s2w)} wii functions ({game} game): ' + ', '.join(f'{k} {v}' for k, v in m.items()))
    print(f'wrote {OUT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
