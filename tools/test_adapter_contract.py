"""A third synthetic adapter must work without adding native title branches."""
import copy
from dataclasses import replace
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import struct
import tempfile
import unittest
from types import SimpleNamespace
from game_catalog import ADAPTERS, ROOT, GAMES, code_identity, load_games, render_c_catalog, render_installations
from adapter import capabilities
from adapter_patches import apply_instruction_patches


class AdapterContract(unittest.TestCase):
    def test_instruction_guards_are_atomic_and_text_scoped(self):
        base = 0x7000000000
        image = bytearray(32); struct.pack_into('<2I', image, 8, 0x52800020, 0xd65f03c0)
        module = SimpleNamespace(name='main', base=base, lo=base+8, words=(0x52800020, 0xd65f03c0), nso=SimpleNamespace(image=image))
        module.in_text = lambda address: base+8 <= address < base+16 and address % 4 == 0
        module.word = lambda address: module.words[(address-module.lo)//4]
        first = dict(module='main', offset=8, expected=0x52800020, replacement=0x52800040)
        bad = dict(module='main', offset=12, expected=0, replacement=0xd503201f)
        release = replace(GAMES[1].releases[0], instruction_patches=(first,bad))
        before = bytes(image)
        with self.assertRaisesRegex(ValueError, 'instruction mismatch'): apply_instruction_patches(module,release)
        self.assertEqual(before,bytes(image));self.assertEqual(module.words,(0x52800020,0xd65f03c0))
        outside = replace(release,instruction_patches=(dict(first,offset=24),))
        with self.assertRaisesRegex(ValueError,'outside'): apply_instruction_patches(module,outside)
        valid = replace(release,instruction_patches=(first,))
        self.assertEqual(apply_instruction_patches(module,valid),1)
        self.assertEqual(module.words[0],0x52800040)
        self.assertEqual(struct.unpack_from('<I',image,8)[0],0x52800040)
        self.assertNotEqual(code_identity(GAMES[1],valid),code_identity(GAMES[1],GAMES[1].releases[0]))

    @unittest.skipUnless(shutil.which('make'), 'GNU make is required to inspect the production build identity')
    def test_native_core_identity_ignores_unrelated_catalog_changes(self):
        makefile = (ROOT / 'Makefile').read_text(encoding='utf-8')
        lines = iter(makefile.splitlines())
        assignment = next(line for line in lines if line.startswith('GAMELIB_SRCS :=')).split(':=', 1)[1]
        while assignment.endswith('\\'): assignment = assignment[:-1] + next(lines)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); (root / 'Makefile').write_text(makefile, encoding='utf-8')
            (root / 'identity.mk').write_text('print-cache-id: ; @echo $(GAMELIB_ID)\n', encoding='utf-8')
            for name in set(assignment.split()) | {'tools/game_catalog.py', 'runtime/title_catalog.inc', 'tools/fetch_zig.sh', 'runtime/ui/launcher.mm',
                                                 'runtime/nvn/maxwell/translate.c', 'runtime/nvn/maxwell/shader.h'}:
                target = root / name; target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / name, target)
            def identity():
                return subprocess.check_output(['make', '--no-print-directory', '-s', '-f', 'Makefile',
                    '-f', 'identity.mk', 'print-cache-id'], cwd=root, text=True).strip()
            before = identity(); self.assertRegex(before, r'^[0-9a-f]{16}$')
            (root / 'adapters/new-title').mkdir(parents=True)
            (root / 'adapters/new-title/adapter.json').write_text('{}')
            with (root / 'runtime/title_catalog.inc').open('a') as stream: stream.write('\n// another reviewed title\n')
            with (root / 'tools/game_catalog.py').open('a') as stream: stream.write('\n# registry-only change\n')
            self.assertEqual(before, identity())
            with (root / 'runtime/cpu.h').open('a') as stream: stream.write('\n// ABI change\n')
            self.assertNotEqual(before, identity())

    def test_release_identity_has_only_relevant_dependencies(self):
        for game in GAMES:
            for release in game.releases:
                before = code_identity(game, release)
                renamed = replace(game, name='New display name')
                relabeled = replace(release, status='development', installable=False, installation={})
                self.assertEqual(before, code_identity(renamed, relabeled))
                changed = replace(release, modules=tuple(replace(m, build_id='ab'*32) if m.name == 'sdk' else m for m in release.modules))
                self.assertNotEqual(before, code_identity(game, changed))
                self.assertNotEqual(before, code_identity(replace(game, adapter_revision=game.adapter_revision+1), release))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            from game_catalog import PROFILE_INPUTS, SHARED_GENERATOR_INPUTS
            for name in PROFILE_INPUTS['las'] + SHARED_GENERATOR_INPUTS:
                target = root / name; target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / name, target)
            las, acnh = (next(game for game in GAMES if game.id == key) for key in ('las', 'acnh'))
            las_before = code_identity(las, las.releases[0], root)
            acnh_before = code_identity(acnh, acnh.releases[0], root)
            with (root / 'tools/patches.py').open('ab') as stream: stream.write(b'\n# changed LAS patch\n')
            self.assertNotEqual(las_before, code_identity(las, las.releases[0], root))
            self.assertEqual(acnh_before, code_identity(acnh, acnh.releases[0], root))
            # Upstream split runtime-only addresses out of the generator's patch table.
            runtime_table = root / 'runtime/game_addresses/runtime_v1_0_0.h'
            runtime_table.write_text('// runtime-only address change\n', encoding='utf-8')
            las_after_patch = code_identity(las, las.releases[0], root)
            runtime_table.write_text('// another runtime-only address change\n', encoding='utf-8')
            self.assertEqual(las_after_patch, code_identity(las, las.releases[0], root))
            with (root / 'runtime/game_addresses/patches_v1_0_0.h').open('ab') as stream:
                stream.write(b'\n// changed LAS patch address\n')
            self.assertNotEqual(las_after_patch, code_identity(las, las.releases[0], root))
            self.assertEqual(acnh_before, code_identity(acnh, acnh.releases[0], root))
            with (root / 'tools/patches.py').open('rb') as stream: normalized = stream.read().replace(b'\r\n', b'\n')
            changed = code_identity(las, las.releases[0], root)
            (root / 'tools/patches.py').write_bytes(normalized.replace(b'\n', b'\r\n'))
            self.assertEqual(changed, code_identity(las, las.releases[0], root))

    def test_third_adapter_drives_native_content_slots_and_save_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); adapters = root / 'adapters'; shutil.copytree(ADAPTERS, adapters)
            data = json.loads((ADAPTERS / 'acnh/adapter.json').read_text())
            data['releases'] = [data['releases'][0]]
            data.update(id='fixture', name='Synthetic title', application_id='0100000000000010', order=100, romfs_roots=['Data'])
            data['releases'][0].update(status='development', installable=False)
            data['releases'][0]['modules'][0]['build_id'] = 'ed' * 32
            data['releases'][0]['installation'] = {}
            data['releases'][0]['content'] = dict(minimum_files=2, image_satisfies_count=False)
            (adapters / 'fixture').mkdir(); descriptor = adapters / 'fixture/adapter.json'
            descriptor.write_text(json.dumps(data))
            games = load_games(adapters)
            # Registry ordering cannot move the numeric slots used by existing mod/save settings.
            self.assertEqual(render_installations(games), render_installations(tuple(reversed(games))))
            for original, loaded in zip(GAMES, games):
                for old, new in zip(original.releases, loaded.releases):
                    self.assertEqual(code_identity(original, old), code_identity(loaded, new))
            runtime = root / 'runtime'; (runtime / 'ui').mkdir(parents=True)
            for name in ('title_profile.h', 'title_profile.c', 'game_profiles.h', 'game_install.h', 'settings.h',
                         'ui/title_content.mm', 'ui/game_profiles.mm', 'ui/game_install.mm', 'cache_sha256.h', 'ui/host_path.h'):
                shutil.copy2(ROOT / 'runtime' / name, runtime / name)
            (runtime / 'title_catalog.inc').write_text(render_c_catalog(games), encoding='utf-8')
            (runtime / 'game_installations.inc').write_text(render_installations(games), encoding='utf-8')
            install = root / 'data/games/0100000000000010/v1.0.0'
            (install / 'exefs').mkdir(parents=True); (install / 'romfs/Data').mkdir(parents=True)
            for module in games[-1].releases[0].modules:
                header = bytearray(0x60); header[:4] = b'NSO0'; header[0x40:0x60] = bytes.fromhex(module.build_id)
                (install / 'exefs' / module.name).write_bytes(header)
            for name in ('a', 'b'): (install / 'romfs/Data' / name).write_text(name)
            harness = root / 'test.cpp'
            harness.write_text(r'''
#include "runtime/title_profile.h"
#include "runtime/game_profiles.h"
extern "C" {
#include "runtime/settings.h"
Settings g_settings={};
}
#include <cassert>
#include <cstring>
#include <string>
int main(int argc, char **argv) {
    assert(argc == 3); char why[512];
    const TitleProfile *p = title_profile_validate(argv[1], why, sizeof why);
    assert(p && !strcmp(p->key, "fixture") && !p->installable && !p->las_extensions);
    int count = -1; std::string romfs = std::string(argv[1]) + "/romfs";
    assert(title_content_validate(p, romfs.c_str(), false, &count, why, sizeof why) && count == 2);
    int slot = game_profile_from_id("fixture/1.0.0"); assert(slot >= 0);
    assert(game_profile_index(argv[1], argv[2]) == slot);
    assert(!strcmp(game_profile_save_dir(slot), "save/0100000000000010/v1.0.0"));
    assert(game_profile_parse_setting("fixture/1.0.0") == slot);
    assert(!game_profile_las_extensions(slot));
    assert(game_profile_parse_setting("1.0.0") == GAME_PROFILE_BASE);
}
''')
            exe = root / ('contract.exe' if os.name == 'nt' else 'contract')
            subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + ['-std=c++17', '-x', 'c++', str(harness),
                str(runtime / 'title_profile.c'), str(runtime / 'ui/title_content.mm'), str(runtime / 'ui/game_profiles.mm'),
                str(runtime / 'ui/game_install.mm'), '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(install), str(root / 'data')], check=True)
            # Descriptor errors must fail before generation or installation.
            for change in ('short-id', 'init-order', 'path-escape', 'development-enabled'):
                invalid = copy.deepcopy(data)
                if change == 'short-id': invalid['releases'][0]['modules'][0]['build_id'] = 'ed'
                if change == 'init-order': invalid['init_order'] = ['main']
                if change == 'path-escape': invalid['releases'][0]['installation'] = {'directory': '../outside'}
                if change == 'development-enabled': invalid['releases'][0]['installable'] = True
                descriptor.write_text(json.dumps(invalid))
                with self.subTest(change=change), self.assertRaises(ValueError): load_games(adapters)

    def test_missing_capability_is_explicit(self):
        game = replace(GAMES[0], requirements={'gpu.pool_mapping': 'sparse', 'new.service': '1'})
        self.assertTrue(all(not c['available'] for c in capabilities(game)))


if __name__ == '__main__': unittest.main()
