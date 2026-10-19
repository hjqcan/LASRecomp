"""Validate a plaintext RomFS image retained in an extracted installation."""
import struct

from binary_view import View, require, safe_name
from game_catalog import get_game

EMPTY = 0xffffffff


def romfs_header(view, offset, game=None):
    game = game or get_game()
    h = struct.unpack('<10Q', view.read(offset, 80))
    require(h[0] == 0x50 and 0x50 <= h[9] <= 1 << 20, 'Invalid RomFS header')
    for i in (1, 3, 5, 7):
        require(h[i] >= 0x50 and 0 < h[i + 1] <= 16 << 20 and
                h[i] + h[i + 1] <= view.size - offset, 'Invalid RomFS table range')
    root = struct.unpack('<6I', view.read(offset + h[3], 24))
    require(root[0] == 0 and root[5] == 0, 'Invalid RomFS root')
    child, visited, names = root[2], set(), set()
    while child != EMPTY:
        require(child not in visited and child + 24 <= h[4], 'Invalid RomFS directory chain')
        visited.add(child)
        entry = struct.unpack('<6I', view.read(offset + h[3] + child, 24))
        require(child + 24 + entry[5] <= h[4], 'Invalid RomFS name length')
        names.add(safe_name(view.read(offset + h[3] + child + 24, entry[5])))
        child = entry[1]
    require(set(game.romfs_roots) <= names, 'Not the game RomFS')
    return h
