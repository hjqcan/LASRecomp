"""Reviewed title adapters, shared by archive inspection and build planning.

Only identifiers and host configuration live here. No executable code or assets
from a game are distributed. A known build is not necessarily ready to play.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path


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

    @property
    def main_build_id(self):
        return next(m.build_id for m in self.modules if m.name == 'main')


@dataclass(frozen=True)
class Game:
    id: str
    name: str
    application_id: str
    status: str
    installable: bool
    generator_profile: str
    adapter_revision: int
    romfs_roots: tuple[str, ...]
    init_order: tuple[str, ...]
    releases: tuple[Release, ...]

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


def _id(value):
    if not 0 < len(value) <= 64 or len(value) % 2:
        raise ValueError('Invalid catalog Build ID')
    bytes.fromhex(value)
    return value.lower().ljust(64, '0')


_LAS_SDK = Module('sdk', _id('3ae34e689d7a35f13cbf42bfd672af8b5eb64a9d'), 0x7004000000, True)
GAMES = (
    Game('las', "The Legend of Zelda: Link's Awakening", '01006bb00c6f0000',
         'beta', True, 'las', 1, ('region_common',), ('sdk', 'main'), (
             Release('1.0.0', 'base', 0, (
                 Module('main', _id('ae16f71e002af8cb059a9a74c4d90f34ba984892'), 0x7000000000), _LAS_SDK)),
             Release('1.1.0', 'update', 131072, (
                 Module('main', _id('909e904af78ac1b8deefe97ab2ccdb51968f0ec7'), 0x7000000000), _LAS_SDK)),
         )),
    Game('acnh', 'Animal Crossing: New Horizons', '01006f8002326000',
         'development', False, 'generic', 1, ('Movie',), ('sdk', 'subsdk0', 'main'), (
             Release('1.0.0', 'base', 0, (
                 Module('main', _id('7fc1baff976aeca414520cb89f4616cb'), 0x7000000000),
                 Module('sdk', _id('ad3fdfbd62a5f4b082d385ffe420d4400abb831d'), 0x7004000000, True),
                 Module('subsdk0', _id('5a7ada8dd7447c10aa244944f64d0aff17c50539'), 0x7006000000),
             )),
         )),
)


def get_game(game_id='las'):
    for game in GAMES:
        if game.id == game_id:
            return game
    raise ValueError(f'Unknown game adapter: {game_id}')


def catalog():
    return {'schema': 1, 'games': [asdict(game) for game in GAMES]}


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
                status=game.status, installable=game.installable,
                adapter_revision=game.adapter_revision, generator_profile=game.generator_profile,
                installation_key=f'{game.application_id}/{build_id}',
                init_order=game.init_order, modules=modules)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, help='Inspect local modules without generating or running code')
    args = parser.parse_args(argv)
    try:
        result = inspect_exefs(args.exefs) if args.exefs else catalog()
    except (ValueError, OSError) as error:
        parser.exit(1, f'Catalog inspection failed: {error}\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
