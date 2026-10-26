"""Map the game-address tables of one game version onto another build of the game's main program.

Usage (tools/venv/bin/python):
  version_map.py OLD_GAME NEW_GAME [--from v1_0_0] [--to v1_0_1] [--check]

OLD_GAME / NEW_GAME: folders with the extracted game files (exefs/main, exefs/sdk) of the version the tables exist for
and of the new one; their main build IDs must be the ones runtime/game_addresses/versions.h lists for --from / --to.
Writes runtime/game_addresses/patches_<to>.h and runtime_<to>.h from the <from> tables (same names, same layout, same
comments), or with --check only compares them with the files there. Nothing from the game files is written: only
addresses.

How an address is mapped (anything else is an error, to be looked at by hand):
- code: the functions of both builds (exception-table entries, symbols, relative relocations, call targets) are
  compared with their pc-relative immediates masked and aligned in address order. An address inside a function that
  is identical in both builds moves with that function; one inside a changed function is refused.
- read-only data: kept when the bytes around it are identical in both builds and the segment starts at the same place.
- data / bss: kept when both segments have the same place and size.
- sdk addresses: kept when the sdk is byte-identical. Values outside the modules (limits, sizes) are copied.
Then check the result with test_game_addresses.py --game (patch asserts and hook words on the new build).
"""
import argparse
import bisect
import difflib
import hashlib
import os
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import eh_frame_functions  # noqa: E402
from game_addresses import TABLE_DIR, VERSIONS, load_table, version_for_build_id  # noqa: E402
from nso import Nso, R_AARCH64_RELATIVE  # noqa: E402

MAIN_BASE = 0x7000000000
SDK_BASE = 0x7004000000
RODATA_WINDOW = 32  # bytes on each side of a read-only data address that must match


def sext(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v


def function_starts(n):
    lo, hi = n.text[0], n.text[0] + n.text[1]
    ok = lambda a: lo <= a < hi and a % 4 == 0  # noqa: E731
    s = {a for a in eh_frame_functions(n) if ok(a)}
    for name, info, shndx, value, size in n.syms:
        if shndx and (info & 0xF) == 2 and ok(value):
            s.add(value)
    for off, typ, sym, add in n.relocs:
        if typ == R_AARCH64_RELATIVE and ok(add):
            s.add(add)
    words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[lo:hi]))
    for i, w in enumerate(words):
        if (w & 0xFC000000) == 0x94000000:  # BL
            t = lo + i * 4 + sext(w & 0x3FFFFFF, 26) * 4
            if ok(t):
                s.add(t)
    return sorted(s), words


def pc_relative(w, pc):
    """(mask without the immediate, target) of a pc-relative instruction, else None."""
    if (w & 0x7C000000) == 0x14000000:  # B / BL
        return 0xFC000000, pc + sext(w & 0x3FFFFFF, 26) * 4
    if (w & 0xFF000010) == 0x54000000:  # B.cond
        return 0xFF00001F, pc + sext((w >> 5) & 0x7FFFF, 19) * 4
    if (w & 0x7E000000) == 0x34000000:  # CBZ / CBNZ
        return 0xFF00001F, pc + sext((w >> 5) & 0x7FFFF, 19) * 4
    if (w & 0x7E000000) == 0x36000000:  # TBZ / TBNZ
        return 0xFFF8001F, pc + sext((w >> 5) & 0x3FFF, 14) * 4
    if (w & 0x9F000000) == 0x10000000:  # ADR
        return 0x9F00001F, pc + sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21)
    if (w & 0x9F000000) == 0x90000000:  # ADRP
        return 0x9F00001F, (pc & ~0xFFF) + (sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12)
    if (w & 0x3B000000) == 0x18000000:  # LDR (literal)
        return 0xFF00001F, pc + sext((w >> 5) & 0x7FFFF, 19) * 4
    return None


def signature(words, lo, a, end):
    """A hash of a function's code that ignores where it and what it refers to are (internal branches kept)."""
    out, adrp = [], set()
    for pc in range(a, end, 4):
        w = words[(pc - lo) // 4]
        r = pc_relative(w, pc)
        if r:
            mask, t = r
            if (w & 0x9F000000) == 0x90000000:
                adrp.add(w & 31)
                out.append(w & mask)
            else:
                out.append(w if a <= t < end else w & mask)
            continue
        # the low 12 bits that go with an ADRP (ADD immediate, LDR/STR unsigned offset)
        if ((w & 0x7F800000) == 0x11000000 or (w & 0x3B000000) == 0x39000000) and ((w >> 5) & 31) in adrp:
            out.append(w & 0xFFC003FF)
            continue
        out.append(w)
    return hashlib.sha1(struct.pack(f'<{len(out)}I', *out)).hexdigest()


def functions(n):
    starts, words = function_starts(n)
    lo, hi = n.text[0], n.text[0] + n.text[1]
    return starts, [(min(starts[i + 1] if i + 1 < len(starts) else hi, hi) - a,
                     signature(words, lo, a, starts[i + 1] if i + 1 < len(starts) else hi)) for i, a in enumerate(starts)]


class Mapper:
    def __init__(self, old_dir, new_dir):
        self.a = Nso(str(Path(old_dir) / 'exefs/main'))
        self.b = Nso(str(Path(new_dir) / 'exefs/main'))
        sdk = [Path(d) / 'exefs/sdk' for d in (old_dir, new_dir)]
        self.sdk_same = all(p.exists() for p in sdk) and sdk[0].read_bytes() == sdk[1].read_bytes()
        self.starts_a, sa = functions(self.a)
        self.starts_b, sb = functions(self.b)
        sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False)
        self.same = {}
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                for k in range(i2 - i1):
                    self.same[self.starts_a[i1 + k]] = self.starts_b[j1 + k]
        self.changed = [f for f in self.starts_a if f not in self.same]

    def map(self, value):
        """(new value, how) or (None, why not)."""
        a, b = self.a, self.b
        if SDK_BASE <= value < SDK_BASE + 0x4000000:
            return (value, 'sdk: identical') if self.sdk_same else (None, 'sdk: the sdk differs')
        o = value - MAIN_BASE
        if not 0 <= o < a.size:
            return value, 'not inside main: copied'
        if a.text[0] <= o < a.text[0] + a.text[1]:
            f = self.starts_a[bisect.bisect_right(self.starts_a, o) - 1]
            if f not in self.same:
                return None, f'code: inside the changed function main+{f:#x}'
            d = self.same[f] - f
            return value + d, 'code: identical function' + (f', moved {d:+#x}' if d else '')
        if a.rodata[0] <= o < a.rodata[0] + a.rodata[1]:
            if a.rodata[0] != b.rodata[0]:
                return None, 'rodata: the segment moved'
            lo, hi = max(a.rodata[0], o - RODATA_WINDOW), min(a.rodata[0] + a.rodata[1], o + RODATA_WINDOW)
            if bytes(a.image[lo:hi]) != bytes(b.image[lo:hi]):
                return None, 'rodata: the bytes around it differ'
            return value, 'rodata: identical'
        if (a.data, a.size) == (b.data, b.size) and a.data[0] <= o:
            return value, 'data/bss: same layout'
        return None, 'data/bss: the layout differs'


def map_header(text, values, mapper, label):
    lines, notes = [], []
    for line in text.splitlines():
        m = re.fullmatch(r'(GA_ADDR\(([A-Z][A-Z0-9_]*),\s+)(0x[0-9a-f]+)\)', line)
        if m:
            new, how = mapper.map(values[m[2]])
            if new is None:
                raise SystemExit(f'GA_{m[2]} ({values[m[2]]:#x}) does not map: {how}. Find it by hand.')
            if new != values[m[2]]:
                notes.append(f'  GA_{m[2]:32s} {values[m[2]]:#x} -> {new:#x}  ({how})')
            line = f'{m[1]}{new:#0{len(m[3])}x})'
        elif line.startswith("// Link's Awakening "):
            line = re.sub(r"^// Link's Awakening [^:]+:", f"// Link's Awakening {label}:", line)
        lines.append(line)
    return '\n'.join(lines) + '\n', notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('old_game', type=Path)
    ap.add_argument('new_game', type=Path)
    ap.add_argument('--from', dest='src', default='v1_0_0')
    ap.add_argument('--to', dest='dst', default='v1_0_1')
    ap.add_argument('--check', action='store_true', help='compare with the committed tables, write nothing')
    args = ap.parse_args()
    for key, folder in ((args.src, args.old_game), (args.dst, args.new_game)):
        bid = Nso(str(folder / 'exefs/main')).build_id
        if version_for_build_id(bid) != key:
            raise SystemExit(f'{folder}: main build ID {bid[:16]} is not {key} ({VERSIONS[key][0]})')
    mapper = Mapper(args.old_game, args.new_game)
    print(f'{len(mapper.starts_a)} / {len(mapper.starts_b)} functions, {len(mapper.same)} identical, changed: ' +
          (', '.join(f'main+{f:#x}' for f in mapper.changed) or 'none'))
    stale = []
    for kind in ('patches', 'runtime'):
        src = TABLE_DIR / f'{kind}_{args.src}.h'
        text, notes = map_header(src.read_text(encoding='utf-8'), load_table(src), mapper, VERSIONS[args.dst][0])
        dst = TABLE_DIR / f'{kind}_{args.dst}.h'
        print(f'{dst.name}: {len(load_table(src))} addresses, {len(notes)} moved')
        for n in notes:
            print(n)
        if args.check:
            if not dst.exists() or dst.read_text(encoding='utf-8') != text:
                stale.append(dst.name)
        else:
            dst.write_text(text, encoding='utf-8')
    if stale:
        raise SystemExit(f'Out of date: {", ".join(stale)} (run without --check)')


if __name__ == '__main__':
    main()
