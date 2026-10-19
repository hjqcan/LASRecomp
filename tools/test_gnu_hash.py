"""GNU-only NSO symbol discovery and bounded native table decoding."""
from pathlib import Path
import os
import shlex
import struct
import subprocess
import tempfile
import unittest

from nso import Nso

ROOT = Path(__file__).resolve().parents[1]
GNU_HASH = 0x6ffffef5


def hash_image(buckets, chains, first=1, words=1, size=1024, table=0x80):
    data = bytearray(size)
    struct.pack_into('<4I', data, table, len(buckets), first, words, 5)
    start = table + 16 + words * 8
    struct.pack_into('<' + 'I' * len(buckets), data, start, *buckets)
    struct.pack_into('<' + 'I' * len(chains), data, start + len(buckets)*4, *chains)
    return data


def synthetic_nso(gnu=True, sysv=False):
    n = Nso.__new__(Nso)
    n.image = bytearray(4096)
    struct.pack_into('<I', n.image, 4, 0x20)
    n.image[0x20:0x24] = b'MOD0'
    struct.pack_into('<6i', n.image, 0x24, 0xe0, 0, 0, 0, 0, 0)
    tags = [(6, 0x300), (5, 0x400), (10, 32), (11, 24)]
    if gnu: tags.append((GNU_HASH, 0x200))
    if sysv: tags.append((4, 0x280))
    tags.append((0, 0))
    for i, tag in enumerate(tags): struct.pack_into('<QQ', n.image, 0x100+i*16, *tag)
    struct.pack_into('<4I', n.image, 0x200, 2, 1, 1, 5)
    struct.pack_into('<2I', n.image, 0x218, 0, 1)
    struct.pack_into('<2I', n.image, 0x220, 0x100, 0x103)
    struct.pack_into('<2I', n.image, 0x280, 1, 3)
    n.image[0x400:0x40c] = b'\0foo\0needed\0'
    struct.pack_into('<IBBHQQ', n.image, 0x318, 1, 0x12, 0, 1, 0x40, 4)
    struct.pack_into('<IBBHQQ', n.image, 0x330, 5, 0x12, 0, 0, 0, 0)
    return n


class GnuHashTests(unittest.TestCase):
    def test_gnu_only_symbols_are_available_to_generator(self):
        n = synthetic_nso()
        n._parse_mod0()
        self.assertEqual([s[0] for s in n.exports()], ['foo'])
        self.assertEqual([s[0] for s in n.imports()], ['needed'])
        self.assertEqual(len(n.syms), 3)

    def test_sysv_count_remains_authoritative_when_both_exist(self):
        n = synthetic_nso(gnu=True, sysv=True)
        struct.pack_into('<4I', n.image, 0x200, 0, 0, 0, 0)
        n._parse_mod0()
        self.assertEqual(len(n.syms), 3)

    def test_native_and_python_tables(self):
        from nso import gnu_hash_symbol_count
        cases = []
        def add(data, table=0x80, maximum=32, expected=None):
            if expected is None:
                with self.assertRaises(ValueError): gnu_hash_symbol_count(data, table, maximum)
            else:
                self.assertEqual(gnu_hash_symbol_count(data, table, maximum), expected)
            cases.append((data, table, maximum, expected))
        add(hash_image([1], [0x11]), expected=2)
        add(hash_image([3, 0, 1], [0x10, 0x11, 0x20, 0x22, 0x23]), expected=6)
        add(hash_image([0, 0], [], first=4), expected=4)
        add(hash_image([0], [], first=0), maximum=0, expected=0)
        add(hash_image([2], [0x11], first=2), maximum=3, expected=3)
        add(hash_image([1], [0x10], size=0xa0), maximum=32)  # unterminated chain
        add(hash_image([1], [0x10, 0x11]), maximum=2)
        add(hash_image([1], [0x11]), table=0x81)
        add(hash_image([1], [0x11]), table=1012)
        add(hash_image([1], [0x11]), maximum=1)
        add(hash_image([1], [0x11], first=2))
        add(hash_image([0], [], first=33))
        for header in ((0,1,1,5), (1,1,0,5), (1,1,3,5), (0xffffffff,1,1,5), (1,1,0x80000000,5)):
            data = hash_image([1], [0x11])
            struct.pack_into('<4I', data, 0x80, *header)
            add(data)
        # The native helper must reject malformed tables without changing its output.
        chunks = []
        for data, table, maximum, expected in cases:
            values = ','.join(str(b) for b in data)
            chunks.append('{ const uint8_t image[]={' + values + '}; uint64_t count=999;\n'
                          f'bool ok=gnu_hash_symbol_count(image,sizeof image,{table}ull,{maximum}ull,&count);\n'
                          + ('assert(!ok && count==999);' if expected is None else f'assert(ok && count=={expected});') + '}')
        source = '#include <assert.h>\n#include "elf_hash.h"\nint main(void) {\n' + '\n'.join(chunks) + '\nreturn 0; }\n'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            file = root/'hash.c'; exe = root/('hash.exe' if os.name == 'nt' else 'hash')
            file.write_text(source)
            cc = shlex.split(os.environ.get('CC', 'clang'))
            subprocess.run(cc + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                '-I'+str(ROOT/'runtime'), str(file), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__': unittest.main()
