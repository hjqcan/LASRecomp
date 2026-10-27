#!/usr/bin/env python3
"""Key rings (Randomizer setting 'key-rings', default off): each dungeon's small keys come as one item, the dungeon's
key ring, which gives all of them at once.

  - item keyring-Dn (n = 0 Color Dungeon, 1-8), shown as the game's small key, one per dungeon that has keys; the
    keys it replaces leave the pool, their places go to the most plentiful junk rupee
  - counted after the room shuffle set each dungeon's key count; the sizes go into settings.json 'key-ring-sizes'
  - logic: a ring counts as all of its dungeon's keys (shuffler / playthrough KEY_RINGS)
  - always given with index RING_INDEX + d (d = 0-7 D1-D8, 9 Color Dungeon), also inside its own dungeon: the runtime
    (runtime/hle/keysanity.c) adds the ring's keys to that dungeon, the get message names the dungeon
    (keysanity_text.py with the sentences below), in the game's own message box
"""
import re

RING = 'keyring-'
RING_INDEX = 100  # (runtime/hle/keysanity.c)

# the get message: '{name}' = the key ring (red), '{place}' = the dungeon (English only; other languages show it on a
# second page, as the keysanity messages do)
GET = {'en': 'You got the {name} for {place}!',
       'de': 'Du hast den {name} erhalten!', 'fr': 'Vous avez obtenu le {name} !', 'fr_us': 'Tu as obtenu le {name}!',
       'es': '¡Has conseguido el {name}!', 'es_us': '¡Obtuviste el {name}!', 'it': 'Hai ottenuto il {name}!',
       'nl': 'Je hebt de {name}!', 'ru': 'Вы получили {name}!', 'ja': '{name}を 手に入れた！',
       'ko': '{name}를 손에 넣었다!', 'zh_cn': '得到了{name}！', 'zh_tw': '得到了{name}！'}
NAME = {'en': 'Key Ring', 'de': 'Schlüsselbund', 'fr': 'trousseau de clés', 'es': 'llavero', 'it': 'mazzo di chiavi',
        'nl': 'sleutelbos', 'ru': 'связку ключей', 'ja': 'カギのたば', 'ko': '열쇠 꾸러미', 'zh_cn': '钥匙串',
        'zh_tw': '鑰匙串'}


def index_of(item):
    n = int(item[-1])
    return RING_INDEX + (9 if n == 0 else n - 1)


def convert(item_defs, settings):
    """(After the room shuffle set the key counts) small keys -> one ring per dungeon. Returns {'Dn': keys}."""
    import playthrough
    from RandomizerCore import shuffler
    sizes = {}
    for n in range(9):
        key = f'key-D{n}'
        q = item_defs.get(key, {}).get('quantity', 0)
        if q <= 0:
            continue
        item_defs[RING + f'D{n}'] = dict(item_defs[key], quantity=1)
        item_defs[key]['quantity'] = 0
        sizes[f'D{n}'] = q
    freed = sum(sizes.values()) - len(sizes)
    rupees = sorted((k for k, v in item_defs.items() if re.fullmatch(r'rupee-\d+', k) and v.get('type') == 'junk'),
                    key=lambda k: (-item_defs[k]['quantity'], k))
    if rupees and freed > 0:
        item_defs[rupees[0]]['quantity'] += freed
    settings['key-ring-sizes'] = sizes
    rings = {RING + d: n for d, n in sizes.items()}
    shuffler.KEY_RINGS.clear()
    shuffler.KEY_RINGS.update(rings)
    playthrough.KEY_RINGS.clear()
    playthrough.KEY_RINGS.update(rings)
    return sizes


def mark_indexes(placements):
    """Every ring carries its dungeon's ring index. Returns the number of rings."""
    n = 0
    for loc, item in placements.items():
        if loc not in ('settings', 'indexes') and isinstance(item, str) and item.startswith(RING):
            placements['indexes'][loc] = index_of(item)
            n += 1
    return n


def message(lang, name_raw, wait):
    """The ring's get message in `lang` (loc_text language code); name_raw = the dungeon's name (Place.msbt)."""
    import loc_text as lt
    name = (lt.tr(NAME, lang) or NAME['en'], True)
    place = (lt._clean(name_raw), True)
    if lang == 'en':
        return lt.wrap(lt.fill(GET['en'], name=name, place=place), wait, lang) + wait + b'\0'
    return lt.pages([lt.fill(lt.tr(GET, lang), name=name), [place]], wait, lang)
