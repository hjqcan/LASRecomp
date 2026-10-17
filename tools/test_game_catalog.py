"""Adapter boundaries and cross-title rejection; synthetic headers only."""
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch
from types import SimpleNamespace

from game_catalog import ADAPTERS, GAMES, get_game, inspect_exefs, load_games
from game_versions import VERSIONS
from import_game import find_exefs, import_game, inspect_package, main, validate_metadata
from containers import View
from test_import_game import pfs_files, synthetic_nso, synthetic_romfs, ncz


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def write_modules(self, game):
        for module in game.releases[0].modules:
            (self.root / module.name).write_bytes(synthetic_nso(module.build_id))

    def test_plans_identify_titles_and_exact_module_layout(self):
        for game in GAMES:
            self.write_modules(game)
            plan = inspect_exefs(self.root)
            self.assertEqual(plan['game_id'], game.id)
            self.assertEqual(plan['installable'], game.id == 'las')
            self.assertEqual(plan['generator_profile'], 'las' if game.id == 'las' else 'generic')
        self.write_modules(get_game('acnh'))
        plan = inspect_exefs(self.root)
        self.assertEqual(plan['init_order'], ('sdk', 'subsdk0', 'main'))
        self.assertEqual([m['base'] for m in plan['modules']], ['0x7000000000', '0x7004000000', '0x7006000000'])

    def test_unknown_build_and_tail_collision_are_rejected(self):
        game = get_game()
        self.write_modules(game)
        main_file = self.root / 'main'
        changed = bytearray(main_file.read_bytes())
        changed[0x5f] = 1  # same old 20-byte prefix, different complete Build ID
        main_file.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, 'No unique adapter'):
            inspect_exefs(self.root)
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            game.release(game.releases[0].main_build_id[:40])

    def test_mixed_sdk_and_missing_subsdk_are_rejected(self):
        game = get_game('acnh')
        self.write_modules(game)
        (self.root / 'sdk').write_bytes(synthetic_nso(get_game().releases[0].modules[1].build_id))
        with self.assertRaisesRegex(ValueError, 'sdk Build ID mismatch'):
            inspect_exefs(self.root)
        self.write_modules(game)
        (self.root / 'subsdk0').unlink()
        with self.assertRaises(FileNotFoundError):
            inspect_exefs(self.root)

    def test_development_gate_precedes_filesystem_changes(self):
        destination = self.root / 'new-parent' / 'game'
        with self.assertRaisesRegex(ValueError, 'in development'):
            import_game(self.root / 'missing.nsz', None, destination, game_id='acnh')
        self.assertFalse(destination.parent.exists())

    def test_metadata_cross_title_and_unknown_update_are_rejected(self):
        game = get_game('acnh')
        metadata = dict(application_id=game.application_id, title_id=game.application_id,
                        role='base', content_version=0)
        validate_metadata(metadata, 'base', game)
        with self.assertRaisesRegex(ValueError, 'not Link'):
            validate_metadata(metadata)
        with self.assertRaisesRegex(ValueError, 'Unsupported content version'):
            validate_metadata(dict(metadata, title_id=game.update_id, role='update', content_version=65536), game=game)

    def test_archive_inspection_reports_development_without_installing(self):
        game = get_game('acnh')
        archive = self.root / 'example.nsz'
        xml = f'<ContentMeta><Type>Application</Type><Id>{game.application_id}</Id><Version>0</Version></ContentMeta>'
        archive.write_bytes(pfs_files({'program.ncz': b'not decoded during inspection', 'app.cnmt.xml': xml.encode()}))
        info = inspect_package(archive, game=game)
        self.assertEqual(info['game_id'], 'acnh')
        self.assertEqual(info['game_version'], '1.0.0')
        self.assertEqual(info['metadata_source'], 'XML (unverified)')
        self.assertFalse(info['installable'])
        self.assertEqual(list(self.root.iterdir()), [archive])
        # The file picker no longer needs a manually selected title.
        self.assertEqual(inspect_package(archive)['game_id'], 'acnh')

    def test_metadata_free_auto_detection_and_development_import(self):
        game = get_game('acnh')
        files = {m.name: synthetic_nso(m.build_id) for m in game.releases[0].modules}
        files.update({'rtld': b'fixture loader', 'main.npdm': b'fixture metadata'})
        exefs = pfs_files(files)
        romfs = synthetic_romfs('Movie')
        # A misleading filename must not select LAS or an update.
        archive = self.root / 'Zelda v1.1.0.nsz'
        archive.write_bytes(pfs_files({'program.ncz': ncz(bytes(romfs) + exefs, sections=[
            (0x4000, len(romfs)), (0x4000 + len(romfs), len(exefs))])}))
        with redirect_stdout(io.StringIO()):
            info = inspect_package(archive)
        self.assertEqual((info['game_id'], info['game_version'], info['role']), ('acnh', '1.0.0', 'base'))
        self.assertEqual(info['metadata_source'], 'ExeFS')
        self.assertEqual(info['main_build_id_full'], game.releases[0].main_build_id)
        output = self.root / 'managed' / 'acnh'
        with redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, 'in development'):
            import_game(archive, None, output)
        self.assertFalse(output.parent.exists())
        with patch('import_game.shutil.disk_usage', return_value=SimpleNamespace(free=100 << 30)), redirect_stdout(io.StringIO()):
            result = import_game(archive, None, output, allow_development=True)
        self.assertEqual(result['game_id'], 'acnh')
        self.assertEqual((output / 'romfs/Movie/example.bin').read_bytes(), b'original')
        self.assertEqual(inspect_exefs(output / 'exefs')['game_id'], 'acnh')
        with redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, 'No unique supported'):
            inspect_package(archive, game=get_game('las'))

    def test_archive_module_identity_checks_all_modules(self):
        game = get_game('acnh')
        files = {m.name: synthetic_nso(m.build_id) for m in game.releases[0].modules}
        files.update({'rtld': b'fixture loader', 'main.npdm': b'fixture metadata'})
        payload = pfs_files(files)
        found, build_id = find_exefs(View(io.BytesIO(payload)), [(0, len(payload))], game)
        self.assertEqual(build_id, game.releases[0].main_build_id)
        self.assertEqual(len(found), 5)
        files['sdk'][-1] = 1
        payload = pfs_files(files)
        with self.assertRaisesRegex(ValueError, 'No unique supported'):
            find_exefs(View(io.BytesIO(payload)), [(0, len(payload))], game)

    def test_catalog_cli_and_existing_patch_versions_agree(self):
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(['--catalog']), 0)
        self.assertEqual([g['id'] for g in json.loads(output.getvalue())['games']], [game.id for game in GAMES])
        self.assertEqual({r.main_build_id[:40]: r.version for r in get_game().releases}, VERSIONS)

    def test_za_requires_development_virtual_memory_and_nvn55(self):
        game = get_game('pokemon_za'); release = game.releases[0]
        self.assertEqual((release.runtime['nvn_major'],release.runtime['nvn_minor']),(55,15))
        self.assertEqual([m.base for m in release.modules],[0x7000000000,0x7008000000])
        self.assertFalse(release.installable)
        output=self.root/'untouched'/'game'
        with self.assertRaisesRegex(ValueError,'in development'):
            import_game(self.root/'missing.nsz',None,output,game_id=game.id)
        self.assertFalse(output.parent.exists())
        descriptor=self.root/'adapters/pokemon_za/adapter.json';descriptor.parent.mkdir(parents=True)
        data=json.loads((ADAPTERS/'pokemon_za/adapter.json').read_text(encoding='utf8'))
        data['releases'][0].update(status='beta',installable=True)
        descriptor.write_text(json.dumps(data),encoding='utf8')
        with self.assertRaisesRegex(ValueError,'unavailable core capability'):load_games(descriptor.parent.parent)
        core=json.loads((ADAPTERS/'core-capabilities.json').read_text(encoding='utf8'))
        core_file=descriptor.parent.parent/'core-capabilities.json'
        # Either missing feature independently prevents promoting this release.
        for available,variant in (('gpu.api','nvn55'),('kernel.virtual_memory','dynamic_physical')):
            enabled=json.loads(json.dumps(core))
            enabled['capabilities'][available]['variants'].append(variant)
            core_file.write_text(json.dumps(enabled),encoding='utf8')
            with self.subTest(available=available),self.assertRaisesRegex(ValueError,'unavailable core capability'):
                load_games(descriptor.parent.parent)
        data['releases'][0].update(status='development',installable=False)
        for invalid in (0,-1,'55',True):
            data['runtime']['nvn_major']=invalid;descriptor.write_text(json.dumps(data),encoding='utf8')
            with self.subTest(value=invalid),self.assertRaisesRegex(ValueError,'runtime configuration'):
                load_games(descriptor.parent.parent)


if __name__ == '__main__':
    unittest.main()
