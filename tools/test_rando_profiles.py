"""Asset-free native patch/profile checks; optional local executable preimage validation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import rando_profiles as profiles
from game_versions import BASE_ID, UPDATE_ID

ROOT = Path(__file__).resolve().parent.parent
CATALOG = json.loads((ROOT/'tools/rando/patch_variants.json').read_text())
CODE = {int(a, 16): [int(w, 16) for w in words] for a, words in CATALOG['code'].items()}
HARNESS = r'''
#include "rando_profile.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    if(argc!=4)return 9;
    GameLayout version=!strcmp(argv[2],"1.0.0")?GAME_100:GAME_110;
    if(!strcmp(argv[1],"compatible"))return rando_seed_compatible(argv[3],version)?0:2;
    RandoPatchSet out;char why[256];memset(&out,0x55,sizeof out);
    if(!rando_patch_read(argv[3],version,&out,why,sizeof why)) {
        if(out.code_count||out.data_count)return 8;
        puts(why);return 2;
    }
    printf("%d %d\n",out.code_count,out.data_count);return 0;
}
'''


class RandoProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name).resolve()
        source = cls.root/'check.c'; source.write_text(HARNESS)
        cls.exe = cls.root/('check.exe' if os.name == 'nt' else 'check')
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=c11', '-Wall', '-Wextra',
                       '-I'+str(ROOT/'runtime'), str(source), str(ROOT/'runtime/rando_profile.c'),
                       '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, text, version='1.1.0', success=True):
        path = self.root/'fixture.pchtxt'; path.write_text(text)
        result = subprocess.run([str(self.exe), 'parse', version, str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if success else 2, result.stdout+result.stderr)
        return result.stdout

    def content(self, version='1.1.0', body=None):
        bid = BASE_ID if version == '1.0.0' else UPDATE_ID
        if body is None:
            body = '\n'.join(f'{a:08X} {words[0].to_bytes(4,"little").hex()}' for a, words in CODE.items())
            body += '\n016FAE60 03\n016CBD73 "NoTiltShift"\n'
        return f'@nsobid-{bid.upper()}\n@flag offset_shift 0x100\n@enabled\n// LAS generator comment\n{body}\n'

    def test_all_sites_both_versions(self):
        for version in ('1.0.0', '1.1.0'):
            self.assertEqual(self.invoke(self.content(version), version).strip(), '42 2')

    def test_all_alternative_words(self):
        for address, words in CODE.items():
            for word in words:
                self.invoke(self.content(body=f'{address:08x} {word.to_bytes(4,"little").hex()}'))

    def test_header_catalog_matches(self):
        header = (ROOT/'runtime/rando_patch_words.h').read_text()
        found = [(int(a, 16), int(w, 16)) for a,w in re.findall(r'\{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u\}', header)]
        self.assertEqual(found, [(a,w) for a,words in CODE.items() for w in words])

    def test_wrong_build_and_missing_header(self):
        self.invoke(self.content('1.0.0'), success=False)
        self.invoke(self.content().split('\n',1)[1], success=False)

    def test_bad_code_data_and_encoding(self):
        for body in ('00000000 00000000', '006A62F8 00000000', '016FAE60 04', '016FAE60 0300',
                     '016CBD73 "arbitrary"', '01CBB220 00', '006A62F8 xx000000', '006A62F9 40020034',
                     '006A62F8 000', 'FFFFFFFF 00'):
            self.invoke(self.content(body=body), success=False)

    def test_duplicate_and_oversized(self):
        line = '006A62F8 40020034'
        self.invoke(self.content(body=line+'\n'+line), success=False)
        self.invoke(self.content()+'\n'+('x'*2000), success=False)
        self.invoke(self.content().replace('@enabled', '@arbitrary'), success=False)

    def test_legacy_and_versioned_seeds(self):
        seed = self.root/'seed'; seed.mkdir(exist_ok=True)
        marker = seed/'game-version.txt'
        def check(version, expected):
            result = subprocess.run([str(self.exe), 'compatible', version, str(seed)])
            self.assertEqual(result.returncode, 0 if expected else 2)
        check('1.0.0', True); check('1.1.0', False)
        marker.write_bytes(b'LASRANDO1\r\n1.1.0\r\n')
        check('1.0.0', False); check('1.1.0', True)
        marker.write_text('LASRANDO1\n1.2.0\n')
        check('1.0.0', False); check('1.1.0', False)

    def test_preimage_guard(self):
        image = bytearray(0x16faf00)
        digest = hashlib.sha256(b''.join(struct.pack('<I',a)+image[a:a+4] for a in sorted(CODE))).hexdigest()
        data = {a:hashlib.sha256(image[a:a+64]).hexdigest() for a in profiles.DATA_DIGESTS}
        with patch.object(profiles, 'CODE_DIGEST', digest), patch.object(profiles, 'DATA_DIGESTS', data):
            for bid in (BASE_ID, UPDATE_ID):
                self.assertEqual(profiles.validate_catalog(image, bid, CODE), CODE)
            image[min(CODE)] ^= 1
            with self.assertRaisesRegex(ValueError, 'preimage'):
                profiles.validate_catalog(image, UPDATE_ID, CODE)
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            profiles.validate_catalog(image, '00'*20, CODE)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--main-100', type=Path)
    parser.add_argument('--main-110', type=Path)
    args, remaining = parser.parse_known_args()
    for path in (args.main_100, args.main_110):
        if path:
            from nso import Nso
            nso = Nso(path)
            profiles.validate_catalog(nso.image, nso.build_id, CODE)
            print(f'Local executable {nso.build_id}: 42 code preimages and both data preimages verified')
    unittest.main(argv=[__file__]+remaining)
