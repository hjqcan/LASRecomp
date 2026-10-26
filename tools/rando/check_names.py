#!/usr/bin/env python3
"""Player wording for check names (the in-game check tracker, tools/rando/tracker.py).

LAS location names are terse ids (tail-cave-bonk-tree); the randomizer's own checks are numbered per screen
(bush-09E-1). display_name() turns both into what a player reads: "Tree south of Tail Cave (bonk)",
"Bush, Mabe Village (north-west)". Dungeon checks drop their "Dn-" prefix (the list's title names the dungeon).
"""
import re

# LAS overworld locations -> wording (anything missing falls back to the id, spelled out)
NAMES = {
    '5-seashell-reward': 'Seashell Mansion: 5 shells', '15-seashell-reward': 'Seashell Mansion: 15 shells',
    '30-seashell-reward': 'Seashell Mansion: 30 shells', '40-seashell-reward': 'Seashell Mansion: 40 shells',
    '50-seashell-reward': 'Seashell Mansion: 50 shells',
    'above-cave-dig': 'Dig spot above the cave', 'above-d3': 'Dig spot above Key Cavern',
    'above-turtle-rock': 'Ledge above Turtle Rock', 'animal-village-cave': 'Cave by Animal Village',
    'animal-village-northwest': 'Northwest of Animal Village', 'armos-cave': 'Armos Maze cave',
    'armos-knight': 'Armos Knight (Southern Face Shrine)', 'bay-fisherman': "Fisherman under the bridge",
    'bay-passage-sunken': 'Sunken item in the bay passage', 'beach-bonk-tree': 'Tree on the beach (bonk)',
    'beach-chest': 'Chest on the beach', 'beach-dig': 'Dig spot on the beach',
    'beside-seashell-mansion': 'Beside the Seashell Mansion', 'chef-bear': 'Chef Bear (Animal Village)',
    'christine-grateful': 'Christine, thank-you gift', 'christine-trade': 'Christine, trade',
    'ciao-ciao': 'Ciao Ciao (BowWow)', 'dampe-bottle-challenge': "Dampe's bottle challenge",
    'dampe-final': "Dampe's final challenge", 'dampe-heart-challenge': "Dampe's heart challenge",
    'dampe-page-1': "Dampe, first page", 'dampe-page-2': "Dampe, second page",
    'desert-cave': 'Cave in Yarna Desert', 'desert-dig': 'Dig spot in Yarna Desert',
    'desert-south': 'South of Yarna Desert', 'diamond-island': 'Diamond island',
    'doghouse-dig': "BowWow's doghouse (dig)", 'dream-shrine-left': 'Dream Shrine, left',
    'dream-shrine-right': 'Dream Shrine, right', 'fishing-100': 'Fishing pond: 100 points',
    'fishing-150': 'Fishing pond: 150 points', 'fishing-50': 'Fishing pond: 50 points',
    'fishing-cheep-cheep': 'Fishing pond: Cheep Cheep', 'fishing-loose': 'Fishing pond: loose item',
    'fishing-ol-baron': "Fishing pond: Ol' Baron", 'fishing-orange': 'Fishing pond: orange fish',
    'ghost-grave-dig': "Ghost's grave (dig)", 'ghost-house-pot': "Pot in the Ghost's house",
    'ghost-reward': "Ghost's reward", 'goponga-cave-left': 'Goponga Swamp cave, left',
    'goponga-cave-right': 'Goponga Swamp cave, right', 'goponga-west-dig': 'West of Goponga Swamp (dig)',
    'goriya-trader': 'Goriya (trade)', 'grandma-yahoo': 'Grandma Yahoo (broom)',
    'graveyard-cave': 'Cave under the Cemetery', 'invisible-zora': 'Invisible Zora',
    'kanalet-bombed-guard': 'Kanalet Castle: bombed guard', 'kanalet-crow': 'Kanalet Castle: crow',
    'kanalet-final-guard': 'Kanalet Castle: last guard', 'kanalet-kill-room': 'Kanalet Castle: fight room',
    'kanalet-mad-bomber': 'Kanalet Castle: Mad Bomber', 'kanalet-moat-north': 'Kanalet moat, north (sunken)',
    'kanalet-moat-south': 'Kanalet moat, south (sunken)', 'kanalet-south-cave': 'Cave south of Kanalet Castle',
    'kiki': 'Kiki the monkey', 'lanmola': 'Lanmola (Yarna Desert)', 'mabe-bushes': 'Bushes in Mabe Village',
    'mabe-well': 'Mabe Village well', 'mad-batter-bay': "Mad Batter, Martha's Bay",
    'mad-batter-taltal': 'Mad Batter, Tal Tal', 'mad-batter-woods': 'Mad Batter, Mysterious Forest',
    'mamasha': 'Mamasha (trade)', 'mamu': 'Mamu (song)', 'manbo': 'Manbo (song)', 'marin': 'Marin (song)',
    'mermaid-cave': "Mermaid statue cave", 'mermaid-martha': 'Martha the mermaid',
    'mr-write': 'Mr. Write (trade)', 'north-of-moblin-cave': 'North of the Moblin cave',
    'north-wasteland': 'North of Tabahl Wasteland', 'papahl': 'Papahl (trade)',
    'plains-rock-maze': 'Rock maze in Ukuku Prairie', 'pond-island': 'Island in the pond',
    'pothole-final': 'Pothole Field: last hole', 'pothole-north': 'Pothole Field: north',
    'rapids-ascent-cave': 'Rapids Ride: cave on the climb', 'rapids-east-island': 'Rapids Ride: east island',
    'rapids-middle-island': 'Rapids Ride: middle island', 'rapids-race-30': 'Rapids Ride: race under 30 s',
    'rapids-race-35': 'Rapids Ride: race under 35 s', 'rapids-race-45': 'Rapids Ride: race under 45 s',
    'rapids-south-island': 'Rapids Ride: south island', 'rapids-west-island': 'Rapids Ride: west island',
    'river-crossing-cave': 'Cave by the river crossing', 'rock-maze': 'Rock maze', 'sale': 'Sale (trade)',
    'shop-slot1': 'Town Tool Shop: shelf 1', 'shop-slot2': 'Town Tool Shop: shelf 2',
    'shop-slot3-1st': 'Town Tool Shop: shelf 3', 'shop-slot4': 'Town Tool Shop: shelf 4',
    'shop-slot5-1st': 'Town Tool Shop: shelf 5', 'shop-slot6': 'Town Tool Shop: shelf 6',
    'small-coast-island': 'Small island off the coast', 'south-bay-dig': "South of Martha's Bay (dig)",
    'south-bay-sunken': "South of Martha's Bay (sunken)", 'southwest-bay-bush': "Bush southwest of Martha's Bay",
    'swamp-chest': 'Chest in Goponga Swamp', 'syrup': 'Syrup the witch',
    'tail-cave-bonk-tree': 'Tree south of Tail Cave (bonk)', 'taltal-5-chest-puzzle': 'Tal Tal: five chest game',
    'taltal-bomb-hole': 'Tal Tal: bomb hole', 'taltal-crossing-cave': 'Tal Tal: crossing cave',
    'taltal-east-bridge': 'Tal Tal: east bridge', 'taltal-east-drop': 'Tal Tal: east drop (sunken)',
    'taltal-east-left-chest': 'Tal Tal: east, left chest', 'taltal-entrance-blocks': 'Tal Tal: blocks at the entrance',
    'taltal-entrance-chest': 'Tal Tal: chest at the entrance', 'taltal-heights-hole': 'Tal Tal Heights: hole',
    'taltal-ledge-chest': 'Tal Tal: chest on the ledge', 'taltal-left-ascent-cave': 'Tal Tal: cave on the left climb',
    'taltal-rooster-cave': 'Tal Tal: cave by the rooster', 'taltal-sunken': 'Tal Tal: sunken item',
    'taltal-west-chest': 'Tal Tal: west chest', 'taltal-west-dig': 'Tal Tal: west (dig)',
    'taltal-west-escape': 'Tal Tal: west, way out', 'taltal-west-rock': 'Tal Tal: under a rock (west)',
    'tarin': 'Tarin', 'tarin-ukuku': 'Tarin at the beehive', 'trendy-prize-final': 'Trendy Game: last prize',
    'ukuku-bonk-tree': 'Tree in Ukuku Prairie (bonk)', 'ukuku-cave-east-chest': 'Ukuku cave, east chest',
    'ukuku-cave-east-loose': 'Ukuku cave, east item', 'ukuku-cave-west-chest': 'Ukuku cave, west chest',
    'ukuku-cave-west-loose': 'Ukuku cave, west item', 'under-skull-rock': 'Under the skull rock (dig)',
    'villa-cave': 'Cave by the villa', 'walrus': 'Walrus', 'washed-up': 'Washed up on the beach',
    'wasteland-dig': 'Dig spot in Tabahl Wasteland', 'woods-crossing-cave-chest': 'Forest crossing cave, chest',
    'woods-crossing-cave-loose': 'Forest crossing cave, item', 'woods-east-dig': 'Mysterious Forest, east (dig)',
    'woods-loose': 'Mysterious Forest, loose item', 'woods-north-cave-chest': 'Forest north cave, chest',
    'woods-north-cave-loose': 'Forest north cave, item', 'woods-north-chest': 'Mysterious Forest, north chest',
    'woods-south-chest': 'Mysterious Forest, south chest', 'woods-west-dig': 'Mysterious Forest, west (dig)',
}
KIND_WORD = {'bush': 'Bush', 'dig': 'Dig spot', 'pot': 'Pot', 'apple': 'Apple tree'}
# interiors holding pots (potsanity), by level
HOUSES = {'MarinTarinHouse': "Marin and Tarin's house", 'DrWrightHouse': "Mr. Write's house",
          'MadamHouse': "Madam MeowMeow's house", 'QuadupletHouse': "the quadruplets' house",
          'TracyHouse': "Crazy Tracy's house", 'UlriraHouse': "Ulrira's house", 'ArmosShrine': 'the Southern Face Shrine',
          'MagicPowderHouse': "the witch's hut", 'RichardVilla': "Richard's villa", 'LibraryHouse': 'the library',
          'GhostHouse': "the Ghost's house", 'ShellHouse': 'the Seashell Mansion', 'Telephone': 'a telephone booth',
          'ToolShop': 'the Town Tool Shop', 'AnimalHouse': 'Animal Village', 'DreamShrine': 'the Dream Shrine'}


def house(level):
    for k, v in HOUSES.items():
        if level.startswith(k):
            return v
    return re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', level)


def spelled(name):
    s = name.replace('-', ' ')
    return s[:1].upper() + s[1:]


def direction(fx, fz):
    """A position inside a box (0..1 each way) as a compass word: north-west ... centre ... south-east."""
    v = 'north' if fz < 1 / 3 else 'south' if fz > 2 / 3 else ''
    h = 'west' if fx < 1 / 3 else 'east' if fx > 2 / 3 else ''
    return f'{v}-{h}' if v and h else v or h or 'centre'


def display_name(name, kind=None, area=None, where=None):
    """name: the LAS location; kind: bush / dig / pot / apple for the randomizer's numbered checks; area: the place name it
    is in; where: a compass word inside that area (or a room / house name)."""
    if name in NAMES:
        return NAMES[name]
    m = re.match(r'trendy-prize-(\d+)$', name)
    if m:
        return f'Trendy Game: prize {m.group(1)}'
    m = re.match(r'(D\d)-(.*)$', name)
    if m and not m.group(2).startswith('pot-'):
        return spelled(m.group(2))
    if kind in KIND_WORD:
        parts = [KIND_WORD[kind]]
        if area:
            parts.append(area)
        s = ', '.join(parts)
        return f'{s} ({where})' if where else s
    return spelled(name)
