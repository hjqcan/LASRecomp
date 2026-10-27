#!/usr/bin/env python3
"""Hints (Randomizer setting "Hints", off by default): the owl statues and the telephone booths tell the player
about this seed. Off: vanilla statue text and LAS's tunic-swap phone, nothing here runs.

Sources: the 9 field owl statues (their Hint.msbt labels get our text), the 24 dungeon owl statues (each its own
message: the two shared entries are split the way LAS's owls.makeDungeonChanges does it, a new entry and the room's
actor pointing at it; the Chamber Dungeon's copy stays vanilla) and the 8 phone booths (one shared list of hints:
each call gives the next one, then LAS's tunic swap as before).

Hint kinds (pick(), from the seed's own logic, after the Hint Sheet's steps):
  barren    an area or dungeon with nothing that can help progress ("Nothing you need lies in ...")
  needed    an area whose items the goal can't do without, own-dungeon keys and goal items left aside (few of these)
  quest     what a long side quest holds: something needed / a good item / nothing of note
  item      where a good optional item lies (Heart Container, tunics, upgrades...), area only
  count     dungeon statues: how many Hint Sheet steps this dungeon holds
  boss      dungeon statues: what this dungeon's boss guards
Never an item at its place when that check is a Hint Sheet step, never a small key, one pool without repeats, drawn
with the seed's RNG. Area names follow the seed: dungeons by dungeon (room shuffle: dungeon-of), the way in when
dungeon entrances are shuffled, shuffled interiors by the area of the door used (hint_sheet.door_of).

    pick(logic_defs, placements, settings, check, out_dir, ent, romfs, layouts) -> summary (writes seed_hints.json)
    apply(out_dir, romfs, settings) -> summary (Hint.msbt, Telephone.msbt, the two owl flows, Telephone.bfevfl, rooms)
"""
import glob
import json
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import hint_sheet as hs  # noqa: E402
import loc_text as lt  # noqa: E402
from msbt import Msbt  # noqa: E402

# field statue entry -> its Hint.msbt label
FIELD_OWLS = {'examine_tarutaru': 'WestTalTalStoneOwl', 'examine_sannmyaku': 'EastTalTalStoneOwl',
              'examine_kouti': 'TalTalHeightStoneOwl', 'examine_ride': 'RapidsRideStoneOwl', 'examine_kao': 'FaceStoneOwl',
              'examine_ukuku': 'UkukuStoneOwl', 'examine_anaboko': 'PotholeStoneOwl', 'examine_yana': 'YarnaStoneOwl',
              'examine_area05': 'MarthaStoneOwl'}
# dungeon statue entry (after the split) -> its vanilla room. Room shuffle moves statue rooms like any other (even
# into another dungeon): a statue tells about the dungeon its room sits in now, the first one there its count.
DUNGEON_OWLS = {
    'examine_Tail04B': 'Lv01TailCave_04B', 'examine_Tail04B05F': 'Lv01TailCave_05F',
    'examine_Tail04C': 'Lv01TailCave_04C', 'examine_Pod07E': 'Lv02BottleGrotto_07E',
    'examine_Pod06G': 'Lv02BottleGrotto_06G', 'examine_Pod04C': 'Lv02BottleGrotto_04C',
    'examine_Key03G': 'Lv03KeyCavern_03G', 'examine_Key02D': 'Lv03KeyCavern_02D', 'examine_Key01A': 'Lv03KeyCavern_01A',
    'examine_Angler05E': 'Lv04AnglersTunnel_05E', 'examine_Namazu06F': 'Lv05CatfishsMaw_06F',
    'examine_Namazu03C': 'Lv05CatfishsMaw_03C', 'examine_Face04D': 'Lv06FaceShrine_04D',
    'examine_Face08G': 'Lv06FaceShrine_08G', 'examine_Face03G': 'Lv06FaceShrine_03G',
    'examine_Eagle02D': 'Lv07EagleTower_02D', 'examine_Eagle05D': 'Lv07EagleTower_05D',
    'examine_Eagle04B': 'Lv07EagleTower_04B', 'examine_Turtle06H': 'Lv08TurtleRock_06H',
    'examine_Turtle04G': 'Lv08TurtleRock_04G', 'examine_Turtle04C': 'Lv08TurtleRock_04C',
    'examine_Color06C': 'Lv10ClothesDungeon_06C', 'examine_Color07D': 'Lv10ClothesDungeon_07D',
    'examine_Color05F': 'Lv10ClothesDungeon_05F'}
# the split: new entry -> the entry its statue used (LAS owls.makeDungeonChanges)
SPLIT = {'examine_Tail04B': 'examine_Tail04B05F', 'examine_Color06C': 'examine_Color',
         'examine_Color07D': 'examine_Color', 'examine_Color05F': 'examine_Color'}
STATUE_ACTOR = b'ObjDungeonOw'
PHONES = 8
PHONE_FLAG = 'LARHintCall{}'  # (rotation: flag i set = hint i given; the last hint clears them)

GOOD = ('heart-container', 'red-tunic', 'blue-tunic', 'bomb-capacity', 'arrow-capacity', 'powder-capacity', 'bottle',
        'shell-sensor', 'medicine')
NOT_PROGRESS = ('stone-beak', 'compass', 'map')  # (dungeon items that open nothing: b778697)
QUESTS = {'trendy': ('trendy-prize-',), 'mansion': ('-seashell-reward',), 'rapids': ('rapids-race-',),
          'fishing': ('fishing-',), 'dampe': ('dampe-',), 'trade': ()}
MIN_CHECKS = 2  # (an area with fewer real checks says little when barren)


def area_key(region):
    """Both halves of the Tal Tal Mountains share one name: one area."""
    return 'taltal-mountains' if region.startswith('taltal-mountains') else region


# ------------------------------------------------------------------ picking
def _areas(logic_defs, placements, settings, ent, romfs):
    """{location: area} for the seed's real checks: 'D1'.. for dungeons, else the overworld area (shuffled interiors:
    the door's area)."""
    dungeon_of = settings.get('dungeon-of', {}) or {}
    absent = set(settings.get('absent-locations', ()))
    out = {}
    for loc, node in logic_defs.items():
        if node.get('type') != 'item' or loc.startswith('starting-') or loc in absent or not placements.get(loc):
            continue
        dn = hs.check_dungeon({'loc': loc}, dungeon_of)
        if dn:
            out[loc] = dn
            continue
        door = hs.door_of(loc, logic_defs, ent)
        area = hs.door_area(door, romfs) if door else node.get('spoiler-region')
        if area:
            out[loc] = area_key(area)
    return out


def pick(logic_defs, placements, settings, check, out_dir, ent, romfs, layouts=None):
    import playthrough
    with open(os.path.join(out_dir, 'hint_steps.json')) as f:
        steps = json.load(f)['steps']
    step_locs = {s['loc'] for s in steps}
    rng = random.Random(f"{settings['seed']}-hints")
    excluded = set(settings.get('excluded-locations', ())) | set(settings.get('absent-locations', ()))
    tri = int(settings.get('triforce-needed', 0) or 0)
    goal = {hs.TRIFORCE} if tri else set(playthrough.INSTRUMENTS)
    # what can help progress: an item some logic condition reads, or the goal; seashells / leaves only when the
    # Hint Sheet needs them; beaks, maps and compasses never (they open nothing)
    word = re.compile(r'[A-Za-z0-9][A-Za-z0-9\-\[\]]*')
    read = set()
    for node in logic_defs.values():
        for ck, cv in node.items():
            if ck.startswith('condition-') and isinstance(cv, str):
                read.update(re.sub(r'\[\d+\]$', '', w) for w in word.findall(cv))
    gated = {s['item'] for s in steps if s['item'] in hs.GATED}

    def useful(item):
        b = hs.base_item(item)
        if hs.JUNK.match(b) or b.startswith(tuple(p + '-' for p in NOT_PROGRESS)):
            return False
        if b in hs.GATED:
            return b in gated
        return b in read or b in goal or b == hs.TRIFORCE
    area_of = _areas(logic_defs, placements, settings, ent, romfs)
    locs_in = {}
    for loc, a in area_of.items():
        locs_in.setdefault(a, []).append(loc)
    real = {a: [l for l in ls if l not in excluded] for a, ls in locs_in.items()}
    reach = hs._reach_fn(logic_defs, settings, check)
    full = reach(placements)

    def own_dungeon_item(item, area):
        m = re.match(r'^(key|nightmare-key|compass|map|stone-beak)-(D\d)$', hs.base_item(item))
        return bool(m) and m.group(2) == area
    barren, needed = [], []
    for a in sorted(locs_in):
        if len(real.get(a, ())) < MIN_CHECKS:
            continue
        if not any(useful(placements[l]) for l in locs_in[a]):
            barren.append(a)
            continue
        pl = dict(placements)
        for l in locs_in[a]:
            it = placements[l]
            if hs.base_item(it) not in goal and not own_dungeon_item(it, a):
                pl[l] = None
        if reach(pl)[1] < full[1]:
            needed.append(a)
    quests = []
    trade = set()
    try:
        from RandomizerCore import randomizer_data as rd
        trade = set(rd.TRADE_GIFT_LOCATIONS)
    except Exception:
        pass
    for q, pre in QUESTS.items():
        ls = sorted(l for l in area_of if l not in excluded and (l in trade if q == 'trade' else
                                                                  any(p in l if p.startswith('-') else l.startswith(p)
                                                                      for p in pre)))
        if not ls:
            continue
        items = [placements[l] for l in ls]
        good = [l for l in ls if hs.base_item(placements[l]) in GOOD and l not in step_locs]
        if any(useful(i) for i in items):
            quests.append({'kind': 'quest', 'quest': q, 'says': 'need'})
        elif good:
            quests.append({'kind': 'quest', 'quest': q, 'says': 'item', 'item': hs.base_item(placements[good[0]]),
                           'loc': good[0]})
        else:
            quests.append({'kind': 'quest', 'quest': q, 'says': 'junk'})
    goods = []
    for loc in sorted(area_of):
        it = hs.base_item(placements[loc])
        if it in GOOD and loc not in step_locs and loc not in excluded and not useful(it):
            door = hs.door_of(loc, logic_defs, ent)
            goods.append({'kind': 'item', 'item': it, 'area': area_of[loc], 'loc': loc,
                          'door': door['id'] if door else None})
    # dungeon statues: the first tells the count, the second what the boss guards, the rest draw from the pool
    dungeon_of = settings.get('dungeon-of', {}) or {}
    dungeon_steps = {}
    for s in steps:
        dn = hs.check_dungeon(s, dungeon_of)
        if dn:
            dungeon_steps[dn] = dungeon_steps.get(dn, 0) + 1
    statues = {}  # dungeon -> [(entry, the room file it sits in now)]
    where_now = {room: cell for L in (layouts or {}).values() for cell, room in L.items()}
    for entry, room in DUNGEON_OWLS.items():
        level = room.rsplit('_', 1)[0]
        cell = where_now.get(room) if level in (layouts or {}) else room
        if cell:  # (else a room the shuffle left out)
            statues.setdefault(hs.LEVEL_DUNGEON[cell.rsplit('_', 1)[0]], []).append((entry, cell))
    bosses = {}
    for loc, a in sorted(area_of.items()):
        m = re.match(r'^(D\d)-(.*)$', loc)
        if m and m.group(2) in hs.BOSSES and len(statues.get(a, ())) > 1:  # (a statue left for it)
            it = hs.base_item(placements[loc])
            says = 'need' if useful(it) else 'item' if it in GOOD and loc not in step_locs else 'junk'
            bosses[a] = dict({'kind': 'boss', 'dungeon': a, 'says': says, 'loc': loc}, **({'item': it} if says == 'item' else {}))
    named = {h['loc'] for h in quests + list(bosses.values()) if h.get('says') == 'item'}
    goods = [g for g in goods if g['loc'] not in named]  # (one item, one hint)
    for lst in (barren, needed, quests):
        rng.shuffle(lst)
    by_item = {}  # (good items: one of each kind in turn, not four Heart Containers in a row)
    for g in goods:
        by_item.setdefault(g['item'], []).append(g)
    kinds = sorted(by_item)
    rng.shuffle(kinds)
    for k in kinds:
        rng.shuffle(by_item[k])
    goods = [g for i in range(max(map(len, by_item.values()), default=0)) for k in kinds if i < len(by_item[k])
             for g in [by_item[k][i]]]
    pools = {'barren': [{'kind': 'barren', 'area': a} for a in barren],
             'needed': [{'kind': 'needed', 'area': a} for a in needed][:3],
             'quest': quests, 'item': goods}

    def draw(kinds):
        for k in kinds:
            if pools[k]:
                return pools[k].pop(0)
        return None
    out = []
    order = ['barren', 'quest', 'needed', 'item']
    for i, entry in enumerate(FIELD_OWLS):  # 3 barren, 3 quests, 3 needed / good items
        want = ['barren', 'quest', 'needed' if i % 2 else 'item'][i % 3]
        h = draw([want] + [k for k in order if k != want])
        if h:
            out.append(dict(h, where=f'field:{entry}'))
    for i in range(PHONES):
        h = draw(order[i % 4:] + order[:i % 4])
        if h:
            out.append(dict(h, where=f'phone:{i}'))
    for dn in sorted(statues):
        for i, (entry, cell) in enumerate(statues[dn]):
            at = {'where': f'dungeon:{entry}', 'room': cell}
            if i == 0:
                out.append(dict({'kind': 'count', 'dungeon': dn, 'n': dungeon_steps.get(dn, 0)}, **at))
            elif dn in bosses:
                out.append(dict(bosses.pop(dn), **at))
            else:
                h = draw(order)
                if h:  # (none left: the statue keeps its vanilla text)
                    out.append(dict(h, **at))
    entered_via = {inside: door for door, inside in (placements.get('dungeon-entrances') or {}).items()}
    with open(os.path.join(out_dir, 'seed_hints.json'), 'w') as f:
        json.dump({'hints': out, 'entered-via': entered_via, 'areas': {a: sorted(ls) for a, ls in sorted(locs_in.items())}},
                  f, indent=1)
    kinds = {}
    for h in out:
        kinds[h['kind']] = kinds.get(h['kind'], 0) + 1
    return f"{len(out)} hints ({', '.join(f'{k} {n}' for k, n in sorted(kinds.items()))})"


# ------------------------------------------------------------------ text
QUEST_EN = {'trendy': 'the Trendy Game', 'mansion': 'the Seashell Mansion', 'rapids': 'the Rapids Ride race',
            'fishing': 'the fishing pond', 'dampe': "Dampé's Shack", 'trade': 'the trading quest'}
QUEST_LABEL = {'trendy': 'GameShop', 'mansion': 'ShellMansion', 'rapids': 'RapidsRide', 'fishing': 'FishingPond',
               'dampe': 'DanpeiShack'}
# our sentences in the other languages ({place} / {item}: the game's own names)
T = {
    'barren': {'de': '{place}: nichts, was du brauchst.', 'fr': "{place} : rien d'indispensable.",
               'fr_us': "{place}: rien d'indispensable.", 'es': '{place}: nada imprescindible.',
               'it': '{place}: niente di indispensabile.', 'nl': '{place}: niets wat je nodig hebt.',
               'ru': '{place}: ничего нужного.', 'ja': '{place}： 必要な物は ない。', 'ko': '{place}: 필요한 것은 없다.',
               'zh_cn': '{place}：没有需要的东西。', 'zh_tw': '{place}：沒有需要的東西。'},
    'needed': {'de': '{place}: hier liegt etwas, was du brauchst.', 'fr': "{place} : il y a quelque chose d'indispensable.",
               'fr_us': "{place}: il y a quelque chose d'indispensable.", 'es': '{place}: hay algo imprescindible.',
               'it': "{place}: c'è qualcosa di indispensabile.", 'nl': '{place}: hier ligt iets wat je nodig hebt.',
               'ru': '{place}: здесь есть кое-что нужное.', 'ja': '{place}： 必要な物が ある。',
               'ko': '{place}: 필요한 것이 있다.', 'zh_cn': '{place}：有需要的东西。', 'zh_tw': '{place}：有需要的東西。'},
    'junk': {'de': '{place}: nichts Besonderes.', 'fr': '{place} : rien de spécial.', 'fr_us': '{place}: rien de spécial.',
             'es': '{place}: nada especial.', 'it': '{place}: niente di speciale.', 'nl': '{place}: niets bijzonders.',
             'ru': '{place}: ничего особенного.', 'ja': '{place}： たいした物は ない。', 'ko': '{place}: 별것 없다.',
             'zh_cn': '{place}：没什么特别的。', 'zh_tw': '{place}：沒什麼特別的。'},
    'count': {'de': 'Dinge, die du hier brauchst: {n}.', 'fr': 'Objets indispensables ici : {n}.',
              'fr_us': 'Objets indispensables ici: {n}.', 'es': 'Objetos imprescindibles aquí: {n}.',
              'it': 'Oggetti indispensabili qui: {n}.', 'nl': 'Dingen die je hier nodig hebt: {n}.',
              'ru': 'Нужных вещей здесь: {n}.', 'ja': 'ここで 必要な物： {n}個', 'ko': '여기서 필요한 것: {n}개',
              'zh_cn': '这里需要的东西：{n}件', 'zh_tw': '這裡需要的東西：{n}件'},
    'count0': {'de': 'Hier gibt es nichts, was du brauchst.', 'fr': "Rien d'indispensable ici.",
               'es': 'Aquí no hay nada imprescindible.', 'it': "Qui non c'è niente di indispensabile.",
               'nl': 'Hier ligt niets wat je nodig hebt.', 'ru': 'Здесь нет ничего нужного.',
               'ja': 'ここに 必要な物は ない。', 'ko': '여기에는 필요한 것이 없다.', 'zh_cn': '这里没有需要的东西。',
               'zh_tw': '這裡沒有需要的東西。'},
    'boss-item': {'de': 'Der Endboss hier bewacht: {item}', 'fr': "Le boss d'ici garde : {item}",
                  'fr_us': "Le boss d'ici garde: {item}", 'es': 'El jefe de aquí guarda: {item}',
                  'it': 'Il guardiano di qui custodisce: {item}', 'nl': 'De eindbaas hier bewaakt: {item}',
                  'ru': 'Босс здесь охраняет: {item}', 'ja': 'ここの ボスが 持つ物： {item}',
                  'ko': '이곳 보스가 지닌 것: {item}', 'zh_cn': '这里的首领守着：{item}', 'zh_tw': '這裡的首領守著：{item}'},
    'boss-need': {'de': 'Der Endboss hier bewacht etwas, was du brauchst.',
                  'fr': "Le boss d'ici garde quelque chose d'indispensable.",
                  'es': 'El jefe de aquí guarda algo imprescindible.',
                  'it': 'Il guardiano di qui custodisce qualcosa di indispensabile.',
                  'nl': 'De eindbaas hier bewaakt iets wat je nodig hebt.', 'ru': 'Босс здесь охраняет кое-что нужное.',
                  'ja': 'ここの ボスは 必要な物を 持っている。', 'ko': '이곳 보스는 필요한 것을 지니고 있다.',
                  'zh_cn': '这里的首领守着需要的东西。', 'zh_tw': '這裡的首領守著需要的東西。'},
    'boss-junk': {'de': 'Der Endboss hier bewacht nichts, was du brauchst.',
                  'fr': "Le boss d'ici ne garde rien d'indispensable.",
                  'es': 'El jefe de aquí no guarda nada imprescindible.',
                  'it': 'Il guardiano di qui non custodisce niente di indispensabile.',
                  'nl': 'De eindbaas hier bewaakt niets wat je nodig hebt.', 'ru': 'Босс здесь не охраняет ничего нужного.',
                  'ja': 'ここの ボスは 必要な物を 持っていない。', 'ko': '이곳 보스는 필요한 것을 지니고 있지 않다.',
                  'zh_cn': '这里的首领没有守着需要的东西。', 'zh_tw': '這裡的首領沒有守著需要的東西。'},
    'trade': {'de': 'Tauschkette', 'fr': 'Quête des échanges', 'es': 'Cadena de intercambios', 'it': 'Catena di scambi',
              'nl': 'Ruilketen', 'ru': 'Цепочка обменов', 'ja': 'わらしべ交換', 'ko': '물물교환', 'zh_cn': '以物易物',
              'zh_tw': '以物易物'},
}


def _place_en(h, via):
    a = h.get('area') or h.get('dungeon')
    if a in hs.DUNGEON:
        text = hs.DUNGEON[a]
        key = next((k for k, v in hs.DUNGEON_KEY.items() if v == a), None)
        if via.get(key) and via[key] != key:
            text += f" (through {hs.DUNGEON[hs.DUNGEON_KEY[via[key]]].removeprefix('the ')}'s entrance)"
        return text
    return hs.AREA.get(a) or hs.AREA.get(a + '-east') or a.replace('-', ' ').title()


def english(h, via, ent_doors=None):
    """-> [(text, red)]"""
    def cap(s):
        return s[:1].upper() + s[1:]
    k = h['kind']
    if k == 'barren':
        return [(f'Nothing you need lies in {_place_en(h, via)}.', False)]
    if k == 'needed':
        return [(f'Something you need lies in {_place_en(h, via)}.', False)]
    if k == 'quest':
        q = cap(QUEST_EN[h['quest']])
        if h['says'] == 'need':
            return [(f'{q} holds something you need.', False)]
        if h['says'] == 'junk':
            return [(f'{q} holds nothing of note.', False)]
        return [(f'{q} holds ', False), (hs.item_phrase(h['item']), True), ('.', False)]
    if k == 'item':
        verb = ' lie ' if h['item'] in hs.PLURAL else ' lies '
        where = f'in {_place_en(h, via)}'
        door = (ent_doors or {}).get(h.get('door'))
        if door:  # (a shuffled interior: the door that leads there now)
            name = hs.door_name(door)
            where += ', in a cave' if name == 'a cave' else f', behind the door of {name}'
        return [(cap(hs.item_phrase(h['item'])), True), (verb + where + '.', False)]
    if k == 'count':
        n = h['n']
        if not n:
            return [('Nothing in this dungeon is needed.', False)]
        return [('This dungeon holds one thing you need.' if n == 1 else f'This dungeon holds {n} things you need.', False)]
    if k == 'boss':
        if h['says'] == 'need':
            return [('The boss here guards something you need.', False)]
        if h['says'] == 'junk':
            return [('The boss here guards nothing you need.', False)]
        return [('The boss here guards ', False), (hs.item_phrase(h['item']), True), ('.', False)]
    raise ValueError(k)


PAGE = ('\f', False)  # (a segment that starts a new page)


def localized(h, via, names, lang):
    """-> [(text or game bytes, red)] in `lang`: our sentence, the game's names. Chinese breaks lines anywhere, so the
    game's names go in as text there; the way into a shuffled dungeon gets its own page in the full-width scripts."""
    segs = _localized(h, via, names, lang)
    if lang in lt.CHAR_WRAP:
        segs = [(t.decode() if isinstance(t, bytes) and b'\x0e' not in t else t, r) for t, r in segs]
    return segs


def _localized(h, via, names, lang):
    import loc_text as lt
    cjk = lang in ('ja', 'zh_cn', 'zh_tw')

    def up(b):
        t = b.decode('utf-8', 'surrogateescape')
        return (t[:1].upper() + t[1:]).encode('utf-8', 'surrogateescape')

    def place():
        a = h.get('area') or h.get('dungeon')
        if a in hs.DUNGEON:
            dn = hs.dungeon_name(a, lang, names)
            seg = [(dn if isinstance(dn, bytes) else dn.encode(), False)]
            key = next((k for k, v in hs.DUNGEON_KEY.items() if v == a), None)
            if via.get(key) and via[key] != key:
                vn = hs.dungeon_name(hs.DUNGEON_KEY[via[key]], lang, names)
                way[:] = [(('（' if cjk else ' (') + lt.tr(lt.DETAIL['entrance'], lang) + lt.T['colon'].get(lang, ': '), False),
                        (vn if isinstance(vn, bytes) else vn.encode(), False), ('）' if cjk else ')', False)]
            return seg
        label = lt.AREA_LABEL.get(a) or lt.AREA_LABEL.get(a + '-east')
        return [(names.place(label) or _place_en(h, via).encode(), False)]
    way = []  # (a shuffled dungeon's way in: after the place, or its own last page in the full-width scripts)

    def tail():
        return ([PAGE] + way if cjk else []) if way else []

    def quest():
        q = h['quest']
        if q == 'trade':
            return [(lt.tr(T['trade'], lang), False)]
        return [(names.place(QUEST_LABEL[q]) or QUEST_EN[q].encode(), False)]

    def sentence(key, p):
        tpl = lt.tr(T[key], lang)
        pre, post = tpl.split('{place}')
        p = list(p)
        if not pre:  # (the name opens the sentence)
            p[0] = (up(p[0][0]) if isinstance(p[0][0], bytes) else p[0][0][:1].upper() + p[0][0][1:], p[0][1])
        if way and not cjk:
            p += way
        return ([(pre, False)] if pre else []) + p + [(post, False)] + tail()

    def item_colon(p):
        colon = {'fr': ' : ', 'ja': '： '}.get(lang) or lt.T['colon'].get(lang, ': ')  # (ja: a line may break there)
        return [(up(hs.item_name(h['item'], names, lang)), True), (colon, False)] + p + (way if not cjk else tail())
    k = h['kind']
    if k in ('barren', 'needed'):
        return sentence(k, place())
    if k == 'quest':
        if h['says'] == 'item':
            return item_colon(quest())
        return sentence('needed' if h['says'] == 'need' else 'junk', quest())
    if k == 'item':
        return item_colon(place())
    if k == 'count':
        if not h['n']:
            return [(lt.tr(T['count0'], lang), False)]
        return [(lt.tr(T['count'], lang).format(n=h['n']), False)]
    if k == 'boss':
        if h['says'] == 'item':
            return lt.fill(lt.tr(T['boss-item'], lang), item=(hs.item_name(h['item'], names, lang), True))
        return [(lt.tr(T['boss-need' if h['says'] == 'need' else 'boss-junk'], lang), False)]
    raise ValueError(k)


# ------------------------------------------------------------------ seed files
OWL_LABEL = 'LAROwl_{}'      # Hint.msbt: a dungeon statue's message (entry without 'examine_')
PHONE_LABEL = 'LARHint{}'    # Telephone.msbt


def _flow_path(out_dir, romfs, name):
    return hs._src(out_dir, romfs, f'region_common/event/{name}')


def _examine_of(fc, entry, event_tools):
    """The DungeonOwlStatue.Examine event an entry shows (HasItem -> fork -> Examine)."""
    ep = event_tools.findEntryPoint(fc, entry)
    assert ep is not None, entry
    sw = ep.main_event.v
    fork = sw.data.cases[1].v
    for f in fork.data.forks:
        ev = f.v
        d = ev.data
        if getattr(d, 'actor_action', None) and d.actor_action.v.v == 'Examine':
            return ev
    raise AssertionError(f'no Examine under {entry}')


def _owl_flows(out_dir, romfs, hints, event_tools):
    """DungeonOwlStatue.bfevfl: every statue its own message; the split entries are built like their shared ones."""
    p = _flow_path(out_dir, romfs, 'DungeonOwlStatue.bfevfl')
    flow = event_tools.readFlow(p)
    fc = flow.flowchart
    told = {h['where'][8:] for h in hints if h['where'].startswith('dungeon:')}
    for entry in DUNGEON_OWLS:
        if entry not in told:
            continue
        msg = f'Hint:{OWL_LABEL.format(entry[8:])}'
        if entry in SPLIT:
            assert event_tools.findEntryPoint(fc, entry) is None, entry
            shared = _examine_of(fc, SPLIT[entry], event_tools)
            assert shared.data.params.data['message'].startswith('Hint:'), entry
            ed = event_tools.createSubFlowEvent(fc, '', 'Beak_ed', {})
            st = event_tools.createSubFlowEvent(fc, '', 'Beak_st', {})
            ex = event_tools.createActionEvent(fc, 'DungeonOwlStatue', 'Examine', {'message': msg})
            wait = event_tools.createActionEvent(fc, 'Link', 'PlayAnimation', {'name': 'wait', 'blendTime': 0.1})
            fork, _ = event_tools.createForkEvent(fc, None, [st, ex, wait], ed)
            nob = event_tools.createSubFlowEvent(fc, '', 'examine_NoBeak', {})
            sw = event_tools.createSwitchEvent(fc, 'Inventory', 'HasItem', {'itemType': 63, 'count': 1}, {0: nob, 1: fork})
            event_tools.addEntryPoint(fc, entry)
            event_tools.insertEventAfter(fc, entry, sw)
        else:
            ev = _examine_of(fc, entry, event_tools)
            assert ev.data.params.data['message'].startswith('Hint:Lv'), (entry, ev.data.params.data['message'])
            ev.data.params.data['message'] = msg
    event_tools.writeFlow(p, flow)


def _split_rooms(out_dir, romfs, hints, leb):
    """The split statues' rooms (wherever the room shuffle put them) point their actor at the new entry (asserted:
    one statue there using the shared entry)."""
    n = 0
    for h in hints:
        entry = h['where'][8:]
        if not h['where'].startswith('dungeon:') or entry not in SPLIT:
            continue
        cell = h['room']
        rel = f"region_common/level/{cell.rsplit('_', 1)[0]}/{cell}.leb"
        r = leb.Room(hs._read(out_dir, romfs, rel))
        found = [a for a in r.actors if a.name.startswith(STATUE_ACTOR) and a.parameters[0] == SPLIT[entry].encode()]
        assert len(found) == 1, (cell, entry, len(found))
        found[0].parameters[0] = entry.encode()
        out = os.path.join(out_dir, 'romfs', rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(r.repack())
        n += 1
    return n


def _phone_flow(out_dir, romfs, n, event_tools, oead_tools):
    """Telephone.bfevfl: hint i when flags 0..i-1 are set and flag i isn't; the last hint clears them, so the next
    call starts over. With LAS's tunic swap (after the call's join, Event98): just before it, shown with Dialog.Show
    (LAS names the phone's speaker the Fairy Queen). Without it (Treat Tunics as Upgrades: the vanilla call): in place
    of the game's own hint pick (Event218 -> Event97 JumpToAppropriateHintEntry), Ulrira speaking (Telephone.Examine)."""
    import seed_tables
    rel = 'region_common/datasheets/GlobalFlags.gsheet'
    sp = hs._src(out_dir, romfs, rel)
    sheet = oead_tools.readSheet(sp)
    flags = [PHONE_FLAG.format(i) for i in range(n - 1)]
    for fl in flags:
        sheet['values'].append({'symbol': fl, 'index': seed_tables.next_flag(sheet, out_dir)})
    oead_tools.writeSheet(sp, sheet)
    p = _flow_path(out_dir, romfs, 'Telephone.bfevfl')
    flow = event_tools.readFlow(p)
    fc = flow.flowchart
    after = event_tools.findEvent(fc, 'Event98').data.nxt.v
    if type(after.data).__name__ == 'SubFlowEvent' and after.data.entry_point_name == 'Telephone':  # (the tunic swap)
        before, end, show = 'Event98', after.name, ('Dialog', 'Show')
    else:
        pick = event_tools.findEvent(fc, 'Event97')
        assert pick.data.actor_action.v.v == 'JumpToAppropriateHintEntry', 'Telephone.bfevfl changed'
        assert event_tools.findEvent(fc, 'Event218').data.nxt.v is pick, 'Telephone.bfevfl changed'
        before, end, show = 'Event218', None, ('Telephone', 'Examine')
    nxt = end
    for fl in flags:  # (the last hint: clear every flag)
        nxt = event_tools.createActionEvent(fc, 'EventFlags', 'SetFlag', {'symbol': fl, 'value': False}, nxt)
    nxt = event_tools.createActionEvent(fc, *show, {'message': f'Telephone:{PHONE_LABEL.format(n - 1)}'}, nxt)
    for i in reversed(range(n - 1)):
        give = event_tools.createActionEvent(fc, 'EventFlags', 'SetFlag', {'symbol': flags[i], 'value': True}, end)
        give = event_tools.createActionEvent(fc, *show, {'message': f'Telephone:{PHONE_LABEL.format(i)}'}, give)
        nxt = event_tools.createSwitchEvent(fc, 'EventFlags', 'CheckFlag', {'symbol': flags[i]}, {0: give, 1: nxt})
    event_tools.insertEventAfter(fc, before, nxt)
    event_tools.writeFlow(p, flow)


def phone_opening(raw):
    """The game's own call opening (dial sound, 'BRRING! ...', Ulrira's voice) from a Telephone.msbt `*2` label:
    everything up to Ulrira's voice, the hint starting on a new page."""
    import keysanity_text as kt
    import loc_text as lt
    toks = kt._tokens(raw)
    i = next(i for i, (k, g, r) in enumerate(toks) if k == 'tag' and b'GRANDFATHER' in r)
    out = b''
    for j, (k, g, r) in enumerate(toks[:i + 1]):
        if k == 'text' and r == b'\n' and j and toks[j - 1][1] == kt.WAIT:  # (CJK: a new line, not a new page)
            r = lt.PAGE
        out += r
    return out


def apply(out_dir, romfs, settings):
    from RandomizerCore.Tools import event_tools, oead_tools, leb
    import keysanity_text as kt
    import loc_text as lt
    p = os.path.join(out_dir, 'seed_hints.json')
    if not os.path.exists(p):
        return 'no hints'
    with open(p) as f:
        data = json.load(f)
    hints, via = data['hints'], data.get('entered-via') or {}
    import entrance_logic as el
    ed = el.load()
    doors = {d['id']: d for d in ed['doors']}
    field = {h['where'][6:]: h for h in hints if h['where'].startswith('field:')}
    dungeon = {h['where'][8:]: h for h in hints if h['where'].startswith('dungeon:')}
    phone = [h for h in sorted((h for h in hints if h['where'].startswith('phone:')), key=lambda h: int(h['where'][6:]))]
    _owl_flows(out_dir, romfs, hints, event_tools)
    rooms = _split_rooms(out_dir, romfs, hints, leb)
    if phone:
        _phone_flow(out_dir, romfs, len(phone), event_tools, oead_tools)
    nfiles = 0
    for hp in sorted(glob.glob(os.path.join(romfs, 'region*', '*', 'message', 'Hint.msbt'))):
        rel = os.path.relpath(hp, romfs)
        lang = lt.lang_of(rel)
        hm = Msbt(hs._read(out_dir, romfs, rel))
        tel_rel = rel.replace('Hint.msbt', 'Telephone.msbt')
        tm = Msbt(hs._read(out_dir, romfs, tel_rel))
        wait = next((r for k, g, r in reversed(kt._tokens(hm.texts['NoBeakStoneOwl'])) if g == kt.WAIT), b'')
        names = None if lang == 'en' else lt.Names(out_dir, romfs, rel.replace('Hint.msbt', 'System.msbt'))

        def text(h):
            return english(h, via, doors) if lang == 'en' else localized(h, via, names, lang)

        def msg(h, head=b''):
            return head + lt.pages(paged(text(h)), wait, lang)
        for entry, h in field.items():
            hm.set(FIELD_OWLS[entry], msg(h))
        for entry, h in dungeon.items():
            hm.set(OWL_LABEL.format(entry[8:]), msg(h))
            hm.copy_attr(OWL_LABEL.format(entry[8:]), 'Lv1StoneOwl1')
        opening = phone_opening(tm.texts['TelephonGoldLeafCrow2'])
        quote = opening.split(b'SE_ENV_ULULIRA_DIAL', 1)[1][:1] == b'"'
        for i, h in enumerate(phone):
            seg = text(h)
            if quote:
                seg = [('"', False)] + seg + [('"', False)]
            tm.set(PHONE_LABEL.format(i), opening + lt.pages(paged(seg), wait, lang))
            tm.copy_attr(PHONE_LABEL.format(i), 'TelephonGoldLeafCrow2')  # (Ulrira speaking)
        for r, m in ((rel, hm), (tel_rel, tm)):
            out = os.path.join(out_dir, 'romfs', r)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'wb') as f:
                f.write(m.pack())
        nfiles += 2
    with open(os.path.join(out_dir, 'seed_hints.txt'), 'w') as f:  # (readable: the spoiler's Hints section)
        for h in hints:
            f.write(f"{where_name(h)}: {''.join(t for t, _ in english(h, via, doors))}\n")
    return f'{len(field)} field statues, {len(dungeon)} dungeon statues, {len(phone)} phone hints, {rooms} rooms, {nfiles} message files'


FIELD_NAMES = {'examine_tarutaru': 'Owl statue, Tal Tal Mountains (west)', 'examine_sannmyaku': 'Owl statue, Tal Tal Mountains (east)',
               'examine_kouti': 'Owl statue, Tal Tal Heights', 'examine_ride': 'Owl statue, Rapids Ride',
               'examine_kao': 'Owl statue, Face Shrine', 'examine_ukuku': 'Owl statue, Ukuku Prairie',
               'examine_anaboko': 'Owl statue, Pothole Field', 'examine_yana': 'Owl statue, Yarna Desert',
               'examine_area05': "Owl statue, Martha's Bay"}


def paged(segs):
    """segments -> pages of segments (split at PAGE)"""
    pages = [[]]
    for sg in segs:
        if sg == PAGE:
            pages.append([])
        else:
            pages[-1].append(sg)
    return [p for p in pages if p]


def where_name(h):
    kind, key = h['where'].split(':', 1)
    if kind == 'field':
        return FIELD_NAMES[key]
    if kind == 'phone':
        return f'Phone call {int(key) + 1}'
    lv, cell = h['room'].rsplit('_', 1)
    return f"Owl statue, {hs.DUNGEON[hs.LEVEL_DUNGEON[lv]].removeprefix('the ')} {cell}"
