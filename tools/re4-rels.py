#!/usr/bin/env python3
"""Lay out every RE4 REL at a fixed home address and link them there, as OSLink would.

The game loads a REL into a heap buffer and OSLinks it wherever the buffer happens to be. Translated
code needs fixed addresses, so each module gets a home in a reserved MEM2 window (REL_BASE, sorted by
module id, file then .bss). The runtime's OSLink hook copies the loaded file to its home and links it
there; this tool applies the same relocations offline so the translator sees the linked code.

Outputs (derived from the disc, never committed):
  generated/rel/rel_region.bin   linked image of [REL_BASE, end)
  generated/rel/rel_layout.txt   "module <id> <name> <home> <file size> <bss addr> <bss size>" and
                                 "exec <start> <size>" lines for the translator
  generated/RelHomes.h           module table for the runtime
Usage: python -I tools/re4-rels.py
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REL_DIR = ROOT / 'assets' / 're4' / 'files' / 'rel'
OUT = ROOT / 'generated' / 'rel'
HEADER = ROOT / 'generated' / 'RelHomes.h'
REL_BASE = 0x96000000
REL_LIMIT = 0x97000000  # the runtime reserves [REL_BASE, REL_LIMIT) out of the MEM2 arena

R_NONE, R_ADDR32, R_ADDR24, R_ADDR16, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA = 0, 1, 2, 3, 4, 5, 6
R_ADDR14, R_ADDR14_T, R_ADDR14_N, R_REL24, R_REL14, R_REL14_T, R_REL14_N = 7, 8, 9, 10, 11, 12, 13
R_RVL_NONE, R_RVL_SECT, R_RVL_STOP = 201, 202, 203


def align(v, a):
    a = max(a, 1)
    return (v + a - 1) // a * a


class Module:
    def __init__(self, path):
        self.name = path.name
        self.raw = path.read_bytes()
        h = struct.unpack_from('>12I4B3I', self.raw, 0)
        (self.id, _, _, nsec, secoff, _, _, self.version, self.bss_size, _, self.imp_off, self.imp_size,
         self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec, self.prolog, self.epilog,
         self.unresolved) = h
        self.align, self.bss_align = struct.unpack_from('>2I', self.raw, 0x40) if self.version >= 2 else (0, 0)
        self.sections = []  # (file offset, size, executable)
        for i in range(nsec):
            off, size = struct.unpack_from('>2I', self.raw, secoff + 8 * i)
            self.sections.append((off & ~1, size, bool(off & 1)))
        self.imports = [struct.unpack_from('>2I', self.raw, self.imp_off + 8 * i)
                        for i in range(self.imp_size // 8)]

    def place(self, cursor):
        self.home = align(cursor, max(self.align, 0x20))
        self.bss = align(self.home + len(self.raw), max(self.bss_align, 0x20))
        self.end = self.bss + self.bss_size
        bss_secs = [i for i, (off, size, _) in enumerate(self.sections) if off == 0 and size]
        if len(bss_secs) > 1:
            raise SystemExit('%s: %d zero-offset sections, OSLink links one .bss' % (self.name, len(bss_secs)))
        self.base = [self.home + off if off else (self.bss if size else 0)
                     for off, size, _ in self.sections]
        return self.end

    def addr(self, sec, offset):
        return self.base[sec] + offset if sec else 0


def link(mods, image):
    """Apply every module's relocations at its home; returns {branch address: target} far branches."""
    by_id = {m.id: m for m in mods}
    far = {}

    def put32(a, v):
        struct.pack_into('>I', image, a - REL_BASE, v & 0xFFFFFFFF)

    def put16(a, v):
        struct.pack_into('>H', image, a - REL_BASE, v & 0xFFFF)

    def get32(a):
        return struct.unpack_from('>I', image, a - REL_BASE)[0]

    for m in mods:
        image[m.home - REL_BASE:m.home - REL_BASE + len(m.raw)] = m.raw
        for imp_id, rel_off in m.imports:
            src = by_id.get(imp_id) if imp_id else None
            if imp_id and src is None:
                raise SystemExit('%s imports unknown module %d' % (m.name, imp_id))
            p = 0
            cur = rel_off
            while True:
                delta, typ, sec, addend = struct.unpack_from('>HBBI', m.raw, cur)
                cur += 8
                if typ == R_RVL_STOP:
                    break
                if typ == R_RVL_SECT:
                    p = m.base[sec]
                    continue
                p += delta
                if typ in (R_RVL_NONE, R_NONE):
                    continue
                s = addend if imp_id == 0 else src.base[sec] + addend
                s &= 0xFFFFFFFF
                if typ == R_ADDR32:
                    put32(p, s)
                elif typ == R_ADDR24:
                    put32(p, (get32(p) & ~0x03FFFFFC) | (s & 0x03FFFFFC))
                elif typ == R_ADDR16:
                    put16(p, s)
                elif typ == R_ADDR16_LO:
                    put16(p, s)
                elif typ == R_ADDR16_HI:
                    put16(p, s >> 16)
                elif typ == R_ADDR16_HA:
                    put16(p, (s + 0x8000) >> 16)
                elif typ in (R_ADDR14, R_ADDR14_T, R_ADDR14_N):
                    put32(p, (get32(p) & ~0xFFFC) | (s & 0xFFFC))
                elif typ == R_REL24:
                    # Homes are in MEM2, more than 32 MiB from the DOL: such a b/bl cannot encode
                    # its target, so it is recorded as a far branch for the translator instead.
                    if not -0x2000000 <= s - p < 0x2000000:
                        far[p] = s
                    put32(p, (get32(p) & ~0x03FFFFFC) | ((s - p) & 0x03FFFFFC))
                elif typ in (R_REL14, R_REL14_T, R_REL14_N):
                    put32(p, (get32(p) & ~0xFFFC) | ((s - p) & 0xFFFC))
                else:
                    raise SystemExit('%s: relocation type %d at 0x%08X' % (m.name, typ, p))
    return far


def build():
    """The linked REL region: (modules sorted by id, image bytes, far branches)."""
    mods = sorted((Module(p) for p in REL_DIR.glob('*.rel')), key=lambda m: m.id)
    cursor = REL_BASE
    for m in mods:
        cursor = m.place(cursor)
    if cursor > REL_LIMIT:
        raise SystemExit('REL homes end at 0x%08X, past 0x%08X' % (cursor, REL_LIMIT))
    image = bytearray(align(cursor, 0x20) - REL_BASE)
    far = link(mods, image)
    return mods, image, far


def exec_ranges(mods):
    return [(m.base[i], size) for m in mods for i, (off, size, ex) in enumerate(m.sections) if ex and off and size]


def main():
    mods, image, far = build()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'rel_region.bin').write_bytes(bytes(image))
    lines = ['region %08X %08X\n' % (REL_BASE, len(image))]
    lines += ['module %d %s %08X %08X %08X %08X\n' % (m.id, m.name, m.home, len(m.raw), m.bss, m.bss_size)
              for m in mods]
    lines += ['exec %08X %08X\n' % r for r in exec_ranges(mods)]
    lines += ['far %08X %08X\n' % kv for kv in sorted(far.items())]
    (OUT / 'rel_layout.txt').write_text(''.join(lines), encoding='ascii')
    rows = ''.join('    {%3d, 0x%08Xu, 0x%06Xu, 0x%08Xu, 0x%06Xu},  // %s\n'
                   % (m.id, m.home, len(m.raw), m.bss, m.bss_size, m.name) for m in mods)
    HEADER.write_text(
        '// Generated by tools/re4-rels.py from the disc RELs: each module\'s fixed home (see that tool).\n'
        '#pragma once\n#include <cstdint>\n\n'
        'struct RelHome { uint32_t id, home, fileSize, bss, bssSize; };\n'
        'constexpr uint32_t kRelRegionBase = 0x%08Xu;\nconstexpr uint32_t kRelRegionLimit = 0x%08Xu;\n'
        'constexpr RelHome kRelHomes[] = {\n%s};\n' % (REL_BASE, REL_LIMIT, rows), encoding='ascii')
    code = sum(s for _, s in exec_ranges(mods))
    print('RELs: %d modules, region 0x%08X-0x%08X, %d KiB code, %d far branches -> %s'
          % (len(mods), REL_BASE, REL_BASE + len(image), code >> 10, len(far), OUT.relative_to(ROOT)))


if __name__ == '__main__':
    main()
