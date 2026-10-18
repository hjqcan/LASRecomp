"""Folder/image mounting through production fs.c; no game assets or GPU required."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_import_game import synthetic_romfs

ROOT = Path(__file__).resolve().parents[1]


class RomfsSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='romfs-source-')
        cls.root = Path(cls.temporary.name)
        cls.exe = cls.root / ('romfs-source.exe' if os.name == 'nt' else 'romfs-source')
        cc = shlex.split(os.environ.get('CC', 'clang'))
        cxx = shlex.split(os.environ.get('CXX', 'clang++'))
        objects = []
        for name, source, compiler, standard in (
                ('test', 'tools/test_romfs_source.c', cc, 'gnu11'),
                ('layout', 'runtime/romfs_build.c', cc, 'gnu11'),
                ('content', 'runtime/ui/title_content.mm', cxx, 'c++17')):
            obj = cls.root / (name + '.o'); objects.append(str(obj))
            language = 'c++' if standard == 'c++17' else 'c'
            subprocess.run(compiler + ['-x', language, '-std=' + standard, '-O2',
                '-D_FILE_OFFSET_BITS=64', '-pthread', '-Iruntime', '-c', source, '-o', str(obj)],
                cwd=ROOT, check=True)
        subprocess.run(cxx + objects + ['-pthread', '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.case = self.root / self.id().split('.')[-1]
        self.folder = self.case / 'romfs'
        self.folder.mkdir(parents=True)

    def files(self, root='Movie'):
        (self.folder / root).mkdir()
        (self.folder / 'Sound').mkdir()
        (self.folder / root / 'a.bin').write_bytes(b'movie-payload')
        (self.folder / 'Sound' / 'b.bin').write_bytes(b'sound-payload')

    def image(self):
        self.folder.with_suffix('.bin').write_bytes(synthetic_romfs(('Movie',)))

    def check(self, title, kind, reason=''):
        result = subprocess.run([str(self.exe), str(self.folder), title, kind, reason],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_acnh_folder_without_image(self):
        self.files(); self.check('acnh', 'folder')

    def test_other_adapter_folder_without_las_marker(self):
        self.files('Data'); self.check('fixture', 'folder')

    def test_las_folder_still_preferred_over_legacy_image(self):
        self.files('region_common'); self.image(); self.check('las', 'folder')

    def test_acnh_image_remains_preferred(self):
        self.files(); self.image(); self.check('acnh', 'image')

    def test_image_backed_partial_folder(self):
        (self.folder / 'Movie').mkdir(); self.image(); self.check('acnh', 'image')

    def test_legacy_las_image_fallback(self):
        self.image(); self.check('las', 'image')

    def test_incomplete_folder_is_rejected(self):
        (self.folder / 'Movie').mkdir()
        (self.folder / 'Movie/a.bin').write_bytes(b'movie-payload')
        self.check('acnh', 'reject', 'incomplete')

    def test_wrong_folder_is_rejected(self):
        self.files('WrongGame'); self.check('acnh', 'reject', 'content rules')


if __name__ == '__main__':
    unittest.main()
