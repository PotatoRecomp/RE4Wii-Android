#!/usr/bin/env python3
"""Shared helpers for the runtime's addressed hooks: Dolphin totaldb.dsy signatures (checksum + size)
and the inventory of hook macros in runtime/src. Used by re4_remap.py and re4_gcnames.py."""
import re
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DSY = ROOT / 'refs' / 'NWiiRecomp' / 'third_party' / 'dolphin-sigdb' / 'totaldb.dsy'
RT = ROOT / 'runtime' / 'src'

HOOK = re.compile(
    r"\b(PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?|REGISTER_TRANSLATED_FUNCTION|"
    r"MKW_KNOWN_TYPED_NATIVE|MKW_KNOWN_NATIVE_CPU_CALL|GX_FATAL_STUB)\s*\(\s*((?:0x)?)([0-9A-Fa-f]{8})(u?)")


def checksum_step(s, opcode):
    """Port Dolphin HashSignatureDB::ComputeCodeChecksum (one instruction)."""
    op = opcode & 0xFC000000
    op2 = op3 = 0
    aux = op >> 26
    if aux == 4:
        op2 = opcode & 0x3F
        if op2 in (0, 8, 16, 21, 22):
            op3 = opcode & 0x7C0
    elif 7 <= aux <= 15:
        op2 = opcode & 0x03FF0000
    elif aux in (19, 31, 63):
        op2 = opcode & 0x7FF
    elif aux == 59:
        op2 = opcode & 0x3F
        if op2 < 16:
            op3 = opcode & 0x7C0
    elif 32 <= aux < 56:
        op2 = opcode & 0x03FF0000
    s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)
    return s ^ (op | op2 | op3)


def load_dsy():
    b = DSY.read_bytes()
    n = struct.unpack_from('<I', b, 0)[0]
    db = defaultdict(set)
    for i in range(n):
        ck, size, name = struct.unpack_from('<II128s', b, 4 + 136 * i)
        db[(ck, size)].add(name.split(b'\0')[0].decode('ascii', 'replace').split('\t')[0].strip())
    return db


def inventory():
    rows = []
    for f in sorted(RT.rglob('*')):
        if f.suffix not in ('.cpp', '.h', '.hpp', '.inc'):
            continue
        for i, line in enumerate(f.read_text(encoding='utf-8', errors='surrogateescape').splitlines(), 1):
            for m in HOOK.finditer(line):
                rows.append((f, i, m.group(1), int(m.group(3), 16)))
    return rows


def statement_end(lines, start, col):
    depth, started = 0, False
    for i in range(start, len(lines)):
        for ch in (lines[i][col:] if i == start else lines[i]):
            if ch == '(':
                depth += 1
                started = True
            elif ch == ')':
                depth -= 1
                if started and depth == 0:
                    return i
    return start
