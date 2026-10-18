"""Bounded patch parsing, all-or-nothing preparation and native mode separation."""
from pathlib import Path
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

import acnh_speedfix as fix

ROOT = Path(__file__).resolve().parents[1]


class PatchTests(unittest.TestCase):
    def test_bundle(self):
        manifest, _ = fix.load_bundle()
        self.assertEqual(manifest['modules']['main']['words'], 6303)
        self.assertEqual(sum(len(x.get('words', [])) for x in manifest['resources']), 107)
        self.assertEqual((ROOT / 'runtime/acnh_speedfix_data.inc').read_text(), fix.render_native_manifest())

    def test_pchtxt_memory_offsets(self):
        text = '@nsobid-AB\n@flag offset_shift 0x100\n@enabled\n00000100 11223344\n@stop\n'
        self.assertEqual(fix.read_pchtxt(text, 'ab'.ljust(64, '0')), {256: bytes.fromhex('11223344')})
        for bad in (text.replace('AB', 'AC'), text.replace('@enabled', '@disabled'),
                    text.replace('@stop', '00000100 55667788\n@stop'),
                    text + '00000104 00000000\n', text.replace('00000100', '00000101')):
            with self.assertRaises(ValueError): fix.read_pchtxt(bad, 'ab'.ljust(64, '0'))

    def test_ips_bounds_and_repeat(self):
        patch = b'PATCH\0\0\1\0\2ab\0\0\5\0\0\0\3zEOF'
        self.assertEqual(fix.apply_ips(patch, b'0123456789'), b'0ab34zzz89')
        for bad in (patch[:-1], patch + b'x', patch.replace(b'\0\0\5', b'\0\0\xff'),
                    b'PATCH\0\0\1\0\2ab\0\0\2\0\1zEOF'):
            with self.assertRaises(ValueError): fix.apply_ips(bad, b'0123456789')

    def test_resource_guards(self):
        original = b'01234567'
        expected = b'abcd4567'
        spec = dict(path='Bcsv/test', size=8, source_sha256=hashlib.sha256(original).hexdigest(),
                    result_sha256=hashlib.sha256(expected).hexdigest(), words=[[0, b'0123'.hex(), b'abcd'.hex()]])
        self.assertEqual(fix.patch_resource(original, spec), expected)
        for value in (b'01234568', b'0123456'):
            with self.assertRaises(ValueError): fix.patch_resource(value, spec)
        spec['words'][0][1] = b'xxxx'.hex()
        with self.assertRaises(ValueError): fix.patch_resource(original, spec)

    def test_prepared_directory_retains_real_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); source=root/'source';target=root/'target'
            (source/'Movie/intro').mkdir(parents=True);(source/'Pack').mkdir()
            (source/'Movie/intro/a.bin').write_bytes(b'unchanged movie')
            (source/'Pack/StaticParam.pack').write_bytes(b'normal parameters')
            target.mkdir()
            fix.copy_romfs(source,target,{'Pack/StaticParam.pack'})
            self.assertEqual((target/'Movie/intro/a.bin').read_bytes(),b'unchanged movie')
            self.assertFalse((target/'Pack/StaticParam.pack').exists())
            (target/'Pack/StaticParam.pack').write_bytes(b'patched parameters')
            self.assertEqual((source/'Pack/StaticParam.pack').read_bytes(),b'normal parameters')

    def test_nso_text_rodata_and_data(self):
        header = bytearray(256)
        header[:4] = b'NSO0'
        header[0x40:0x60] = b'\xab' * 32
        segments = [(0, bytearray(range(32))), (0x1000, bytearray(range(32, 64))),
                    (0x2000, bytearray(range(64, 96)))]
        raw = fix.write_nso(header, segments)
        source = hashlib.sha256(b''.join(struct.pack('<II', a, len(d)) + d for a, d in segments)).hexdigest()
        patch = '@nsobid-' + 'AB' * 32 + '\n@enabled\n00000004 01020304\n00001008 05060708\n0000200C 090A0B0C\n@stop\n'
        expected = [(a, bytearray(d)) for a, d in segments]
        for index, at, value in ((0, 4, b'\1\2\3\4'), (1, 8, b'\5\6\7\10'), (2, 12, b'\11\12\13\14')):
            expected[index][1][at:at+4] = value
        spec = dict(build_id='ab'*32, source_sha256=source, words=3,
                    result_sha256=hashlib.sha256(fix.write_nso(header, expected)).hexdigest())
        result = fix.patch_nso(raw, spec, patch)
        self.assertEqual(fix.read_nso(result), expected)
        for index, (_, data) in enumerate(expected):
            self.assertEqual(result[0xa0+index*32:0xc0+index*32], hashlib.sha256(data).digest())
        self.assertEqual(fix.read_nso(raw), segments)
        with self.assertRaises(ValueError): fix.patch_nso(result, spec, patch)
        with self.assertRaises(ValueError): fix.patch_nso(raw, spec, patch.replace('0000200C', '00003000'))


NATIVE = r'''
#include "acnh_speedfix.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
static TitleProfile base;
extern "C" const TitleProfile *title_profile_current(void) { return &base; }
int main(int argc, char **argv) {
    namespace fs=std::filesystem;
    assert(argc==3 || argc==4);
    fs::path root=fs::u8path(argv[1]); fs::create_directories(root/"exefs");
    base.key="acnh";base.version="3.0.1";base.code_identity="normal-code-identity";
    char why[512]={},out[2048];
    auto normal=root.u8string();
    assert(acnh60_profile(&base,normal.c_str(),why,sizeof why)==&base);
    assert(acnh60_boot(normal.c_str(),why,sizeof why) && !acnh60_active());
    fs::path mod=root/".acnh60";fs::create_directories(mod/"exefs");
    auto name=mod.u8string();
    assert(!acnh60_profile(&base,name.c_str(),why,sizeof why));
    assert(!acnh60_validate(name.c_str(),why,sizeof why));
    {std::ofstream f(mod/".acnh-speedfix60",std::ios::binary);f<<"LASACNH60-1\n"<<argv[2]<<"\n";}
    const TitleProfile *variant=acnh60_profile(&base,name.c_str(),why,sizeof why);
    assert(variant && variant!=&base && strcmp(variant->code_identity,base.code_identity));
    assert(!strcmp(acnh60_generator_identity(variant),base.code_identity));
    assert(acnh60_base_path(name.c_str(),out,sizeof out) && fs::u8path(out)==root);
    assert(!acnh60_validate(name.c_str(),why,sizeof why)); // incomplete bundle never enables the mode
    assert(!acnh60_boot(name.c_str(),why,sizeof why) && !acnh60_active());
    base.version="1.0.0";
    assert(!acnh60_profile(&base,name.c_str(),why,sizeof why));
    base.version="3.0.1";
    {std::ofstream f(mod/".acnh-speedfix60");f<<"LASACNH60-1\ntruncated\n";}
    assert(!acnh60_profile(&base,name.c_str(),why,sizeof why));
    if(argc==4) {
        assert(acnh60_validate(argv[3],why,sizeof why));
        assert(acnh60_boot(argv[3],why,sizeof why) && acnh60_active());
        assert(acnh60_romfs_override("/Pack/StaticParam.pack",out,sizeof out));
        assert(!acnh60_romfs_override("Pack/Other.pack",out,sizeof out));
        acnh60_clear();assert(!acnh60_active());
    }
    puts("Native SpeedFix mode identity, incomplete-bundle rejection and reset: PASS");
}
'''


def native_test():
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp).resolve()
        source = path / 'test.cpp'; source.write_text(NATIVE)
        executable = path / ('test.exe' if os.name == 'nt' else 'test')
        compiler = shlex.split(os.environ.get('CXX', 'clang++'))
        command = compiler + ['-x', 'c++', '-std=c++17', '-Iruntime', str(source),
                              'runtime/ui/acnh_speedfix.mm', '-o', str(executable)]
        subprocess.run(command, cwd=ROOT, check=True)
        args = [str(executable), str(path / 'game'), fix.load_bundle()[1]]
        if os.environ.get('ACNH60_TEST_PREPARED'):
            args.append(os.environ['ACNH60_TEST_PREPARED'])
        subprocess.run(args, check=True)


if __name__ == '__main__':
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(PatchTests))
    if not result.wasSuccessful(): raise SystemExit(1)
    native_test()
