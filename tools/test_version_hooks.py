"""Check canonical/updated hook coordinates without game assets."""
from types import SimpleNamespace
from pathlib import Path
import hashlib
import json
import re
import struct

from game_versions import address, hook_table, original_address, patch_table, TEXT_CUT, TEXT_END
from patches import HOOKS, PATCHES
from game_addresses import ADDRESS

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

# Every instruction replacement must read the selected executable, including
# patches added by a later upstream merge. The canonical slot remains zero in
# the updated image, so bypassing patch_word cannot accidentally pass a guard.
fixture=json.loads((Path(__file__).parent/'tests/patches_v1_0_0.json').read_text())['patches']
for version in ('1.0.0', '1.1.0'):
    mapped=patch_table(PATCHES,version)
    for text,expected in fixture.items():
        site=int(text,16); actual=address(site,version); word=int(expected['word'],16)
        if word & 0x7c000000 == 0x14000000:
            imm=word & 0x3ffffff
            if imm & 0x2000000: imm-=0x4000000
            target=address(site+imm*4,version)
            word=(word & 0xfc000000) | (((target-actual)//4) & 0x3ffffff)
        image=bytearray(0x104)
        gen=SimpleNamespace(base=actual-0x100,game_version=version,nso=SimpleNamespace(image=image))
        struct.pack_into('<I',image,0x100,word)
        preceding = expected.get('preceding_words', [])
        for i, previous in enumerate(preceding):
            struct.pack_into('<I', image, 0x100-4*(len(preceding)-i), int(previous,16))
        code=mapped[actual](gen,actual)
        canonical=re.sub(r'(?<![0-9a-fA-F])700[0-9a-fA-F]{7}(?![0-9a-fA-F])',
                         lambda m:f'{original_address(int(m[0],16),version):x}',code)
        assert hashlib.sha256(canonical.encode()).hexdigest()==expected['sha256'], (version,text)
        for i in range(len(preceding)):
            offset=0x100-4*(len(preceding)-i)
            image[offset]^=1
            try: mapped[actual](gen,actual)
            except AssertionError: pass
            else: raise AssertionError(f'Changed preceding instruction accepted: {version} {text}')
            image[offset]^=1
        if site==ADDRESS.FONT_BOX_GLYPH_SITE:
            assert f'goto L_{address(ADDRESS.FONT_BOX_GLYPH_SKIP,version):x}' in code
            image[0x100]^=1
            try: mapped[actual](gen,actual)
            except AssertionError: pass
            else: raise AssertionError('Changed font glyph instruction accepted')
print('Both executable profiles: all patch outputs, hook relocation, target labels and instruction guards passed')
