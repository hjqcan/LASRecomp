"""Read the adapter catalog or prepare an already-extracted game installation."""
import argparse
import json
from pathlib import Path
import sys

from game_catalog import catalog, inspect_exefs
from host_paths import io_path


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--catalog', action='store_true', help='List reviewed adapters and installation status')
    action.add_argument('--inspect-exefs', type=Path, help='Identify already-extracted executable modules')
    action.add_argument('--prepare-speedfix60', type=Path,
                        help='Prepare ACNH 3.0.1 60 FPS + SpeedFix from an installed game folder')
    args = parser.parse_args(argv)
    try:
        if args.prepare_speedfix60:
            from acnh_speedfix import prepare
            prepare(args.prepare_speedfix60)
        else:
            result = inspect_exefs(io_path(args.inspect_exefs)) if args.inspect_exefs else catalog()
            print(json.dumps(result, ensure_ascii=False))
    except (ValueError, OSError) as error:
        print(f'Game setup failed: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
