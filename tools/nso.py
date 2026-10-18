"""NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info."""
import struct
import sys
import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_JMPREL, DT_RELACOUNT = 10, 23, 0x6FFFFFF9
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
DT_GNU_HASH = 0x6ffffef5
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


def gnu_hash_symbol_count(image, table, max_symbols):
    """Count ELF64 symbols from bounded GNU buckets and the final chain."""
    if table < 0 or table % 4 or table > len(image) - 16 or max_symbols < 0:
        raise ValueError('Invalid GNU hash header range')
    buckets, first, words, _ = struct.unpack_from('<4I', image, table)
    if not buckets or not words or words & (words - 1) or first > max_symbols:
        raise ValueError('Invalid GNU hash header')
    bucket_at = table + 16 + words * 8
    chain_at = bucket_at + buckets * 4
    if chain_at > len(image):
        raise ValueError('GNU hash buckets exceed the module')
    last = 0
    for i in range(buckets):
        index, = struct.unpack_from('<I', image, bucket_at + i * 4)
        if index and not first <= index < max_symbols:
            raise ValueError('GNU hash bucket exceeds the symbol table')
        last = max(last, index)
    if not last:
        return first
    for index in range(last, max_symbols):
        at = chain_at + (index - first) * 4
        if at > len(image) - 4:
            raise ValueError('GNU hash chain exceeds the module')
        if struct.unpack_from('<I', image, at)[0] & 1:
            return index + 1
    raise ValueError('Unterminated GNU hash chain')


def relr_relocations(image, table, size, entry_size):
    """Expand ELF64 packed relative offsets without modifying their addends."""
    if entry_size != 8 or table % 8 or size % 8 or not 0 <= table <= len(image) or not 0 <= size <= len(image) - table:
        raise ValueError('Invalid RELR table range or entry size')
    sites, cursor = [], None
    for p in range(table, table + size, 8):
        entry, = struct.unpack_from('<Q', image, p)
        if not entry & 1:
            sites.append(entry)
            cursor = entry + 8
        else:
            if cursor is None or cursor > (1 << 64) - 1 - 63 * 8:
                raise ValueError('Invalid RELR bitmap without a bounded address')
            sites.extend(cursor + (bit - 1) * 8 for bit in range(1, 64) if entry >> bit & 1)
            cursor += 63 * 8
    result = []
    for at in sites:
        if at > len(image) - 8 or (at < table + size and table < at + 8):
            raise ValueError('Invalid RELR relocation target')
        addend, = struct.unpack_from('<q', image, at)
        result.append((at, R_AARCH64_RELATIVE, 0, addend))
    return result


class Nso:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'NSO0', path
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        self.build_id = raw[0x40:0x60].rstrip(b'\0').hex()
        self.build_id_full = raw[0x40:0x60].hex()
        segs = []
        for i in range(3):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
            data = raw[file_off:file_off + comp_size]
            if flags & (1 << i):
                data = lz4.block.decompress(data, uncompressed_size=dec_size)
            segs.append((mem_off, data))
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        self.text = (segs[0][0], len(segs[0][1]))
        self.rodata = (segs[1][0], len(segs[1][1]))
        self.data = (segs[2][0], len(segs[2][1]))
        end = segs[2][0] + len(segs[2][1]) + self.bss_size
        self.size = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.size)
        for off, d in segs:
            img[off:off + len(d)] = d
        self.image = img
        self._parse_mod0()

    def u32(self, off): return struct.unpack_from('<I', self.image, off)[0]
    def u64(self, off): return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(4)
        assert self.image[mod0:mod0 + 4] == b'MOD0'
        rel = lambda i: mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0]
        self.dynamic, self.bss_start, self.bss_end = rel(0), rel(1), rel(2)
        self.eh_start, self.eh_end, self.module_obj = rel(3), rel(4), rel(5)
        dyn = {}
        needed = []
        off = self.dynamic
        while True:
            tag, val = self.u64(off), self.u64(off + 8)
            off += 16
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        strtab = dyn.get(DT_STRTAB, 0)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + n) for n in needed]
        # Preserve SysV nchain when present; newer SDKs can use only GNU hash.
        self.syms = []
        if DT_SYMTAB in dyn and (DT_HASH in dyn or DT_GNU_HASH in dyn):
            st = dyn[DT_SYMTAB]
            if DT_HASH in dyn:
                nchain = self.u32(dyn[DT_HASH] + 4)
            else:
                if st > len(self.image) or dyn.get(11, 24) != 24:  # DT_SYMENT
                    raise ValueError('Invalid ELF64 symbol table')
                nchain = gnu_hash_symbol_count(self.image, dyn[DT_GNU_HASH], (len(self.image) - st) // 24)
            for i in range(nchain):
                o = st + i * 24
                name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
                value, size = struct.unpack_from('<QQ', self.image, o + 8)
                self.syms.append((self.cstr(strtab + name), info, shndx, value, size))
        self.relocs = []
        relr_tags = (DT_RELR, DT_RELRSZ, DT_RELRENT)
        if any(tag in dyn for tag in relr_tags):
            if not all(tag in dyn for tag in relr_tags):
                raise ValueError('Incomplete RELR dynamic tags')
            self.relocs.extend(relr_relocations(self.image, *(dyn[tag] for tag in relr_tags)))
        for base_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if base_tag in dyn:
                b, n = dyn[base_tag], dyn[size_tag] // 24
                for i in range(n):
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, b + i * 24)
                    self.relocs.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))

    def cstr(self, off):
        end = self.image.index(b'\0', off)
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]


if __name__ == '__main__':
    from collections import Counter
    for p in sys.argv[1:]:
        n = Nso(p)
        print(f'== {p}  build {n.build_id[:16]}  image 0x{n.size:x}')
        print(f'  text 0x{n.text[0]:x}+0x{n.text[1]:x}  ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  '
              f'data 0x{n.data[0]:x}+0x{n.data[1]:x}  bss 0x{n.bss_size:x}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        print(f'  relocs {dict(Counter(r[1] for r in n.relocs))}')
