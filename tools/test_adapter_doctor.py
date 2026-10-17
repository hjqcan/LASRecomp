"""The doctor follows managed publication state, while keeping candidates explicit."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from adapter import installation_root
from game_catalog import GAMES


class DoctorTests(unittest.TestCase):
    def test_published_and_pending_versions_are_distinct(self):
        with tempfile.TemporaryDirectory() as temp:
            slot = Path(temp); meta = slot / '.installations'; meta.mkdir()
            (meta / 'owner').write_bytes(b'LASINSTALL2\n')
            for index, release in enumerate(GAMES[0].releases, 1):
                content = meta / f'build-{index}' / 'content'; (content / 'exefs').mkdir(parents=True)
                for module in release.modules:
                    header = bytearray(0x60); header[:4] = b'NSO0'; header[0x40:0x60] = bytes.fromhex(module.build_id)
                    (content / 'exefs' / module.name).write_bytes(header)
                (content / 'installation-receipt').write_bytes(f'LASRECEIPT1\nlas/{release.version}\n'.encode() + b'a'*64 + b'\n')
            (meta / 'current').write_bytes(b'LASINSTALL2\nbuild-1\n')
            (meta / 'pending').write_bytes(b'LASINSTALL2\nbuild-2\n')
            command = [sys.executable, str(Path(__file__).with_name('adapter.py')), 'doctor', '--game-dir', str(slot)]
            active = json.loads(subprocess.check_output(command, encoding='utf-8'))
            candidate = json.loads(subprocess.check_output(command + ['--pending'], encoding='utf-8'))
            self.assertEqual((active['installation']['state'], active['module_plan']['version']), ('published', '1.0.0'))
            self.assertEqual((candidate['installation']['state'], candidate['module_plan']['version']), ('pending', '1.1.0'))
            self.assertIn('does not certify gameplay', candidate['evidence'])
            (meta / 'current').write_bytes((meta / 'pending').read_bytes())
            with self.assertRaisesRegex(ValueError, 'already published'): installation_root(slot, True)
            (meta / 'current').write_bytes(b'LASINSTALL2\nbuild-1\n')
            (meta / 'build-1/content/installation-receipt').write_bytes(b'LASRECEIPT1\nwrong/1.0.0\n' + b'a'*64 + b'\n')
            failed = subprocess.run(command, capture_output=True, encoding='utf-8')
            self.assertNotEqual(failed.returncode, 0); self.assertIn('mismatched', failed.stderr)
            (meta / 'current').unlink()
            with self.assertRaisesRegex(ValueError, '--pending'): installation_root(slot)
            self.assertEqual(installation_root(slot, True)[1]['state'], 'pending')

    def test_legacy_and_invalid_records(self):
        with tempfile.TemporaryDirectory() as temp:
            slot = Path(temp)
            self.assertEqual(installation_root(slot)[1]['state'], 'legacy')
            meta = slot / '.installations'; meta.mkdir()
            (meta / 'owner').write_bytes(b'LASINSTALL2\n')
            for pointer in (b'LASINSTALL2\n../outside\n', b'LASINSTALL2\nbuild-1\nextra\n', b'x'*4097):
                (meta / 'current').write_bytes(pointer)
                with self.assertRaises(ValueError): installation_root(slot)


if __name__ == '__main__': unittest.main()
