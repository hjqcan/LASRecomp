"""Check canonical/updated hook coordinates without game assets."""
from types import SimpleNamespace
import struct

from game_versions import address, hook_table, original_address, TEXT_CUT, TEXT_END
from patches import HOOKS

BASE = 0x7000000000
for version in ('1.0.0', '1.1.0'):
    image = bytearray(TEXT_END - BASE + 0x100)
    for site, (word, _, _) in HOOKS.items():
        actual = address(site, version)
        if word & 0x7c000000 == 0x14000000:
            imm = word & 0x3ffffff
            if imm & 0x2000000:
                imm -= 0x4000000
            target = address(site + imm * 4, version)
            word = (word & 0xfc000000) | (((target - actual) // 4) & 0x3ffffff)
        struct.pack_into('<I', image, actual - BASE, word)
    gen = SimpleNamespace(base=BASE, game_version=version, nso=SimpleNamespace(image=image))
    mapped = hook_table(HOOKS, gen)
    for site, (_, targets, reason) in HOOKS.items():
        actual = address(site, version)
        assert original_address(actual, version) == site
        assert mapped[actual][1:] == (tuple(address(t, version) for t in targets), reason)
    damaged = address(next(iter(HOOKS)), version) - BASE
    image[damaged] ^= 1
    try:
        hook_table(HOOKS, gen)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed hook instruction accepted')
assert address(TEXT_CUT, '1.1.0') == TEXT_CUT + 0xd0
assert address(TEXT_END, '1.1.0') == TEXT_END
print('Both executable profiles: hook relocation, target labels and instruction guards passed')
