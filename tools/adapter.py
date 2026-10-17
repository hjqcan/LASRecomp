"""Inspect, check and scaffold reviewed game adapters without running game code."""
import argparse
import json
from pathlib import Path
import re
from game_catalog import (ADAPTERS, ROOT, GAMES, catalog, get_game, inspect_exefs, load_games,
                          read_build_id, render_c_catalog, render_installations, code_identity)


def capabilities(game, core=None):
    core = core or json.loads((ADAPTERS / 'core-capabilities.json').read_text(encoding='utf-8'))['capabilities']
    return [dict(capability=name, required=variant, available=variant in core.get(name, {}).get('variants', []),
                 status=core.get(name, {}).get('status', 'missing'), limits=core.get(name, {}).get('limits', ''),
                 tests=core.get(name, {}).get('tests', [])) for name, variant in game.requirements.items()]


def report(game):
    return dict(game_id=game.id, requirements=capabilities(game), acceptance=list(game.acceptance),
                releases=[dict(version=r.version, status=r.status, installable=r.installable, requirements=capabilities(r),
                               code_identity=code_identity(game, r)) for r in game.releases],
                evidence='Declared requirements and test recipes; this command does not certify gameplay or run the listed tests.')


def scaffold(args):
    if not re.fullmatch(r'[a-z][a-z0-9_]{0,31}', args.id):
        raise ValueError('Use a lowercase adapter identifier')
    if not re.fullmatch(r'[0-9a-fA-F]{16}', args.application_id):
        raise ValueError('Supply the 16-digit application ID from inspected content metadata')
    modules = []
    for path in sorted(args.exefs.iterdir(), key=lambda p: (p.name != 'main', p.name)):
        if re.fullmatch(r'(main|sdk|rtld|subsdk[0-9])', path.name):
            modules.append(dict(name=path.name, build_id=read_build_id(path), base='REVIEW_REQUIRED', hle=path.name == 'sdk'))
    if not modules or modules[0]['name'] != 'main':
        raise ValueError('No main NSO module found')
    data = dict(schema=1, id=args.id, name=args.name, application_id=args.application_id.lower(), order=100,
                generator_profile='generic', adapter_revision=1, romfs_roots=['REVIEW_REQUIRED'],
                init_order=[m['name'] for m in modules],
                runtime=dict(nvn_minor=0, heap_scale=1, features=[]), requirements=dict(module_tables='4'),
                acceptance=['startup', 'controls', 'graphics-audio', 'save-reopen', 'repeated-start', 'extended-play'],
                releases=[dict(version=args.version, role='base', content_version=0, status='development', installable=False,
                               content=dict(minimum_files=0, image_satisfies_count=False), modules=modules)])
    # The template deliberately fails validation until its layout and content rules are reviewed.
    with args.output.open('x', encoding='utf-8') as out:
        json.dump(data, out, indent=2); out.write('\n')
    return dict(template=str(args.output), review_required=['module bases', 'initialization order', 'NVN version',
                                                          'content role/version', 'content rules', 'required capabilities'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('check', help='Validate source descriptors and generated native tables')
    check.add_argument('--write', action='store_true', help='Regenerate native tables after review')
    doctor = commands.add_parser('doctor', help='Report exact adapter requirements and missing runtime calls')
    doctor.add_argument('--game', default='las')
    doctor.add_argument('--game-dir', type=Path)
    doctor.add_argument('--log', type=Path)
    new = commands.add_parser('scaffold', help='Inventory local NSOs into a template that requires explicit review')
    new.add_argument('--id', required=True); new.add_argument('--name', required=True)
    new.add_argument('--application-id', required=True); new.add_argument('--version', required=True)
    new.add_argument('--exefs', type=Path, required=True); new.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'scaffold':
            result = scaffold(args)
        elif args.command == 'check':
            load_games()
            for name, content in (('title_catalog.inc', render_c_catalog()), ('game_installations.inc', render_installations())):
                path = ROOT / 'runtime' / name
                if args.write: path.write_text(content, encoding='utf-8', newline='\n')
                elif path.read_text(encoding='utf-8') != content: raise ValueError(f'{name} is stale; run adapter.py check --write')
            result = dict(schema=1, adapters=[report(g) for g in GAMES])
            for game in GAMES:
                for release in game.releases:
                    if release.installable and any(not c['available'] for c in capabilities(release)):
                        raise ValueError(f'{game.id} {release.version}: an installable release requires unavailable core capabilities')
        else:
            plan = inspect_exefs(args.game_dir / 'exefs') if args.game_dir else None
            game = get_game(plan['game_id'] if plan else args.game)
            result = report(game)
            if plan: result['module_plan'] = plan
            if args.log:
                # Only diagnostic records, not arbitrary instructions or full player logs.
                records = args.log.read_text(encoding='utf-8', errors='replace').splitlines()
                result['unimplemented_calls'] = list(dict.fromkeys(line[:500] for line in records
                    if '[hle] UNIMPLEMENTED ' in line or '[ipc] STUB ' in line or 'missing host implementation:' in line or
                    'missing service implementation:' in line))[:200]
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'Adapter check failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
