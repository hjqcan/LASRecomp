"""Public helper boundaries, using only synthetic extracted modules."""
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import tempfile
import unittest

from game_catalog import GAMES, catalog
from game_setup import main
from test_game_fixtures import synthetic_nso


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_catalog_matches_reviewed_adapters(self):
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(['--catalog']), 0)
        self.assertEqual(json.loads(output.getvalue()), json.loads(json.dumps(catalog())))

    def test_extracted_modules_are_identified_without_creating_files(self):
        for game in GAMES:
            for release in game.releases:
                source = self.root / game.id / release.version / 'exefs'
                source.mkdir(parents=True)
                for module in release.modules:
                    (source / module.name).write_bytes(synthetic_nso(module.build_id))
                before = sorted(p.relative_to(self.root) for p in self.root.rglob('*'))
                output = io.StringIO()
                with redirect_stdout(output):
                    self.assertEqual(main(['--inspect-exefs', str(source)]), 0)
                plan = json.loads(output.getvalue())
                self.assertEqual((plan['game_id'], plan['version']), (game.id, release.version))
                self.assertEqual(plan['installable'], release.installable)
                self.assertEqual(sorted(p.relative_to(self.root) for p in self.root.rglob('*')), before)

    def test_archive_and_key_arguments_are_not_supported(self):
        for options in (['--base', 'game.nsz'], ['--catalog', '--keys', 'prod.keys'],
                        ['--catalog', '--update', 'update.nsp'], ['--catalog', '--title-keys', 'title.keys']):
            with self.subTest(options=options), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                main(options)
            self.assertEqual(error.exception.code, 2)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_a_file_cannot_be_used_as_an_extracted_directory(self):
        source = self.root / 'game.xci'
        source.write_bytes(b'not an extracted folder')
        with redirect_stderr(io.StringIO()):
            self.assertEqual(main(['--inspect-exefs', str(source)]), 1)
        self.assertEqual(source.read_bytes(), b'not an extracted folder')
        self.assertEqual(list(self.root.iterdir()), [source])

    def test_actions_cannot_be_combined(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            main(['--catalog', '--prepare-speedfix60', str(self.root)])
        self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
