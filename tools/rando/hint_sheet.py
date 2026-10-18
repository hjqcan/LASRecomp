#!/usr/bin/env python3
"""Hint Sheet (runtime/hle/hint_sheet.c): every randomizer seed starts with the game's FIRST bottle (Bottle index 0) as
a hint item. Tarin hands it over right after the Escape Bottle (Tarin.bfevfl, after Event36), held up as the dungeon
map's model (a dummy Items.gsheet row `HintSheet`, npcKey ItemDungeonMap) with its own get text. Used on X / Y, the
game's message box names where the player's next progression item lies.

Why the first bottle, not the second: LAS's fishing pond fix owns the second one (its ASM turns Inventory.RemoveItem(0)
into "take Bottle[1] away", and Fisherman.bfevfl adds / removes it around every fishing game), so a Hint Sheet there
would vanish at the pond. The one real bottle left in the pool is always the second (las_headless.py).

Generation (steps(), called right after the seed's playthrough check): a step is an item copy without which the
playthrough (LAS's logic) reaches fewer real checks or not the Wind Fish, and only when what it opens holds more
progression (another step's item) or it is needed for the Wind Fish. Order: by the sphere where the first
progression it opens can be reached, then by how much progression it opens, then playthrough order; steps for the
Wind Fish alone last. Seashells and Golden Leaves are steps only on demand: the runtime shows them when the next
real step's place can't be reached without them (hints.tsv `needs`: how many) and the player has fewer. Each step
gets its own message (System.msbt, every language: our own English text) and a Common.bfevfl entry showing it;
hints.tsv lists them for the runtime, which shows the first step whose check the player hasn't collected yet (the
check's event flag from tracker.tsv; for items the save shows directly, owning it counts too), or the closing message
once all are.

    steps(logic_defs, placements, settings, check, sph, out_dir) -> number of steps (writes hint_steps.json)
    apply(out_dir, romfs, settings) -> summary (flows, messages, item row, hints.tsv)
"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from msbt import Msbt  # noqa: E402

NAME = 'Hint Sheet'
ENTRY = 'HintSheet'           # Items.gsheet row, Item.bfevfl entry, System.msbt get message
DONE = 'HintSheetDone'        # Common.bfevfl entry + System.msbt message once every step is found
INDEX = 0                     # bottle index (inventory bit 13, item slot 0xd)
MODEL_NPC = 'ItemDungeonMap'  # Npc.gsheet row: the dungeon map's model, held up while Tarin hands it over
LINE = 34                     # characters per message box line (two lines per page)

_RED = b'\x0e\x00\x00\x03\x00\x04\x00\xd9\x45\x6d\xff'  # colour tags: the game's item-name red / plain white
_WHITE = b'\x0e\x00\x00\x03\x00\x04\x00\xed\xed\xed\xff'
_PAGE = b'\x0e\x00\x00\x04\x00\x00\x00'

JUNK = re.compile(r'^((pot-|dig-)?rupee-\d+|pot-heart|pot-arrows|pot-bombs|pot-powder|.*-trap|arrows?(-\d+)?|'
                  r'bombs?-\d+|powder-\d+|heart|fairy)$')


GATED = ('seashell', 'golden-leaf')  # (steps shown only when the next real step needs them: steps())


def step_entry(i):
    return f'HintSheet{i:02d}'


def base_item(item):
    return re.sub(r'\[\d+\]$', '', item or '')


# ------------------------------------------------------------------ generation: which items are steps
def _reach_fn(logic_defs, settings, check):
    """-> reach(placements): (the real checks a playthrough gets to, how much of the goal). The final state of
    playthrough.spheres (the same nodes, enemies and item counts), found as one fixpoint driven by what each logic node
    reads: a node is checked again only when a token of its conditions changes (much faster than sphere by sphere)."""
    import playthrough
    junk = set(settings.get('excluded-locations', ())) | set(settings.get('absent-locations', ()))
    goal = set(playthrough.INSTRUMENTS) | {'kill-nightmare'}
    word = re.compile(r'[A-Za-z0-9][A-Za-z0-9\-\[\]]*')
    readers = {}  # token -> nodes whose conditions read it
    for k, node in logic_defs.items():
        toks = set()
        for ck, cv in node.items():
            if ck.startswith('condition-') and isinstance(cv, str):
                toks.update(word.findall(cv))
        if node.get('region'):
            toks.add(node['region'])
        for t in toks:
            readers.setdefault(t, []).append(k)
    keys = list(logic_defs)

    def reach(pl, extra=None):
        access = {k: 1 for k, v in settings.items() if v is True}
        for k, v in (extra or {}).items():
            access[k] = access.get(k, 0) + v
        seen = set()
        todo, queued = list(reversed(keys)), set(keys)

        def give(tok, n=1):
            access[tok] = access.get(tok, 0) + n
            for r in readers.get(tok, ()):
                if r not in queued and r not in seen:
                    queued.add(r)
                    todo.append(r)
        while todo:
            k = todo.pop()
            queued.discard(k)
            if k in seen:
                continue
            node = logic_defs[k]
            if node.get('type') == 'enemy':
                try:
                    if check(k, access):
                        seen.add(k)
                        for t in (k, k + '[pit]', k + '[heavy]'):
                            if not access.get(t):
                                give(t)
                        continue
                    for v in ('pit', 'heavy'):
                        t = f'{k}[{v}]'
                        if f'condition-{v}' in node and not access.get(t) and check(t, access):
                            give(t)
                except Exception:
                    pass
                continue
            try:
                ok = check(k, access)
            except Exception:
                ok = False
            if not ok:
                continue
            seen.add(k)
            give(k)
            item = pl.get(k) if node.get('type') in ('item', 'follower') else None
            if item:
                give(item)
                b = base_item(item)
                if b != item:
                    give(b)
        return (frozenset(k for k in seen if logic_defs[k].get('type') == 'item' and k not in junk),
                sum(1 for g in goal if access.get(g)))
    return reach


def steps(logic_defs, placements, settings, check, sph, out_dir):
    """The progression steps of a seed in hint order -> hint_steps.json. `check(key, access)` and the starting access
    (the seed's boolean settings) as in las_headless's playthrough check; `sph` = its spheres.

    A step is an item copy without which the playthrough reaches fewer real checks (or the goal). Its effect = the
    checks lost without it. Order (user 2026-10-18: the sheet points at what opens checks now): steps that open checks
    first, by the sphere of the first check they open (ties: more checks first, then playthrough order); steps that
    only matter for the Wind Fish (the Ballad, instruments) last, in playthrough order."""
    import time
    t0 = time.time()
    runs = [0]
    _reach = _reach_fn(logic_defs, settings, check)
    memo = {}

    def without(locs):
        key = tuple(sorted(locs))
        if key not in memo:
            pl = dict(placements)
            for loc in locs:
                pl[loc] = None
            runs[0] += 1
            memo[key] = _reach(pl)
        return memo[key]
    full = without(())
    if os.environ.get('LARECOMP_HINT_VERIFY'):  # (test: the fast fixpoint against the playthrough itself)
        import playthrough
        junk = set(settings.get('excluded-locations', ())) | set(settings.get('absent-locations', ()))
        acc = {k: 1 for k, v in settings.items() if v is True}
        _, acc, seen = playthrough.spheres(logic_defs, placements, acc, check)
        slow = (sum(1 for k in seen if logic_defs[k].get('type') == 'item' and k not in junk),
                sum(1 for g in set(playthrough.INSTRUMENTS) | {'kill-nightmare'} if acc.get(g)))
        print('hint sheet reach check:', (len(full[0]), full[1]), 'fast vs', slow, 'playthrough', flush=True)
    sphere_of = {loc: i for i, s in enumerate(sph) for loc, _ in s}
    order = [(loc, item) for s in sph for loc, item in s if not loc.startswith('starting-')]
    cand = [(loc, item) for loc, item in order if not JUNK.match(base_item(item))]
    groups = {}
    for loc, item in cand:
        groups.setdefault(base_item(item), []).append(loc)
    # (an item no logic condition reads can't matter: no playthrough needed - but the instruments open the egg)
    import playthrough
    goal_items = set(playthrough.INSTRUMENTS)
    read = set()
    word = re.compile(r'[A-Za-z0-9][A-Za-z0-9\-\[\]]*')
    for node in logic_defs.values():
        for ck, cv in node.items():
            if ck.startswith('condition-') and isinstance(cv, str):
                read.update(re.sub(r'\[\d+\]$', '', w) for w in word.findall(cv))
    effect = {}  # step location -> reach without it (or without it and the copies after it: copies that count together)
    for b, locs in groups.items():
        if b not in read and b not in goal_items and not any(i in read for l in locs for i in [placements.get(l)]):
            continue
        if without(locs) == full:  # (no copy of it matters: not progression)
            continue
        if len(locs) == 1:
            effect[locs[0]] = without(locs)
            continue
        if len(locs) > 12:  # (many copies - seashells, leaves: the first k found are the ones that count)
            lo, hi = 1, len(locs)
            while lo < hi:
                mid = (lo + hi) // 2
                if without(locs[mid:]) == full:
                    hi = mid
                else:
                    lo = mid + 1
            for i in range(lo):
                effect[locs[i]] = without(locs[i:])
            continue
        single = [loc for loc in locs if without([loc]) != full]
        for loc in single:
            effect[loc] = without([loc])
        if not single:  # (only together: the first copy found)
            effect[locs[0]] = without(locs)
    count, out = {}, []
    for i, (loc, item) in enumerate(order):
        b = base_item(item)
        count[b] = count.get(b, 0) + 1
        if loc in effect:
            lost = full[0] - effect[loc][0]
            out.append({'loc': loc, 'item': b, 'n': count[b], 'region': logic_defs.get(loc, {}).get('spoiler-region', ''),
                        'opens': len(lost), 'goal': effect[loc][1] < full[1], 'order': i, '_lost': lost})
    # Seashells / Golden Leaves (user 2026-10-18): shown only when the next real step is behind them (a Seashell
    # Mansion reward, what Richard's leaves open) and the player still lacks some - the runtime decides from `needs`.
    # They go last, in playthrough order; every other step lists the gated groups it can't be reached without.
    for g in GATED:
        locs = groups.get(g, [])
        if not any(loc in effect for loc in locs):
            continue
        # (directly: with every other item already in hand, the step's place still can't be reached without them)
        pl = dict(placements)
        others = {}
        for loc, item in order:
            if base_item(item) != g:
                pl[loc] = None
                others[item] = others.get(item, 0) + 1
                if base_item(item) != item:
                    others[base_item(item)] = others.get(base_item(item), 0) + 1
        for loc in locs:
            pl[loc] = None
        runs[0] += 1
        reach_g = _reach(pl, others)[0]
        for st in out:
            if st['item'] not in GATED and st['loc'] in full[0] and st['loc'] not in reach_g:
                lo, hi = 1, len(locs)  # (how many it takes)
                while lo < hi:
                    mid = (lo + hi) // 2
                    runs[0] += 1
                    if st['loc'] in _reach(pl, dict(others, **{g: others.get(g, 0) + mid}))[0]:
                        hi = mid
                    else:
                        lo = mid + 1
                st.setdefault('needs', []).append(f'{g}:{lo}')
    # Worth a hint (user 2026-10-18: "if something only opens ONE check, that one check better be the next thing they
    # need"): a step whose lost checks hold another kept step's item (progression), or that the Wind Fish needs.
    # Dropping one can leave another without progression behind it: repeat until nothing changes. Seashells / leaves
    # count while a kept real step needs them.
    keep = {st['loc'] for st in out}
    while True:
        needed = {n.split(':')[0] for st in out if st['loc'] in keep and st['item'] not in GATED for n in st.get('needs', ())}
        prog = {st['loc'] for st in out if st['loc'] in keep and (st['item'] not in GATED or st['item'] in needed)}
        new_keep = {st['loc'] for st in out if st['loc'] in prog and (st['goal'] or (st['_lost'] & prog) - {st['loc']})}
        if new_keep == keep:
            break
        keep = new_keep
    dropped = [st for st in out if st['loc'] not in keep]
    out = [st for st in out if st['loc'] in keep]
    for st in out:  # what it opens that matters: the progression behind it, and when the first of it can be reached
        behind = sorted((st['_lost'] & keep) - {st['loc']}, key=lambda c: (sphere_of.get(c, 999), c))
        st['unlocks'] = len(behind)
        st['first'] = sphere_of.get(behind[0], 999) if behind else 999
        st['unlocks-example'] = behind[:3]
        del st['_lost']
    out.sort(key=lambda st: (st['item'] in GATED, not st['unlocks'], st['first'], -st['unlocks'], st['order']))
    if dropped:
        print('hint sheet: no hint for', ', '.join(f"{st['item']} ({st['loc']})" for st in dropped[:12]) +
              (' ...' if len(dropped) > 12 else ''), flush=True)
    with open(os.path.join(out_dir, 'hint_steps.json'), 'w') as f:
        json.dump({'steps': out, 'dungeon-entrances': dict(placements.get('dungeon-entrances') or {})}, f, indent=1)
    print(f'hint sheet: {len(out)} steps ({sum(1 for st in out if not st["unlocks"])} for the goal only) from '
          f'{len(order)} items, {runs[0]} playthroughs in {time.time() - t0:.1f}s', flush=True)
    return len(out)


# ------------------------------------------------------------------ text
DUNGEON = {'D0': 'the Color Dungeon', 'D1': 'Tail Cave', 'D2': 'Bottle Grotto', 'D3': 'Key Cavern',
           'D4': "Angler's Tunnel", 'D5': "Catfish's Maw", 'D6': 'Face Shrine', 'D7': "Eagle's Tower",
           'D8': 'Turtle Rock'}
DUNGEON_KEY = {'color-dungeon': 'D0', 'tail-cave': 'D1', 'bottle-grotto': 'D2', 'key-cavern': 'D3', 'angler-tunnel': 'D4',
               'catfish-maw': 'D5', 'face-shrine': 'D6', 'eagle-tower': 'D7', 'turtle-rock': 'D8'}
LEVEL_DUNGEON = {'Lv10ClothesDungeon': 'D0', 'Lv01TailCave': 'D1', 'Lv02BottleGrotto': 'D2', 'Lv03KeyCavern': 'D3',
                 'Lv04AnglersTunnel': 'D4', 'Lv05CatfishsMaw': 'D5', 'Lv06FaceShrine': 'D6', 'Lv07EagleTower': 'D7',
                 'Lv08TurtleRock': 'D8'}
AREA = {'mabe-village': 'Mabe Village', 'toronbo-shores': 'Toronbo Shores', 'mysterious-woods': 'the Mysterious Woods',
        'koholint-prairie': 'Koholint Prairie', 'tabahl-wasteland': 'Tabahl Wasteland', 'ukuku-prairie': 'Ukuku Prairie',
        'sign-maze': 'the Signpost Maze', 'goponga-swamp': 'Goponga Swamp', 'taltal-heights': 'Tal Tal Heights',
        'marthas-bay': "Martha's Bay", 'kanalet-castle': 'Kanalet Castle', 'pothole-field': 'Pothole Field',
        'animal-village': 'Animal Village', 'yarna-desert': 'Yarna Desert', 'ancient-ruins': 'the Ancient Ruins',
        'rapids-ride': 'Rapids Ride', 'taltal-mountains-east': 'the Tal Tal Mountains',
        'taltal-mountains-west': 'the Tal Tal Mountains'}
# words of a location key already said by its area (left out of the detail)
AREA_WORDS = {'mabe-village': ('mabe',), 'toronbo-shores': ('beach',), 'mysterious-woods': ('woods',),
              'tabahl-wasteland': ('wasteland',), 'ukuku-prairie': ('ukuku',), 'goponga-swamp': ('goponga', 'swamp'),
              'taltal-heights': ('taltal',), 'marthas-bay': ('bay',), 'kanalet-castle': ('kanalet',),
              'pothole-field': ('pothole',), 'animal-village': ('animal', 'village'), 'yarna-desert': ('desert',),
              'rapids-ride': ('rapids',), 'taltal-mountains-east': ('taltal',), 'taltal-mountains-west': ('taltal',)}
ITEM = {'sword': 'a Sword', 'shield': 'a Shield', 'bracelet': 'a Power Bracelet', 'boots': 'the Pegasus Boots',
        'flippers': 'the Flippers', 'feather': "Roc's Feather", 'shovel': 'the Shovel', 'hookshot': 'the Hookshot',
        'bow': 'the Bow', 'rod': 'the Magic Rod', 'boomerang': 'the Boomerang', 'ocarina': 'the Ocarina',
        'song-ballad': 'the Ballad of the Wind Fish', 'song-mambo': "Manbo's Mambo", 'song-soul': 'the Frog Song of Soul',
        'yoshi-doll': 'the Yoshi Doll', 'ribbon': 'the Ribbon', 'dog-food': 'the Dog Food', 'bananas': 'the Bananas',
        'stick': 'the Stick', 'honeycomb': 'the Honeycomb', 'pineapple': 'the Pineapple', 'hibiscus': 'the Hibiscus',
        'letter': 'the Letter', 'broom': 'the Broom', 'fishing-hook': 'the Fishing Hook', 'necklace': 'the Necklace',
        'mermaid-scale': "the Mermaid's Scale", 'magnifying-lens': 'the Magnifying Lens', 'golden-leaf': 'a Golden Leaf',
        'mushroom': 'the Sleepy Toadstool', 'tail-key': 'the Tail Key', 'slime-key': 'the Slime Key',
        'angler-key': 'the Angler Key', 'face-key': 'the Face Key', 'bird-key': 'the Bird Key',
        'full-moon-cello': 'the Full Moon Cello', 'conch-horn': 'the Conch Horn', 'sea-lilys-bell': "the Sea Lily's Bell",
        'surf-harp': 'the Surf Harp', 'wind-marimba': 'the Wind Marimba', 'coral-triangle': 'the Coral Triangle',
        'evening-calm-organ': 'the Evening Calm Organ', 'thunder-drum': 'the Thunder Drum',
        'seashell': 'a Secret Seashell', 'bottle': 'a Bottle', 'heart-container': 'a Heart Container',
        'heart-piece': 'a Piece of Heart', 'bomb-capacity': 'a Bomb Bag upgrade', 'arrow-capacity': 'a Quiver upgrade',
        'powder-capacity': 'a Magic Powder upgrade', 'shell-sensor': 'the Seashell Sensor', 'medicine': 'Secret Medicine',
        'red-tunic': 'the Red Tunic', 'blue-tunic': 'the Blue Tunic', 'chamber-stone': 'a Chamber Stone',
        'bomb': 'the Bombs', 'powder': 'the Magic Powder', 'stalfos-note': "Master Stalfos' Note",
        'rooster': 'the Flying Rooster', 'bow-wow': 'BowWow', 'ghost': 'the Ghost', 'marin-companion': 'Marin'}
DUNGEON_ITEM = {'key': 'a Small Key', 'nightmare-key': 'the Nightmare Key', 'compass': 'the Compass', 'map': 'the Dungeon Map',
                'stone-beak': 'the Stone Beak'}
# whole-location wording: gifts, games and other places the location key doesn't say well
PLACE = {'tarin': 'with Tarin in Mabe Village', 'marin': 'with Marin in Mabe Village',
         'mamasha': 'with Mamasha in Mabe Village', 'ciao-ciao': 'with Ciao Ciao in Mabe Village',
         'sale': 'with Sale on Toronbo Shores', 'kiki': 'with Kiki the monkey', 'mr-write': 'with Mr. Write',
         'papahl': 'with Papahl on the mountain', 'christine-trade': 'with Christine in Animal Village',
         'christine-grateful': 'with Christine in Animal Village', 'chef-bear': 'with the bear chef in Animal Village',
         'grandma-yahoo': 'with Grandma Yahoo in Animal Village', 'walrus': 'with the walrus in Yarna Desert',
         'manbo': 'with Manbo at his pond', 'mamu': 'with Mamu in the Signpost Maze', 'syrup': 'with Syrup the witch',
         'goriya-trader': 'with the Goriya on Toronbo Shores', 'mermaid-martha': 'with Martha the mermaid',
         'bay-fisherman': "with the fisherman in Martha's Bay", 'invisible-zora': 'with the hidden Zora',
         'tarin-ukuku': 'with Tarin in Ukuku Prairie', 'washed-up': 'washed up on Toronbo Shores',
         'ghost-reward': "as the Ghost's thanks", 'rooster-statue': 'at the rooster statue in Mabe Village',
         'mabe-well': 'down the well in Mabe Village', 'mabe-bushes': 'among the bushes of Mabe Village',
         'dream-shrine-left': 'in the Dream Shrine', 'dream-shrine-right': 'in the Dream Shrine',
         'lanmola': 'with Lanmola in Yarna Desert', 'armos-knight': 'with the Armos Knight in the Ancient Ruins',
         'kanalet-crow': 'with the crow at Kanalet Castle', 'kanalet-mad-bomber': 'with the bomber at Kanalet Castle',
         'mad-batter-woods': 'with the Mad Batter in the woods', 'mad-batter-bay': "with the Mad Batter of Martha's Bay",
         'mad-batter-taltal': 'with the Mad Batter on the mountain',
         'doghouse-dig': "buried by BowWow's kennel in Mabe Village", 'ghost-grave-dig': "buried by the Ghost's grave",
         'goponga-west-dig': "buried near Mr. Write's house"}


PLURAL = {'boots', 'flippers', 'bomb', 'bananas'}


def item_phrase(item):
    m = re.match(r'^(key|nightmare-key|compass|map|stone-beak)-(D\d)$', item)
    if m:
        return f'{DUNGEON_ITEM[m.group(1)]} for {DUNGEON[m.group(2)]}'
    if item.startswith('soul-'):
        name = item[5:].replace('-', ' ').title()
        return name + ("' Soul" if name.endswith('s') else "'s Soul")
    return ITEM.get(item) or 'the ' + item.replace('-', ' ').title()


def _words(s):
    return re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', s)


# Landmarks for bushes and digs (user 2026-10-18: "buried near the Mabe Village library", "under a bush by Tarin's
# house"): the outsides of houses / caves / dungeons (entrance_data.yml doors, by the level they lead into - an entrance
# shuffle changes what's inside, never the building) and a few Field objects (the screens' .leb actors).
DOOR_NAME = {
    'Lv08TurtleRock': 'Turtle Rock', 'Lv09WindFishsEgg': "the Wind Fish's Egg", 'CuccoHouse': 'the hen house',
    'Lv07EagleTower': "Eagle's Tower", 'MadBattersWell': "the Mad Batter's well", 'FairyFountain': 'the fairy fountain',
    'Lv02BottleGrotto': 'Bottle Grotto', 'MamboCave': "Manbo's cave", 'Lv04AnglersTunnel': "Angler's Tunnel",
    'DrWrightHouse': "Mr. Write's house", 'MoriblinCave': 'the Moblin hideout', 'DanpeiHouse': "Dampe's shack",
    'RapidsRideTicket': 'the raft shop', 'TracyHouse': "Crazy Tracy's house", 'KanaletCastle': 'Kanalet Castle',
    'MagicPowderHouse': "the witch's hut", 'Lv06FaceShrine': 'Face Shrine', 'Lv10ClothesDungeon': 'the Color Dungeon',
    'QuadupletHouse': "the quadruplets' house", 'BedRoom': 'the Dream Shrine', 'SeashellHouse': 'the Seashell Mansion',
    'FlyingCuccoCave': "the rooster's grave", 'Shop': 'the Mabe Village shop', 'DogHouse': "BowWow's kennel",
    'MadamHouse': "Madam MeowMeow's house", 'MarinTarinHouse': "Tarin's house", 'ArmosShrine': 'the Southern Face Shrine',
    'Library': 'the Mabe Village library', 'UlriraHouse': "Ulrira's house", 'CraneGame': 'the Trendy Game',
    'Lv03KeyCavern': 'Key Cavern', 'RabbitBigHouse': "the rabbits' house", 'SchuleDonavitchHouse': "Schule Donavitch's house",
    'ChristineHouse': "Christine's house", 'HiddenZoraHouse': "the Zora's house", 'Lv01TailCave': 'Tail Cave',
    'MamuCave': "Mamu's cave", 'RichardHouse': "Richard's villa", 'Lv05CatfishsMaw': "Catfish's Maw",
    'BearHouse': "the bear chef's house", 'BananaHouse': "Sale's house", 'MermaidStatue': 'the mermaid statue',
    'FishingBoat': "the fisherman's boat", 'GhostHouse': "the Ghost's house", 'TelephoneBox': 'a telephone booth',
    'UnderGrave': 'a grave'}
ACTOR_NAME = {b'ObjSignboard-': 'a signpost', b'ObjFieldOwlS': 'an owl statue', b'ObjWeathercock': 'the weathervane',
              b'ObjGhostsGrave': "the Ghost's grave", b'ObjMarthaSta': 'the mermaid statue', b'ObjWarpManbo': 'a warp hole',
              b'ObjDesertBone': 'the whale bones', b'ObjTabahlTree': 'a lone tree', b'ObjRaft-': 'the raft',
              b'ObjGrave-': 'a gravestone', b'ObjTreasureBox': 'a treasure chest', b'ObjCactus-': 'a cactus',
              b'ObjDesertTree': 'a lone tree'}
GENERIC = ('a ', 'an ')  # (generic landmarks also name the area: "by a signpost in Ukuku Prairie")


def landmarks(romfs, leb):
    """[(name, x, z)] in Field coordinates (a screen is 15 x 12; row r, column c start at c * 15, (r - 1) * 12)"""
    import yaml
    out = []
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entrance_data.yml')) as f:
        for d in yaml.safe_load(f)['doors']:
            lv = d.get('dest_level') or ''
            name = DOOR_NAME.get(lv) or DOOR_NAME.get(re.sub(r'\d+(_.*)?$', '', lv)) or 'a cave'
            out.append((name, d['pos_xz'][0], d['pos_xz'][1]))
    for f in sorted(glob.glob(os.path.join(romfs, 'region_common', 'level', 'Field', 'Field_*.leb'))):
        with open(f, 'rb') as fh:
            room = leb.Room(fh.read())
        for a in room.actors:
            for k, name in ACTOR_NAME.items():
                if a.name.startswith(k):
                    out.append((name, a.posX, a.posZ))
    return out


def tile_xz(room, tile):
    """centre of tile 0-79 (10 x 8 of 1.5) of Field screen `room` ('12C' or 'Field_12C')"""
    m = re.search(r'(\d\d)([A-P])$', room)
    row, col = int(m.group(1)), ord(m.group(2)) - 65
    return col * 15 + (tile % 10) * 1.5 + 0.75, (row - 1) * 12 + (tile // 10) * 1.5 + 0.75


def nearest(marks, x, z):
    """the landmark on the same screen closest to (x, z), else one just over the screen edge (within 6), else None"""
    sx, sz = int(x // 15), int(z // 12)
    best = None
    for name, lx, lz in marks:
        d = ((lx - x) ** 2 + (lz - z) ** 2) ** 0.5
        same = int(lx // 15) == sx and int(lz // 12) == sz
        if not same and d > 6:
            continue
        key = (not same, d)
        if best is None or key < best[0]:
            best = (key, name)
    return best[1] if best else None


def place_phrase(step, dungeon_of, entered_via):
    """Where the check physically is, in words: entrance shuffles change the way in, never the place."""
    loc, region = step['loc'], step.get('region', '')
    area = AREA.get(region, '')

    def dungeon(dn, detail=''):
        """'in Tail Cave (detail)', and in shuffled dungeons the way in: another dungeon's entrance"""
        text = f'in {DUNGEON[dn]}' + (f' ({detail})' if detail else '')
        key = next((k for k, v in DUNGEON_KEY.items() if v == dn), None)
        via = entered_via.get(key)
        if via and via != key:
            text += f", through {DUNGEON[DUNGEON_KEY[via]].removeprefix('the ')}'s entrance"
        return text
    if loc in PLACE:
        return PLACE[loc]
    m = re.match(r'^pot-([A-Za-z0-9]+)-(\d\d[A-P])-(\d+)$', loc)
    if m:
        lv = m.group(1)
        if lv in LEVEL_DUNGEON:
            return 'in a pot ' + dungeon(dungeon_of.get(loc, LEVEL_DUNGEON[lv]))
        return f'in a pot in {_words(lv)}' + (f' ({area})' if area else '')
    if loc.startswith(('bush-', 'dig-')):
        lm = step.get('landmark')
        if lm and lm.startswith(GENERIC) and area:
            lm += f' in {area}'
        if loc.startswith('bush-'):
            return f'under a bush by {lm}' if lm else f'under a bush in {area or "the wilds"}'
        return f'buried near {lm}' if lm else f'buried in {area or "the ground"}'
    if loc.endswith('-dig'):
        return f'buried in {area or "the ground"}'
    if loc.startswith('shop-'):
        return 'in the Mabe Village shop'
    if loc.startswith('trendy-prize'):
        return 'at the Trendy Game in Mabe Village'
    if loc.startswith('fishing-'):
        return 'at the fishing pond in Mabe Village'
    if loc.startswith('dampe-'):
        return "with Dampe's chambers"
    if loc.startswith('rapids-race'):
        return 'as a Rapids Ride race prize'
    m = re.match(r'^(\d+)-seashell-reward$', loc)
    if m:
        return f'at the Seashell Mansion ({m.group(1)} shells)'
    m = re.match(r'^(D\d)-(.*)$', loc)
    if m:
        dn, rest = dungeon_of.get(loc, m.group(1)), m.group(2)
        if rest == 'instrument':
            return 'at the end of ' + dungeon(dn)[3:]
        boss = {'moldorm', 'genie', 'slime-eye', 'angler', 'slime-eel', 'facade', 'eagle', 'hothead'}
        if rest in boss:
            return 'with the boss ' + dungeon(dn)
        if rest.startswith('pot-'):
            return 'in a pot ' + dungeon(dn)
        return dungeon(dn, rest.replace('-', ' '))
    rest = loc.split('-')
    words = AREA_WORDS.get(region, ())
    rest = [w for w in rest if w not in words] or loc.split('-')
    detail = ' '.join({'loose': 'on the ground'}.get(w, w) for w in rest)
    if loc.endswith('-sunken'):
        return f'under water in {area}' if area else 'under water'
    if 'bonk-tree' in loc:
        return f'in a tree in {area} (dash into it)' if area else 'in a tree (dash into it)'
    if area:
        return f'in {area} ({detail})'
    return f'at {detail}'


def hint_text(step, dungeon_of, entered_via):
    """-> [(text, red)] segments of one hint"""
    it = item_phrase(step['item'])
    it = it[0].upper() + it[1:]
    plural = step['item'] in PLURAL
    return [(it, True), ((' lie ' if plural else ' lies ') + place_phrase(step, dungeon_of, entered_via) + '!', False)]


GET_TEXT = [('You got the ', False), (NAME, True), ('! Use it when you are stuck to learn where your next '
            'treasure lies.', False)]
DONE_TEXT = [('Nothing left to find. Go wake the Wind Fish!', False)]


def wrap(segments, wait):
    """Our segments -> message bytes: lines of at most LINE characters, two per page, red words tagged."""
    words = []  # (word, red, glued: no space before it - punctuation right after a coloured name)
    prev = ''
    for text, red in segments:
        for k, w in enumerate(text.split()):
            words.append((w, red, k == 0 and bool(words) and not text[:1].isspace() and not prev[-1:].isspace()))
        prev = text
    def length(line):
        return sum(len(x) + (0 if i == 0 or g else 1) for i, (x, _, g) in enumerate(line))
    lines, cur = [], []
    for w in words:
        if cur and length(cur + [w]) > LINE and not w[2]:
            lines.append(cur)
            cur = []
        cur.append(w)
    if cur:
        lines.append(cur)
    out = b''
    red = False
    for i, line in enumerate(lines):
        if i:
            out += wait + _PAGE if i % 2 == 0 else b'\n'
        for j, (w, r, glued) in enumerate(line):
            if j and not glued:
                out += b' '
            if r != red:
                out += _RED if r else _WHITE
                red = r
            out += w.encode()
    if red:
        out += _WHITE
    return out + wait + b'\0'


# ------------------------------------------------------------------ seed files
def _read(out_dir, romfs, rel):
    for base in (os.path.join(out_dir, 'romfs'), romfs):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            with open(p, 'rb') as f:
                return f.read()
    return None


def _src(out_dir, romfs, rel):
    p = os.path.join(out_dir, 'romfs', rel)
    if os.path.exists(p):
        return p
    p2 = os.path.join(out_dir, 'romfs', rel)
    os.makedirs(os.path.dirname(p2), exist_ok=True)
    with open(os.path.join(romfs, rel), 'rb') as f, open(p2, 'wb') as g:
        g.write(f.read())
    return p2


def _item_row(out_dir, romfs, oead_tools):
    """Items.gsheet `HintSheet`: a dummy row (a copy of the Yoshi Doll's, as LAS's own dummies and pets.py) whose
    item actor / held-up model is the dungeon map's. Its itemID is one no other row has; only the get sequence uses it."""
    rel = 'region_common/datasheets/Items.gsheet'
    p = _src(out_dir, romfs, rel)
    sheet = oead_tools.readSheet(p)
    rows = [oead_tools.parseStruct(v) for v in sheet['values']]
    if any(r['symbol'] == ENTRY for r in rows):
        return
    used = {int(r['itemID']) for r in rows}
    free = next(i for i in list(range(78, 70, -1)) + list(range(150, 200)) if i not in used)
    base = next(r for r in rows if r['symbol'] == 'YoshiDoll')
    sheet['values'].append(oead_tools.dictToStruct(dict(base, symbol=ENTRY, itemID=free, npcKey=MODEL_NPC,
                                                         gettingFlag='')))
    oead_tools.writeSheet(p, sheet)


def apply(out_dir, romfs, settings):
    """After escape_bottle.apply (Tarin's flow): the hand-over, the hint messages and hints.tsv."""
    from RandomizerCore.Tools import event_tools, oead_tools
    import keysanity_text as kt
    sp = os.path.join(out_dir, 'hint_steps.json')
    if not os.path.exists(sp):
        return 'no steps'
    with open(sp) as f:
        data = json.load(f)
    steps_ = data['steps']
    # dungeon-entrances: entrance -> the dungeon it leads into; the hints need dungeon -> its way in
    entered_via = {inside: door for door, inside in (data.get('dungeon-entrances') or {}).items()}
    dungeon_of = settings.get('dungeon-of', {}) or {}
    ev_dir = os.path.join(out_dir, 'romfs', 'region_common', 'event')
    # Tarin: after the Escape Bottle's AddItem (escape_bottle.py), before the starting items and his own item
    tp = os.path.join(ev_dir, 'Tarin.bfevfl')
    if not os.path.exists(tp):
        return 'no Tarin flow'
    flow = event_tools.readFlow(tp)
    fc = flow.flowchart
    after = 'Event36'
    name = event_tools.findEvent(fc, 'Event36').data.nxt.v.name
    for _ in range(4):  # (the Escape Bottle's two events)
        ev = event_tools.findEvent(fc, name)
        d = ev.data
        try:
            act = d.actor_action.v.v
            params = dict(d.params.data) if d.params else {}
        except AttributeError:
            break
        if act == 'AddItemByKey' and params.get('itemKey') == 'Bottle' and params.get('index') == 2:
            after = name
            break
        name = d.nxt.v.name if d.nxt.v else None
        if not name:
            break
    nxt = event_tools.findEvent(fc, after).data.nxt.v.name
    event_tools.createActionChain(fc, after, [
        ('Link', 'GenericItemGetSequenceByKey', {'itemKey': ENTRY, 'keepCarry': False, 'messageEntry': ENTRY}),
        ('Inventory', 'AddItemByKey', {'itemKey': 'Bottle', 'count': 1, 'index': INDEX, 'autoEquip': False})], nxt)
    event_tools.writeFlow(tp, flow)
    _item_row(out_dir, romfs, oead_tools)
    # Item.bfevfl: the get message
    ip = _src(out_dir, romfs, 'region_common/event/Item.bfevfl')
    flow = event_tools.readFlow(ip)
    if event_tools.findEntryPoint(flow.flowchart, ENTRY) is None:
        event_tools.addEntryPoint(flow.flowchart, ENTRY)
        event_tools.createActionChain(flow.flowchart, ENTRY, [('Dialog', 'Show', {'message': f'System:{ENTRY}'})])
    event_tools.writeFlow(ip, flow)
    # Common.bfevfl: one entry per hint (the runtime requests the one to show), plus the closing one
    cp = _src(out_dir, romfs, 'region_common/event/Common.bfevfl')
    flow = event_tools.readFlow(cp)
    names = [step_entry(i) for i in range(len(steps_))] + [DONE]
    for n in names:
        if event_tools.findEntryPoint(flow.flowchart, n) is None:
            event_tools.addEntryPoint(flow.flowchart, n)
            event_tools.createActionChain(flow.flowchart, n, [('Dialog', 'Show', {'message': f'System:{n}'})])
    event_tools.writeFlow(cp, flow)
    if any(s['loc'].startswith(('bush-', 'dig-')) for s in steps_):  # (their nearest landmark)
        from RandomizerCore.Tools import leb
        import bushes
        marks = landmarks(romfs, leb)
        bush_tile = {bushes.location_name(b): (b['room'], b['tile']) for b in bushes.scan(romfs, leb, pits=True)}
        for st in steps_:
            m = re.match(r'^dig-(\d\d[A-P])-(\d+)$', st['loc'])
            spot = (m.group(1), int(m.group(2))) if m else bush_tile.get(st['loc'])
            if spot and spot[1] is not None:
                st['landmark'] = nearest(marks, *tile_xz(*spot))
    texts = {n: hint_text(s, dungeon_of, entered_via) for n, s in zip(names, steps_)}
    texts[DONE] = DONE_TEXT
    nfiles = 0
    for sysp in sorted(glob.glob(os.path.join(romfs, 'region*', '*', 'message', 'System.msbt'))):
        rel = os.path.relpath(sysp, romfs)
        m = Msbt(_read(out_dir, romfs, rel))
        wait = next((r for k, g, r in reversed(kt._tokens(m.texts.get('GetBottle', b''))) if g == kt.WAIT), b'')
        m.set(ENTRY, wrap(GET_TEXT, wait))
        for n, t in texts.items():
            m.set(n, wrap(t, wait))
        out = os.path.join(out_dir, 'romfs', rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(m.pack())
        nfiles += 1
    # hints.tsv (runtime): S <entry> <location> <item> <copy n> <gated groups it needs, or ->, in order; D <entry>
    with open(os.path.join(out_dir, 'hints.tsv'), 'w') as f:
        for n, s in zip(names, steps_):
            f.write(f"S\t{n}\t{s['loc']}\t{s['item']}\t{s['n']}\t{','.join(s.get('needs', ())) or '-'}\n")
        f.write(f'D\t{DONE}\n')
    with open(os.path.join(out_dir, 'hints.txt'), 'w') as f:  # (readable: what each message says)
        for n, t in texts.items():
            f.write(f"{n}: {''.join(x for x, _ in t)}\n")
    return f'{len(steps_)} hints, {nfiles} message files'
