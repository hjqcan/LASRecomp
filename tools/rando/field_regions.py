#!/usr/bin/env python3
"""The logic region of every overworld (Field) tile that holds a randomized check of its own: the bushes
(bushsanity, bushes.py) and dig tiles (digsanity, digs.py). Reviewed by hand, screen by screen (2026-10-20).

Before this table a check took the logic regions of its screen's LAS locations / house doors, or - with none on the
screen - those of the nearest screens, and its region was the alphabetically first of them: dig-03I-42 (the Tal Tal
Heights lane north of Kanalet Castle) got 'river' and, through river's first LAS location (Mermaid Martha), the hint
"Martha's Bay". Every Field screen is listed here instead, from the game's own data and LAS's region graph:

- the walkable areas of the joined Field grid (field_reach.py: collision, pits, deep water, rocks and other solid
  actors, floor heights) and what each touches: the house / cave doors (entrance_data.yml outer_region, outer_extra =
  a pocket's extra need), the LAS locations placed on the Field (chests, heart pieces, owl statues, Secret Seashell
  spots by their seashell index);
- the actors that gate LAS's regions: the rocks (bracelet: Tal Tal Heights above the Moblin cave, Tracy's, the
  swamp / wasteland edge), the bushes open-mabe removes (Mabe's exits), the sea urchins (the beach), Kiki's bridge,
  Kanalet's gate, the sleeping Walrus (the desert), Tarin the raccoon (the woods' north end);
- the game's place names (Field.lvb zones), which also name each check's hint / spoiler area (ZONE_AREA).

An entry is None (no check there: a check showing up on that screen fails), a region, (region, condition) - the
condition is what reaching a pocket of that region needs on top (a ledge only the D8 side exits reach, the Angler's
Tunnel pond, the Pothole Field...) - or (default, [(tiles, region or (region, condition)), ...]) for a screen two
regions share (a cliff, pits or rocks split it): the tiles (0-79, row-major on the 10 x 8 grid) take the given one.
Where the grid can't tell which side a part is on, it takes the side that needs more (noted in the PR / commit).

tools/test_field_regions.py checks the table (every screen listed, LAS region / condition names) and, given the
game files, that every bush and dig check resolves.
"""
import os
import struct

import fixedhash

# Field.lvb zone (datasheet LocationName place) -> hint / spoiler area (hint_sheet.AREA)
ZONE_AREA = {}
for _zones, _area in (((2,), 'taltal-mountains-west'), ((10, 11, 40, 41, 42), 'taltal-heights'),
                      ((20,), 'taltal-mountains-east'), ((30, 31, 32, 33), 'goponga-swamp'), ((50,), 'mysterious-woods'),
                      ((60, 61, 80), 'koholint-prairie'), ((70,), 'tabahl-wasteland'),
                      ((90, 91, 92, 93, 94), 'kanalet-castle'), ((100,), 'rapids-ride'),
                      ((110, 111, 112, 113, 140, 141, 142), 'mabe-village'), ((120, 121), 'ukuku-prairie'),
                      ((130, 131, 132, 133, 134, 135), 'ancient-ruins'), ((150,), 'sign-maze'),
                      ((160, 161), 'pothole-field'), ((170, 171), 'animal-village'), ((180,), 'toronbo-shores'),
                      ((190, 191, 192, 193, 194, 200), 'marthas-bay'), ((210, 211), 'yarna-desert')):
    for _z in _zones:
        ZONE_AREA[_z] = _area
# a region's area where a tile has no zone of its own (none on a check tile with the game files at hand)
REGION_AREA = {
    'taltal-west-0': 'taltal-mountains-west', 'taltal-west-1': 'taltal-mountains-west',
    'taltal-west-2': 'taltal-mountains-west', 'taltal-east-0': 'taltal-mountains-east',
    'taltal-east-1': 'taltal-mountains-east', 'taltal-east-2': 'taltal-mountains-east', 'taltal-river': 'taltal-heights',
    'heights': 'taltal-heights', 'wasteland': 'tabahl-wasteland', 'mr-write-house': 'goponga-swamp',
    'swamp': 'goponga-swamp', 'woods': 'mysterious-woods', 'tracy': 'koholint-prairie', 'ghost-grave': 'koholint-prairie',
    'kanalet-outside': 'kanalet-castle', 'river': 'kanalet-castle', 'bridge': 'kanalet-castle', 'rapids': 'rapids-ride',
    'mabe': 'mabe-village', 'sale-house': 'toronbo-shores', 'beach': 'toronbo-shores', 'plains': 'ukuku-prairie',
    'sign-maze': 'sign-maze', 'pothole': 'pothole-field', 'ghost-house': 'marthas-bay',
    'animal-village': 'animal-village', 'armos-maze': 'ancient-ruins', 'desert': 'yarna-desert'}

SCREENS = {
    'Field_01A': None,
    'Field_01B': ('taltal-west-2', 'D8-4C & bombs'),
    'Field_01C': ('taltal-west-2', 'D8-4C & bombs'),
    'Field_01D': None,
    'Field_01E': None,
    'Field_01F': None,
    'Field_01G': None,
    'Field_01H': ('taltal-east-1', 'hookshot | rooster'),
    'Field_01I': None,
    'Field_01J': None,
    'Field_01K': 'taltal-east-1',
    'Field_01L': 'taltal-east-1',
    'Field_01M': None,
    'Field_01N': None,
    'Field_01O': None,
    'Field_01P': None,
    'Field_02A': 'taltal-west-2',
    'Field_02B': ('taltal-west-2', [
        ((0, 1, 2, 3, 4, 6, 8, 9, 14, 15, 16, 17, 18, 19), ('taltal-west-2', 'D8-4C & bombs')),
    ]),
    'Field_02C': ('taltal-west-2', [
        ((0, 1, 2, 3, 4, 10, 11, 12, 13, 14), ('taltal-west-2', 'D8-4C & bombs')),
    ]),
    'Field_02D': ('taltal-west-1', [
        ((4, 10, 11, 12, 13, 23), 'taltal-west-2'),
        ((17, 18, 19, 27), 'taltal-west-0'),
    ]),
    'Field_02E': 'taltal-west-0',
    'Field_02F': None,
    'Field_02G': ('wasteland', 'bracelet'),
    'Field_02H': (('wasteland', 'bracelet'), [
        ((4, 5, 6, 7, 8, 14, 17, 23, 24, 33, 34), ('taltal-east-1', 'hookshot | rooster')),
        # (the raised ledge, no walkable link from taltal-east-0 ground: entered from the crossing side)
        ((26, 27, 28, 29, 36, 38, 39, 48, 49, 58, 59), ('taltal-east-1', 'hookshot | rooster')),
    ]),
    'Field_02I': ('taltal-east-0', [
        ((26, 35, 36, 44, 45, 46, 54, 55, 56), ('wasteland', 'bracelet & break-crystal')),
        ((20, 21, 30, 31, 40, 41, 50, 51), ('taltal-east-1', 'hookshot | rooster')),  # (02H's raised ledge)
    ]),
    'Field_02J': ('taltal-east-0', [
        ((5, 6, 10, 11, 12, 13, 14, 15, 16, 21, 24, 25, 26, 34, 35, 36, 37, 38, 39, 44, 45, 46, 54), 'taltal-east-1'),
    ]),
    'Field_02K': ('taltal-east-1', [
        ((11, 20, 21, 30, 31, 40, 41), 'taltal-east-0'),
    ]),
    'Field_02L': None,
    'Field_02M': None,
    'Field_02N': ('taltal-east-2', [
        ((1, 11, 12, 13, 21, 22, 23, 31, 32, 33, 43, 53, 63, 64, 73, 74, 75), ('taltal-east-2', 'bombs')),
    ]),
    'Field_02O': ('taltal-east-2', [
        ((9, 12, 13, 14, 15, 16, 17, 18, 19, 22, 23, 24, 25, 26, 27, 28, 29, 35, 39, 45), ('taltal-east-2', '(feather & hookshot) | rooster')),
    ]),
    'Field_02P': (('taltal-east-2', '(feather & hookshot) | rooster'), [
        ((42, 50, 51, 61, 70, 71), 'taltal-east-2'),
    ]),
    'Field_03A': 'mr-write-house',
    'Field_03B': ('mr-write-house', [
        ((4,), 'taltal-west-2'),
        ((7, 8, 9, 17), 'taltal-west-1'),
    ]),
    'Field_03C': ('swamp', [
        ((0, 1, 2, 3, 4, 5, 6, 7, 8, 9), 'taltal-west-1'),
    ]),
    'Field_03D': ('swamp', [
        ((0, 1, 2, 3, 5, 6, 8), 'taltal-west-1'),
    ]),
    'Field_03E': ('swamp', [
        ((17, 18, 19, 28), ('wasteland', 'bracelet')),
        ((49, 58, 59, 68, 69, 79), 'wasteland'),
    ]),
    'Field_03F': (('wasteland', 'bracelet'), [
        ((40, 41, 42, 43, 44, 45, 50, 51, 52, 53, 54, 55, 60, 63, 64, 65, 66, 67, 68, 69, 70, 71, 73, 74, 75, 76, 77, 78, 79), 'wasteland'),
    ]),
    'Field_03G': (('wasteland', 'bracelet'), [
        ((43, 44, 45, 46, 47, 48, 49, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 68, 69, 70, 71, 72, 73, 74, 75, 77, 78, 79), 'wasteland'),
    ]),
    'Field_03H': (('wasteland', 'bracelet'), [
        ((40, 41, 42, 43, 44, 45, 50, 52, 53, 54, 55, 60, 61, 62, 63, 64, 65, 70, 71, 72, 73, 74, 75), 'wasteland'),
    ]),
    'Field_03I': (('wasteland', 'bracelet'), [
        ((9, 19), 'taltal-east-0'),
    ]),
    'Field_03J': ('heights', [
        ((0, 10), 'taltal-east-0'),
    ]),
    'Field_03K': ('heights', [
        ((4, 5, 6, 7, 8, 9, 14, 18, 19), 'taltal-east-0'),
    ]),
    'Field_03L': ('heights', [
        ((0, 10), 'taltal-east-0'),
        ((4, 12, 13, 14, 15, 22, 23, 24, 25, 26, 32, 33, 34, 35, 36), ('heights', '(angler-key & taltal-east-0) | (taltal-river & cross-water)')),
    ]),
    'Field_03M': ('heights', [
        ((9,), 'taltal-east-2'),
    ]),
    'Field_03N': ('heights', [
        ((0, 1, 7, 8, 9), 'taltal-east-2'),
    ]),
    'Field_03O': ('heights', [
        ((0, 1, 2, 3), 'taltal-east-2'),
        ((15,), 'taltal-river'),
        ((47, 48, 49, 56, 57, 58, 59, 66, 67, 77, 78), 'rapids'),
    ]),
    'Field_03P': ('rapids', [
        ((3, 4, 5, 6, 7), ('taltal-east-2', '(feather & hookshot) | rooster')),
    ]),
    'Field_04A': ('mr-write-house', [
        ((57, 58, 59, 64, 65, 66, 67, 75, 76), 'swamp'),
    ]),
    'Field_04B': ('swamp', [
        ((1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18, 21, 22, 25, 26), 'mr-write-house'),
    ]),
    'Field_04C': 'swamp',
    'Field_04D': None,
    'Field_04E': ('swamp', [
        ((39, 47, 48, 49, 55, 56, 57, 58, 64, 65, 74, 75), 'wasteland'),
    ]),
    'Field_04F': 'wasteland',
    'Field_04G': 'wasteland',
    'Field_04H': ('wasteland', [
        ((7, 8, 17, 18, 27, 28, 38, 48, 49, 58, 59), ('wasteland', 'bracelet')),
    ]),
    'Field_04I': ('heights', [
        ((1, 2, 3, 9, 11, 12, 13, 21, 22, 23, 31, 32, 40, 41, 42, 50, 51, 52), ('wasteland', 'bracelet')),
    ]),
    'Field_04J': ('heights', [
        ((0, 1, 11), ('wasteland', 'bracelet')),  # (the lane's last pocket past 03I's rock, above the Heights)
    ]),
    'Field_04K': 'heights',
    'Field_04L': ('heights', [
        ((51, 61, 62, 71, 72), 'bridge'),
    ]),
    'Field_04M': 'heights',
    'Field_04N': 'heights',
    'Field_04O': ('heights', [
        ((7, 8, 16, 17, 18, 26, 27, 28, 36, 37, 38, 46, 47, 48), 'rapids'),
    ]),
    'Field_04P': 'rapids',
    'Field_05A': ('woods', [
        ((5, 6, 14, 15, 16, 17, 24, 25, 26, 27, 28, 29, 36), 'swamp'),
    ]),
    'Field_05B': ('woods', [
        ((20, 21, 22, 23, 24, 25, 30, 31, 32, 33, 34, 35, 36, 40, 41, 42, 43, 44, 45, 46, 51, 52, 53, 54, 55, 56, 61, 62, 63, 64, 65, 66, 70, 71, 72, 73, 74, 75, 76), ('woods', 'powders | rooster | feather')),
    ]),
    'Field_05C': 'woods',
    'Field_05D': 'woods',
    'Field_05E': ('woods', [
        ((3, 5, 13, 14, 15), 'wasteland'),
        ((34, 43, 44, 45, 54), ('woods', 'feather | rooster | hookshot')),
    ]),
    'Field_05F': ('tracy', [
        ((20, 21, 30, 31, 32, 40, 41), 'woods'),
    ]),
    'Field_05G': ('wasteland', [
        ((40, 50, 51), 'tracy'),
        ((62, 72), ('wasteland', 'bracelet')),
    ]),
    'Field_05H': ('wasteland', [
        ((12, 13, 14, 15), ('wasteland', 'feather | hookshot | rooster')),
    ]),
    'Field_05I': ('kanalet-outside', [
        ((53, 54), ('river', 'flippers')),
        ((1,), 'wasteland'),
        ((16, 17, 18, 19), 'river'),  # (the strip between the moat and the outer wall: only from the moat)
    ]),
    'Field_05J': ('kanalet-outside', [
        ((10, 11, 12, 13, 14, 15, 16, 17, 18, 19), 'river'),
    ]),
    'Field_05K': ('kanalet-outside', [
        ((27, 28, 29, 37, 38, 39, 47, 48, 49), 'bridge'),
        ((10, 11, 12, 13, 14, 15, 16), 'river'),
    ]),
    'Field_05L': 'bridge',
    'Field_05M': None,
    'Field_05N': None,
    'Field_05O': None,
    'Field_05P': None,
    'Field_06A': 'woods',
    'Field_06B': ('woods', [
        ((0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39), ('woods', 'powders | rooster | feather')),
    ]),
    'Field_06C': 'woods',
    'Field_06D': 'woods',
    'Field_06E': 'woods',
    'Field_06F': ('wasteland', [
        ((1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 23, 25, 28), 'tracy'),
    ]),
    'Field_06G': ('wasteland', [
        ((1, 2, 3, 4, 11, 12, 13, 14, 21, 24), ('wasteland', 'bracelet')),
    ]),
    'Field_06H': 'wasteland',
    'Field_06I': 'kanalet-outside',
    'Field_06J': None,
    'Field_06K': 'kanalet-outside',
    'Field_06L': 'bridge',
    'Field_06M': None,
    'Field_06N': None,
    'Field_06O': 'rapids',
    'Field_06P': None,
    'Field_07A': 'woods',
    'Field_07B': 'woods',
    'Field_07C': 'woods',
    'Field_07D': 'woods',
    'Field_07E': ('woods', [
        ((62, 71, 72, 73, 74, 77), 'ghost-grave'),
    ]),
    'Field_07F': ('woods', [
        ((7, 8, 16, 17, 18, 26, 27, 37, 57, 58, 59, 67, 68, 69, 77, 78), 'wasteland'),
    ]),
    'Field_07G': 'wasteland',
    'Field_07H': 'wasteland',
    'Field_07I': 'kanalet-outside',
    'Field_07J': 'kanalet-outside',
    'Field_07K': ('kanalet-outside', [
        ((28, 29, 38, 39, 48), 'bridge'),
    ]),
    'Field_07L': 'bridge',
    'Field_07M': None,
    'Field_07N': None,
    'Field_07O': None,
    'Field_07P': None,
    'Field_08A': 'woods',
    'Field_08B': ('woods', [
        ((42, 43, 44, 45, 52, 53, 55, 63, 64, 65, 66), ('woods', 'bracelet')),
    ]),
    'Field_08C': 'woods',
    'Field_08D': 'woods',
    'Field_08E': 'ghost-grave',
    'Field_08F': ('ghost-grave', [
        ((7, 8, 17, 18, 27, 28, 37, 38, 46, 47, 48, 56, 57, 66, 67, 76, 77), 'wasteland'),
    ]),
    'Field_08G': 'wasteland',
    'Field_08H': 'wasteland',
    'Field_08I': 'plains',
    'Field_08J': ('plains', [
        ((4, 5, 6, 14, 16), 'kanalet-outside'),
    ]),
    'Field_08K': 'plains',
    'Field_08L': ('plains', [
        ((4,), 'bridge'),
    ]),
    'Field_08M': None,
    'Field_08N': None,
    'Field_08O': None,
    'Field_08P': None,
    'Field_09A': 'woods',
    'Field_09B': None,
    'Field_09C': 'mabe',
    'Field_09D': ('mabe', [
        ((32, 42), ('mabe', 'bracelet')),
    ]),
    'Field_09E': ('mabe', [
        ((57, 59, 67, 68, 69, 77, 78, 79), 'plains'),
    ]),
    'Field_09F': ('plains', [
        ((6, 7, 16, 17, 25, 26, 27, 35, 36, 46), 'wasteland'),
    ]),
    'Field_09G': 'plains',
    'Field_09H': 'plains',
    'Field_09I': 'plains',
    'Field_09J': 'plains',
    'Field_09K': 'plains',
    'Field_09L': 'plains',
    'Field_09M': ('river', 'hookshot | rooster'),
    'Field_09N': 'armos-maze',
    'Field_09O': ('armos-maze', [
        ((15, 16, 17, 18, 19, 27), 'rapids'),
    ]),
    'Field_09P': ('armos-maze', [
        ((0, 10), 'rapids'),
    ]),
    'Field_10A': 'mabe',
    'Field_10B': 'mabe',
    'Field_10C': 'mabe',
    'Field_10D': 'mabe',
    'Field_10E': ('mabe', [
        ((7, 8, 9, 16, 17, 18, 19, 26, 27, 28, 29, 37, 38, 39, 47, 48, 49, 56, 57, 58, 59, 65, 66, 67, 68, 69, 75, 76, 77, 78, 79), 'plains'),
    ]),
    'Field_10F': 'plains',
    'Field_10G': 'plains',
    'Field_10H': 'plains',
    'Field_10I': 'plains',
    'Field_10J': 'plains',
    'Field_10K': 'plains',
    'Field_10L': ('plains', [
        ((56, 57, 58, 59, 66, 67, 68, 76, 77, 78), 'animal-village'),
    ]),
    'Field_10M': ('animal-village', [
        ((3, 4, 5, 6, 7, 14), ('river', 'hookshot | rooster')),
    ]),
    'Field_10N': ('animal-village', [
        ((9, 29, 39), 'armos-maze'),
        ((12, 13, 14, 24), 'river'),
    ]),
    'Field_10O': ('armos-maze', [
        ((50, 51, 52), 'animal-village'),
    ]),
    'Field_10P': 'armos-maze',
    'Field_11A': 'mabe',
    'Field_11B': 'mabe',
    'Field_11C': 'mabe',
    'Field_11D': 'mabe',
    'Field_11E': 'plains',
    'Field_11F': ('plains', [
        ((29, 34, 35, 36, 37, 38, 39, 44, 45, 46, 47, 48, 49, 53, 54, 55, 56, 57, 58, 59, 64, 65, 66, 67, 68, 69, 74, 75, 76, 77, 78, 79), ('plains', 'feather | cross-water')),
    ]),
    'Field_11G': ('plains', [
        ((20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62, 70, 71, 72), ('plains', 'feather | cross-water')),
        ((14, 15, 16, 17, 18, 35, 36, 37, 45, 46, 47), ('plains', 'cross-water')),
    ]),
    'Field_11H': 'plains',
    'Field_11I': ('plains', [
        ((43, 45, 46, 53, 54, 55, 56, 57, 62, 63, 64, 65, 66, 67, 72, 77), ('plains', 'bombs & (feather | rooster)')),
    ]),
    'Field_11J': 'plains',
    'Field_11K': 'plains',
    'Field_11L': ('animal-village', [
        ((10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 61, 62), 'plains'),
    ]),
    'Field_11M': 'armos-maze',
    'Field_11N': 'armos-maze',
    'Field_11O': 'armos-maze',
    'Field_11P': 'armos-maze',
    'Field_12A': 'mabe',
    'Field_12B': 'mabe',
    'Field_12C': 'mabe',
    'Field_12D': 'mabe',
    'Field_12E': ('plains', [
        ((38, 39, 48, 57, 58, 59, 67, 68, 69, 74, 75, 76, 77), 'sign-maze'),
    ]),
    'Field_12F': ('plains', [
        ((19, 24, 28, 29, 34, 35, 36, 37, 38, 39, 44, 45, 46, 47, 48, 49, 59, 69, 79), ('plains', 'feather | cross-water')),
    ]),
    'Field_12G': (('plains', 'cross-water'), [
        ((1, 10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33, 40, 41, 50, 51, 60, 61, 70, 71), ('plains', 'feather | cross-water')),
    ]),
    'Field_12H': ('plains', [
        ((64, 65), 'pothole'),
    ]),
    'Field_12I': ('plains', [
        ((2, 7, 12, 17, 22, 27, 32, 35, 37, 42, 43, 44, 45, 53, 54, 63, 64, 73, 74), ('plains', 'bombs & (feather | rooster)')),
    ]),
    'Field_12J': 'plains',
    'Field_12K': ('plains', [
        ((56, 57, 58, 59, 66, 68, 69, 78), 'animal-village'),
    ]),
    'Field_12L': ('animal-village', [
        ((10, 11), 'plains'),
    ]),
    'Field_12M': 'armos-maze',
    'Field_12N': 'armos-maze',
    'Field_12O': 'armos-maze',
    'Field_12P': 'armos-maze',
    'Field_13A': 'mabe',
    'Field_13B': 'mabe',
    'Field_13C': 'mabe',
    'Field_13D': 'mabe',
    'Field_13E': 'sign-maze',
    'Field_13F': ('sign-maze', [
        ((6, 16, 17, 18, 19, 26, 27, 28, 29, 37, 38, 39), 'plains'),
    ]),
    'Field_13G': ('pothole', [
        ((10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 41, 51, 61, 71), 'plains'),
    ]),
    'Field_13H': ('pothole', [
        ((8, 18, 28, 38, 48, 58, 68, 78), 'plains'),
    ]),
    'Field_13I': 'plains',
    'Field_13J': 'plains',
    'Field_13K': 'animal-village',
    'Field_13L': 'animal-village',
    'Field_13M': 'animal-village',
    'Field_13N': 'animal-village',
    'Field_13O': None,
    'Field_13P': 'desert',
    'Field_14A': 'mabe',
    'Field_14B': 'mabe',
    'Field_14C': 'mabe',
    'Field_14D': 'mabe',
    'Field_14E': 'sign-maze',
    'Field_14F': 'sign-maze',
    'Field_14G': ('plains', [
        ((6, 14, 15, 16, 17, 18, 19, 26, 36, 37, 38, 39), 'pothole'),
    ]),
    'Field_14H': ('plains', [
        ((1, 4, 6, 10, 11, 12, 14, 16, 22, 23, 24, 26, 30, 31, 35, 36, 41, 42, 43, 44, 45), 'pothole'),
    ]),
    'Field_14I': 'plains',
    'Field_14J': None,
    'Field_14K': 'animal-village',
    'Field_14L': 'animal-village',
    'Field_14M': 'animal-village',
    'Field_14N': 'animal-village',
    'Field_14O': 'desert',
    'Field_14P': 'desert',
    'Field_15A': 'mabe',
    'Field_15B': ('mabe', [
        ((46, 47, 54, 55, 56, 57, 58, 59, 64, 65, 66, 67, 68, 69, 74, 75, 76, 77), 'beach'),
    ]),
    'Field_15C': ('mabe', [
        ((50, 51, 52, 53, 54, 55, 56, 60, 61, 62, 63, 64, 65, 66, 72, 73, 74, 75, 76), 'beach'),
    ]),
    'Field_15D': ('sale-house', [
        ((30, 31, 32, 33, 40, 41, 42, 43, 50, 51, 52, 53), 'mabe'),
    ]),
    'Field_15E': ('beach', [
        ((30, 40, 50), 'sale-house'),
    ]),
    'Field_15F': ('beach', [
        ((39, 49), 'ghost-house'),
    ]),
    'Field_15G': ('ghost-house', [
        ((2, 3, 4, 5), 'plains'),
        ((17, 18, 19, 26, 27, 28, 29, 38), ('plains', '(((rooster | feather) & (boomerang | rod)) | (feather & sword)) & cross-water')),
    ]),
    'Field_15H': ('plains', [
        ((10, 11, 12, 13, 14, 15, 16, 20, 21, 22, 23, 24, 25), ('plains', '(((rooster | feather) & (boomerang | rod)) | (feather & sword)) & cross-water')),
    ]),
    'Field_15I': 'plains',
    'Field_15J': ('plains', [
        ((27, 28, 29, 36, 37, 38, 39, 46, 47, 48, 49, 58, 59), ('animal-village', 'hookshot')),
    ]),
    'Field_15K': ('animal-village', [
        ((20, 21, 30, 31, 40, 41, 42, 50, 51, 61, 71, 72), ('animal-village', 'hookshot')),
        ((4, 5, 6, 15), ('river', 'flippers')),
    ]),
    'Field_15L': 'animal-village',
    'Field_15M': 'animal-village',
    'Field_15N': ('animal-village', [
        ((8, 9, 18, 19, 28, 29, 37, 38, 39, 46, 47, 48, 49, 57, 58, 59, 67, 68, 69, 77, 78, 79), 'desert'),
    ]),
    'Field_15O': 'desert',
    'Field_15P': 'desert',
    'Field_16A': ('beach', [
        ((1, 2, 11, 12, 21, 22, 31, 32, 41, 42, 51, 52), 'mabe'),
    ]),
    'Field_16B': 'beach',
    'Field_16C': 'beach',
    'Field_16D': 'beach',
    'Field_16E': 'beach',
    'Field_16F': 'beach',
    'Field_16G': 'ghost-house',
    'Field_16H': ('plains', [
        ((20, 30, 31, 32, 40, 50, 60), 'ghost-house'),
    ]),
    'Field_16I': ('plains', [
        ((33, 34, 43), ('plains', 'cross-water')),
    ]),
    'Field_16J': ('plains', [
        ((25, 26, 27, 28, 35, 36, 37, 38, 44, 45, 46, 47, 48, 54, 55, 56, 57, 58, 65, 66, 67), ('plains', '((rooster | feather) & (boomerang | rod)) | (feather & sword)')),
    ]),
    'Field_16K': ('animal-village', [
        ((1, 2, 11, 12, 21, 22, 32, 33, 42, 43, 52), ('animal-village', 'hookshot')),
    ]),
    'Field_16L': 'animal-village',
    'Field_16M': 'animal-village',
    'Field_16N': ('animal-village', [
        ((6, 7, 8, 9, 16, 17, 18, 19, 26, 27, 28, 29, 37, 38, 39, 46, 47, 48, 49, 57, 58, 59), 'desert'),
    ]),
    'Field_16O': 'desert',
    'Field_16P': 'desert',
}


class NoRegion(LookupError):
    """A check on a Field screen / tile the table has no reviewed region for."""


def region_of(room, tile):
    """(region, condition or None) of a Field tile. Raises NoRegion for a screen without a reviewed entry."""
    if room not in SCREENS:
        raise NoRegion(f'{room}: not a Field screen of field_regions.SCREENS')
    e = SCREENS[room]
    if e is None:
        raise NoRegion(f'{room} tile {tile}: no check was reviewed on this screen (field_regions.SCREENS is None)')
    parts = []
    if isinstance(e, tuple) and isinstance(e[1], list):
        e, parts = e
    for tiles, r in parts:
        if tile in tiles:
            e = r
            break
    return (e, None) if isinstance(e, str) else e


_ZONES = {}


def tile_zones(romfs):
    """{Field room: [80 zone ids]} from Field.lvb's grid (1.5-unit cells, 8 bytes each, +5 = zone index; the zone
    records' u32 id). {} without the file."""
    if romfs in _ZONES:
        return _ZONES[romfs]
    out = {}
    p = os.path.join(romfs, 'region_common', 'level', 'Field', 'Field.lvb')
    if os.path.exists(p):
        fh = fixedhash.FixedHash(open(p, 'rb').read())
        zones = [struct.unpack_from('<I', z.data, 0)[0] for z in fh.get(b'zone').data.entries]
        grid = fh.get(b'grid').data
        gh, gw = struct.unpack_from('<HH', fh.get(b'gridConfig').data, 0)
        for row in range(16):
            for col in range(16):
                zs = []
                for z in range(8):
                    for x in range(10):
                        gx, gz = col * 10 + x, row * 8 + z
                        zi = grid[(gz * gw + gx) * 8 + 5] if gz < gh and gx < gw else 255
                        zs.append(zones[zi] if zi < len(zones) else -1)
                out[f'Field_{row + 1:02d}{chr(65 + col)}'] = zs
    _ZONES[romfs] = out
    return out


def area_of(region, zone):
    """The hint / spoiler area of a check: its tile's place (zone), else its region's."""
    return ZONE_AREA.get(zone) or REGION_AREA.get(region, 'mabe-village')


def logic_entry(room, tile, zone, *conds):
    """A Field check's LAS logic entry (standing item): its region, area and condition-basic (the pocket's need and
    the check's own conditions `conds`, ANDed)."""
    region, cond = region_of(room, tile)
    parts = [c for c in (cond,) + conds if c]
    return {'type': 'item', 'subtype': 'standing', 'content': 'rupee-5', 'region': region,
            'spoiler-region': area_of(region, zone),
            'condition-basic': ' & '.join(f'({c})' for c in parts) if parts else None}
