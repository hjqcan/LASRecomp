"""Reviewed title adapters, shared by archive inspection and build planning.

Only identifiers and host configuration live here. No executable code or assets
from a game are distributed. A known build is not necessarily ready to play.
"""
import argparse
from dataclasses import asdict, dataclass, field
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1]))
ADAPTERS = ROOT / 'adapters'
SHARED_GENERATOR_INPUTS = ('tools/gen.py', 'tools/lifter.py', 'tools/analyze.py', 'tools/nso.py',
                           'tools/passes.py', 'tools/simd_c.py', 'tools/adapter_patches.py')
PROFILE_INPUTS = {
    'generic': (),
    'las': ('tools/game_versions.py', 'tools/game_addresses.py', 'runtime/game_addresses/v1_0_0.h',
            'tools/rando_profiles.py', 'tools/patches.py', 'tools/rando/patch_variants.json'),
}


@dataclass(frozen=True)
class Module:
    name: str
    build_id: str  # all 32 NSO Build ID bytes, including padding
    base: int
    hle: bool = False


@dataclass(frozen=True)
class Release:
    version: str
    role: str
    content_version: int
    modules: tuple[Module, ...]
    status: str
    installable: bool
    content: dict
    installation: dict
    instruction_patches: tuple[dict, ...] = ()
    init_order: tuple[str, ...] = ()
    romfs_roots: tuple[str, ...] = ()
    runtime: dict = field(default_factory=dict)
    requirements: dict = field(default_factory=dict)

    @property
    def main_build_id(self):
        return next(m.build_id for m in self.modules if m.name == 'main')


@dataclass(frozen=True)
class Game:
    id: str
    name: str
    application_id: str
    generator_profile: str
    adapter_revision: int
    romfs_roots: tuple[str, ...]
    init_order: tuple[str, ...]
    releases: tuple[Release, ...]
    runtime: dict
    requirements: dict
    acceptance: tuple[str, ...]

    @property
    def update_id(self):
        return f'{int(self.application_id, 16) + 0x800:016x}'

    def release(self, build_id):
        # Never accept a prefix: newer toolchains may use the entire field.
        for release in self.releases:
            if release.main_build_id == build_id.lower():
                return release
        raise ValueError(f'Unsupported {self.name} executable Build ID: {build_id}')

    def metadata_release(self, role, content_version):
        for release in self.releases:
            if release.role == role and release.content_version == content_version:
                return release
        raise ValueError(f'Unsupported content version for {self.name}: {role} {content_version}')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def relative_path(value):
    require(isinstance(value, str) and value and not value.startswith('/') and
            not any(p in ('', '.', '..') for p in value.split('/')) and
            not any(c in value for c in '\\:\x00\r\n'), 'Invalid adapter relative path')
    return value


def load_games(directory=ADAPTERS):
    """Read reviewed source adapters, never execute code from an imported game."""
    games, identities, builds, locations = [], set(), set(), set()
    legacy_indices, legacy_aliases, settings = set(), set(), {'existing'}
    core_path = Path(directory) / 'core-capabilities.json'
    core = json.loads((core_path if core_path.exists() else ADAPTERS / 'core-capabilities.json').read_text(encoding='utf-8'))['capabilities']
    documents = [json.loads(p.read_text(encoding='utf-8')) for p in sorted(Path(directory).glob('*/adapter.json'))]
    for data in sorted(documents, key=lambda d: (d.get('order', 100), d['id'])):
        require(data['schema'] == 1, 'Unsupported adapter schema')
        require(re.fullmatch(r'[a-z][a-z0-9_]{0,31}', data['id']), 'Invalid adapter ID')
        require(isinstance(data['name'], str) and 0 < len(data['name']) <= 160 and
                not any(c in data['name'] for c in '\x00\r\n'), 'Invalid game name')
        require(re.fullmatch(r'[0-9a-f]{16}', data['application_id']), 'Invalid application ID')
        require(data['id'] not in identities and data['application_id'] not in identities, 'Duplicate game identity')
        identities.update((data['id'], data['application_id']))
        require(data['generator_profile'] in PROFILE_INPUTS, 'Unknown generator profile')
        require(type(data['adapter_revision']) is int and data['adapter_revision'] > 0, 'Invalid adapter revision')
        require(0 < len(data['romfs_roots']) <= 8, 'Adapter needs 1 to 8 content roots')
        for name in data['romfs_roots']:
            relative_path(name)
            require('/' not in name, 'RomFS roots must be top-level names')
        runtime = data['runtime']
        require(type(runtime['nvn_minor']) is int and runtime['nvn_minor'] > 0 and
                type(runtime['heap_scale']) is int and 1 <= runtime['heap_scale'] <= 16, 'Invalid runtime configuration')
        require(set(runtime['features']) <= {'las_extensions'}, 'Unknown adapter feature')
        require(data['acceptance'] and isinstance(data['requirements'], dict), 'Adapter needs requirements and acceptance checks')
        require(all(isinstance(k, str) and isinstance(v, str) for k, v in data['requirements'].items()), 'Invalid capability requirement')
        releases, versions, content_versions = [], set(), set()
        for item in data['releases']:
            requirements = dict(data['requirements'], **item.get('requirements', {}))
            release_runtime = dict(runtime, **item.get('runtime', {}))
            roots = tuple(item.get('romfs_roots', data['romfs_roots']))
            init_order = tuple(item.get('init_order', data['init_order']))
            require(0 < len(roots) <= 8, 'Release needs content roots')
            for root in roots:
                relative_path(root); require('/' not in root, 'RomFS roots must be top-level names')
            require(type(release_runtime['nvn_minor']) is int and release_runtime['nvn_minor'] > 0 and
                    type(release_runtime['heap_scale']) is int and 1 <= release_runtime['heap_scale'] <= 16 and
                    set(release_runtime['features']) <= {'las_extensions'}, 'Invalid release runtime configuration')
            require(all(isinstance(k, str) and isinstance(v, str) for k, v in requirements.items()), 'Invalid release requirement')
            require(re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,3}', item['version']), 'Invalid release version')
            require(item['version'] not in versions, 'Duplicate release version')
            versions.add(item['version'])
            require(item['role'] in ('base', 'update'), 'Unsupported content role')
            content_id = (item['role'], item['content_version'])
            require(type(item['content_version']) is int and item['content_version'] >= 0 and
                    content_id not in content_versions, 'Duplicate or invalid content version')
            content_versions.add(content_id)
            require(item['status'] in ('development', 'beta', 'verified') and type(item['installable']) is bool,
                    'Invalid release qualification')
            require(not item['installable'] or item['status'] in ('beta', 'verified'), 'Development releases cannot be installable')
            require(not item['installable'] or all(v in core.get(k, {}).get('variants', []) for k, v in requirements.items()),
                    'Installable release requires an unavailable core capability')
            modules = tuple(Module(m['name'], m['build_id'], int(m['base'], 0), m.get('hle', False)) for m in item['modules'])
            require(0 < len(modules) <= 8 and modules[0].name == 'main', 'Invalid module plan')
            names = [m.name for m in modules]
            require(len(set(names)) == len(names) and set(init_order) == set(names) and
                    len(init_order) == len(names), 'Invalid module initialization order')
            for i, module in enumerate(modules):
                require(re.fullmatch(r'(?:main|sdk|rtld|subsdk[0-9])', module.name), 'Invalid module name')
                require(re.fullmatch(r'[0-9a-f]{64}', module.build_id), 'Module IDs must contain all 32 bytes')
                require(type(module.hle) is bool, 'Invalid HLE module flag')
                require(0x7000000000 <= module.base < 0x7080000000 and module.base % 0x1000 == 0 and
                        (i == 0 or module.base > modules[i-1].base), 'Invalid or unordered module bases')
            require(modules[0].build_id not in builds, 'Ambiguous executable Build ID')
            builds.add(modules[0].build_id)
            content = item['content']
            require(type(content['minimum_files']) is int and content['minimum_files'] > 0 and
                    type(content['image_satisfies_count']) is bool, 'Invalid content validation rule')
            installation = dict(item.get('installation', {}))
            installation.setdefault('directory', f'games/{data["application_id"]}/v{item["version"]}')
            installation.setdefault('save_directory', f'save/{data["application_id"]}/v{item["version"]}')
            installation.setdefault('label', data['name'] + ' ' + item['version'])
            installation.setdefault('setting', data['id'] + '/' + item['version'])
            for field, prefix in (('directory', 'games/'), ('save_directory', 'save/')):
                path = relative_path(installation[field])
                require(path.startswith(prefix) and not any(part.startswith('.') for part in path.split('/')) and
                        not any(path.casefold() == other or path.casefold().startswith(other + '/') or
                                other.startswith(path.casefold() + '/') for other in locations), 'Duplicate or invalid managed path')
                locations.add(path.casefold())
            require(not installation.get('legacy_enum') or re.fullmatch(r'[A-Z_]+', installation['legacy_enum']), 'Invalid legacy alias')
            setting = installation['setting']
            require(isinstance(setting, str) and setting and setting not in settings and not any(c in setting for c in '\x00\r\n='),
                    'Duplicate or invalid setting alias')
            settings.add(setting)
            if 'legacy_enum' in installation:
                index = installation.get('legacy_index')
                require(type(index) is int and index > 0 and index not in legacy_indices and
                        installation['legacy_enum'] not in legacy_aliases, 'Duplicate or invalid legacy slot')
                legacy_indices.add(index); legacy_aliases.add(installation['legacy_enum'])
            patches, sites = [], set()
            for patch in item.get('instruction_patches', []):
                require(patch['module'] in names and isinstance(patch.get('reason'), str) and patch['reason'], 'Patch needs a module and reason')
                offset, expected, replacement = (int(patch[k], 0) for k in ('offset', 'expected', 'replacement'))
                site = (patch['module'], offset)
                require(offset >= 0 and offset % 4 == 0 and 0 <= expected <= 0xffffffff and 0 <= replacement <= 0xffffffff and
                        site not in sites, 'Invalid or overlapping instruction patch')
                require(data['generator_profile'] == 'generic', 'LAS patches remain in its guarded legacy profile')
                sites.add(site)
                patches.append(dict(module=patch['module'], offset=offset, expected=expected, replacement=replacement))
            releases.append(Release(item['version'], item['role'], item['content_version'], modules,
                                    item['status'], item['installable'], content, installation, tuple(patches),
                                    init_order, roots, release_runtime, requirements))
        require(releases, 'Adapter needs a reviewed release')
        games.append(Game(data['id'], data['name'], data['application_id'], data['generator_profile'],
                          data['adapter_revision'], tuple(data['romfs_roots']), tuple(data['init_order']),
                          tuple(releases), runtime, data['requirements'], tuple(data['acceptance'])))
    require(games, 'No reviewed adapters found')
    require(legacy_indices == set(range(1, len(legacy_indices)+1)), 'Legacy slot indices must remain contiguous')
    aliases = {}
    for game in games:
        for release in game.releases:
            identity = game.id + '/' + release.version
            for value in (identity, release.installation['setting']):
                require(aliases.get(value, identity) == identity, 'Ambiguous installation setting')
                aliases[value] = identity
    return tuple(games)


GAMES = load_games()


def code_identity(game, release, root=ROOT):
    """Only this release's code-generation dependencies; labels and other games cannot invalidate it."""
    inputs = dict(schema=1, game_id=game.id, application_id=game.application_id, version=release.version,
                  adapter_revision=game.adapter_revision, generator_profile=game.generator_profile,
                  modules=[asdict(m) for m in release.modules], init_order=release.init_order,
                  instruction_patches=release.instruction_patches)
    digest = hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode())
    for name in SHARED_GENERATOR_INPUTS + PROFILE_INPUTS[game.generator_profile]:
        digest.update(name.encode() + b'\0')
        digest.update(hashlib.sha256((root / name).read_text(encoding='utf-8').encode('utf-8')).digest())
    return digest.hexdigest()


def get_game(game_id='las'):
    for game in GAMES:
        if game.id == game_id:
            return game
    raise ValueError(f'Unknown game adapter: {game_id}')


def game_for_application(application_id):
    matches = [game for game in GAMES if game.application_id == application_id.lower()]
    if len(matches) != 1:
        raise ValueError(f'No supported game adapter for application ID: {application_id}')
    return matches[0]


def game_for_build(build_id):
    matches = [game for game in GAMES for release in game.releases
               if release.main_build_id == build_id.lower()]
    if len(matches) != 1:
        raise ValueError(f'No unique adapter for executable Build ID: {build_id}')
    return matches[0]


def catalog():
    return {'schema': 2, 'games': [asdict(game) for game in GAMES]}


def render_c_catalog(games=None):
    lines = ['// Generated by tools/game_catalog.py --write-c; edit adapters/*/adapter.json.',
             'static const TitleProfile title_profiles[] = {']
    for game in games or GAMES:
        for release in game.releases:
            fields = (game.id, game.name, release.version, game.generator_profile, release.status)
            lines.append('    {' + ', '.join(json.dumps(v) for v in fields) + ',')
            lines.append(f'     UINT64_C(0x{game.application_id}), {game.adapter_revision}, {len(release.modules)}, '
                         + ('true' if release.installable else 'false') + ', {')
            for m in release.modules:
                lines.append(f'        {{"{m.name}", "{m.build_id}", UINT64_C(0x{m.base:x}), '
                             + ('true' if m.hle else 'false') + '},')
            order = [next(i for i, m in enumerate(release.modules) if m.name == name) for name in release.init_order]
            lines.append('     }, {' + ', '.join(map(str, order)) + '},')
            roots = ', '.join(json.dumps(name) for name in release.romfs_roots)
            lines.append(f'     "{code_identity(game, release)}", {{{roots}}}, {len(release.romfs_roots)}, '
                         f'{release.content["minimum_files"]}, {str(release.content["image_satisfies_count"]).lower()}, '
                         f'{release.runtime["nvn_minor"]}, {release.runtime["heap_scale"]}, '
                         f'{str("las_extensions" in release.runtime["features"]).lower()}' + '},')
    return '\n'.join(lines + ['};', ''])


def render_installations(games=None):
    entries = [('existing', 'extracted', 'save', 'Existing installation', None, None, 'existing', True)]
    aliases = ['GAME_PROFILE_EXISTING = 0']
    releases = [(g, r) for g in games or GAMES for r in g.releases]
    for game, release in sorted(releases, key=lambda item: (item[1].installation.get('legacy_index', 1 << 20), item[0].id, item[1].version)):
        install = release.installation
        if install.get('legacy_enum'):
            aliases.append(f'GAME_PROFILE_{install["legacy_enum"]} = {len(entries)}')
        entries.append((game.id + '/' + release.version, install['directory'], install['save_directory'],
                        install['label'], game.id, release.version, install['setting'],
                        'las_extensions' in release.runtime['features']))
    lines = ['// Generated by tools/game_catalog.py --write-installations; edit adapters/*/adapter.json.',
             'enum { ' + ', '.join(aliases) + f', GAME_PROFILE_COUNT = {len(entries)}' + ' };',
             'static const GameInstallation game_installations[GAME_PROFILE_COUNT] = {']
    for entry in entries:
        values = [('NULL' if v is None else str(v).lower() if isinstance(v, bool) else json.dumps(v)) for v in entry]
        lines.append('    {' + ', '.join(values) + '},')
    return '\n'.join(lines + ['};', ''])


def render_entry(plan):
    lines = ['// Generated from the reviewed game adapter.',
             f'#define GAMELIB_GAME_ID "{plan["game_id"]}"',
             f'#define GAMELIB_GAME_VERSION "{plan["version"]}"',
             f'#define GAMELIB_ADAPTER_REVISION {plan["adapter_revision"]}',
             f'#define GAMELIB_ADAPTER_ID "{plan["code_identity"]}"']
    for module in plan['modules']:
        name = module['name']
        lines.extend([f'extern const uint64_t {name}_fn_addrs[], {name}_fn_count;',
                      f'extern const GuestFn {name}_fn_ptrs[];', f'extern const char {name}_build_id_full[];'])
    lines.append('static const GameLibModule generated_modules[] = {')
    for module in plan['modules']:
        name = module['name']
        lines.append(f'    {{"{name}", UINT64_C({module["base"]}), {name}_build_id_full, '
                     f'{name}_fn_addrs, {name}_fn_ptrs, &{name}_fn_count}},')
    return '\n'.join(lines + ['};', ''])


def read_build_id(path):
    with Path(path).open('rb') as source:
        header = source.read(0x60)
    if len(header) != 0x60 or header[:4] != b'NSO0':
        raise ValueError(f'Invalid NSO header: {Path(path).name}')
    return header[0x40:0x60].hex()


def inspect_exefs(exefs):
    """Produce a read-only plan after checking every module, never execute it."""
    exefs = Path(exefs)
    build_id = read_build_id(exefs / 'main')
    matches = [(game, release) for game in GAMES for release in game.releases
               if release.main_build_id == build_id]
    if len(matches) != 1:
        raise ValueError(f'No unique adapter for executable Build ID: {build_id}')
    game, release = matches[0]
    modules = []
    for module in release.modules:
        if read_build_id(exefs / module.name) != module.build_id:
            raise ValueError(f'{game.name} {release.version}: {module.name} Build ID mismatch')
        modules.append(dict(name=module.name, build_id=module.build_id,
                            base=f'0x{module.base:x}', hle=module.hle))
    return dict(schema=1, game_id=game.id, name=game.name,
                application_id=game.application_id, version=release.version,
                status=release.status, installable=release.installable,
                adapter_revision=game.adapter_revision, generator_profile=game.generator_profile,
                code_identity=code_identity(game, release),
                installation_key=f'{game.application_id}/{build_id}',
                init_order=release.init_order, modules=modules)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, help='Inspect local modules without generating or running code')
    parser.add_argument('--write-c', type=Path, help='Export the reviewed catalog for the native runtime')
    parser.add_argument('--write-installations', type=Path, help='Export managed slots and legacy setting aliases')
    parser.add_argument('--write-entry', type=Path, help='Write the verified module entry include (requires --exefs)')
    args = parser.parse_args(argv)
    try:
        if args.write_c or args.write_installations:
            if args.write_c:
                args.write_c.write_text(render_c_catalog(), encoding='utf-8', newline='\n')
            if args.write_installations:
                args.write_installations.write_text(render_installations(), encoding='utf-8', newline='\n')
            return
        result = inspect_exefs(args.exefs) if args.exefs else catalog()
        if args.write_entry:
            if not args.exefs:
                raise ValueError('--write-entry requires --exefs')
            args.write_entry.parent.mkdir(parents=True, exist_ok=True)
            args.write_entry.write_text(render_entry(result), encoding='utf-8', newline='\n')
            return
    except (ValueError, OSError) as error:
        parser.exit(1, f'Catalog inspection failed: {error}\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
