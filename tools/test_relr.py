"""Synthetic ELF64 RELR tables: generator discovery and native relocation parity."""
from pathlib import Path
import os
import random
import shlex
import struct
import subprocess
import tempfile
import unittest

from nso import Nso, relr_relocations, R_AARCH64_RELATIVE

ROOT = Path(__file__).resolve().parents[1]


def image(entries, size=4096, table=0x100):
    data = bytearray(size)
    for i, value in enumerate(entries): struct.pack_into('<Q', data, table+i*8, value)
    return data


class RelrTests(unittest.TestCase):
    def test_addresses_bitmaps_and_implicit_addends(self):
        data = image([0x400, 3 | (1<<63), 5, 0x802])
        sites = [0x400,0x408,0x5f8,0x608,0x802]
        for i, site in enumerate(sites): struct.pack_into('<q', data, site, i-2)
        self.assertEqual(relr_relocations(data,0x100,32,8),
                         [(site,R_AARCH64_RELATIVE,0,i-2) for i,site in enumerate(sites)])
        self.assertEqual(relr_relocations(data,0x100,0,8),[])

    def test_invalid_ranges_and_bitmap_order(self):
        cases=[([3],0x100,8,8),([0x400],0x100,8,4),([0x400],0x100,7,8),
               ([0xffa],0x100,8,8),([0x100],0x100,8,8),([0xff8,3],0x100,16,8),
               ([0xfffffffffffffff8,3],0x100,16,8),([0x400],0x102,8,8)]
        for entries,table,size,entry in cases:
            with self.subTest(entries=entries,table=table),self.assertRaises(ValueError):
                relr_relocations(image(entries),table,size,entry)

    def test_nso_dynamic_tags_include_relr_and_rela(self):
        nso=Nso.__new__(Nso); nso.image=bytearray(4096)
        struct.pack_into('<I',nso.image,4,0x20); nso.image[0x20:0x24]=b'MOD0'
        struct.pack_into('<6i',nso.image,0x24,0xe0,0,0,0,0,0)
        tags=[(36,0x200),(35,8),(37,8),(7,0x240),(8,24),(0,0)]
        for i,tag in enumerate(tags):struct.pack_into('<QQ',nso.image,0x100+i*16,*tag)
        struct.pack_into('<Q',nso.image,0x200,0x400);struct.pack_into('<Q',nso.image,0x400,0x88)
        struct.pack_into('<QQq',nso.image,0x240,0x500,R_AARCH64_RELATIVE,0x99)
        nso._parse_mod0()
        self.assertEqual(nso.relocs,[(0x400,R_AARCH64_RELATIVE,0,0x88),(0x500,R_AARCH64_RELATIVE,0,0x99)])
        struct.pack_into('<Q',nso.image,0x120,0) # omit DT_RELRENT and terminate the table
        with self.assertRaisesRegex(ValueError,'Incomplete RELR'):nso._parse_mod0()

    def test_native_tables_and_rejection_before_mutation(self):
        rng=random.Random(20261017); cases=[]
        for trial in range(12):
            entries,sites=[],[]
            for group in range(6):
                start=0x400+group*512;mask=rng.getrandbits(63)
                entries.extend((start,(mask<<1)|1));sites.append(start)
                sites.extend(start+8+bit*8 for bit in range(63) if mask>>bit&1)
            data=image(entries)
            for at in sites:struct.pack_into('<Q',data,at,at*7)
            self.assertEqual([r[0] for r in relr_relocations(data,0x100,len(entries)*8,8)],sites)
            cases.append('{\nuint64_t entries[]={'+','.join(hex(x)+'ull' for x in entries)+'};\n'
                         'memset(img,0,sizeof img);memcpy(img+0x100,entries,sizeof entries);\n'+
                         ''.join(f'put(img,{at},{at*7});\n' for at in sites)+
                         'assert(relr_apply(img,sizeof img,BASE,0x100,sizeof entries,8));\n'+
                         ''.join(f'assert(get(img,{at})==BASE+{at*7});\n' for at in sites)+'}\n')
        source=r'''
#include <assert.h>
#include "relr.h"
#define BASE UINT64_C(0x7000000000)
static void put(uint8_t *p,size_t at,uint64_t v) { memcpy(p+at,&v,8); }
static uint64_t get(uint8_t *p,size_t at) { uint64_t v;memcpy(&v,p+at,8);return v; }
int main(void) {
    uint8_t img[4096],before[4096];
''' + ''.join(cases) + r'''
    memset(img,0,sizeof img); put(img,0x100,0x802); put(img,0x802,UINT64_MAX-7);
    assert(relr_apply(img,sizeof img,BASE,0x100,8,8)); assert(get(img,0x802)==BASE-8);
    // A valid first entry followed by a bad target must not modify the image.
    const uint64_t invalid[][2]={{0x400,0xffa},{0xff8,3},{0x400,0x100},{3,0},{UINT64_MAX-7,3}};
    for (unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        memset(img,0,sizeof img);memcpy(img+0x100,invalid[i],16);put(img,0x400,22);
        memcpy(before,img,sizeof img);
        assert(!relr_apply(img,sizeof img,BASE,0x100,16,8));assert(!memcmp(img,before,sizeof img));
    }
    assert(!relr_apply(img,sizeof img,BASE,4096,8,8));
    assert(!relr_apply(img,sizeof img,BASE,0x100,7,8));
    assert(!relr_apply(img,sizeof img,BASE,0x100,8,16));
    assert(relr_apply(img,sizeof img,BASE,4096,0,8));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);file=root/'relr.c';exe=root/('relr.exe' if os.name=='nt' else 'relr')
            file.write_text(source)
            cc=shlex.split(os.environ.get('CC','clang'))
            subprocess.run(cc+['-std=gnu11','-O2','-I'+str(ROOT/'runtime'),str(file),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=20)


if __name__=='__main__':unittest.main()
