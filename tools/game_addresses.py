"""Read the same 1.0.0 address table the C/C++ runtime includes.

The header is the authoritative data; no generated copy or Python address list
needs updating alongside it. Game-version selection remains unchanged.
"""
from pathlib import Path
import re
import sys
from types import SimpleNamespace


def load_table(path):
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('#define GA_'):
            continue
        match = re.fullmatch(r'#define GA_([A-Z][A-Z0-9_]*)\s+UINT64_C\((0x[0-9a-f]+)\)', line)
        if not match or match[1] in values:
            raise ValueError(f'Invalid or duplicate game-address definition: {line}')
        values[match[1]] = int(match[2], 16)
    if not values:
        raise ValueError('Empty game-address table')
    return values


_root = Path(sys._MEIPASS) if hasattr(sys, '_MEIPASS') else Path(__file__).resolve().parent.parent
TABLE_PATH = _root / 'runtime/game_addresses/v1_0_0.h'
ADDRESS = SimpleNamespace(**load_table(TABLE_PATH))


def addr_text(code):
    """Expand @NAME@ as hex digits inside generated-C addresses, symbols or labels."""
    return re.sub(r'@([A-Z][A-Z0-9_]*)@', lambda m: f'{getattr(ADDRESS, m[1]):x}', code)
