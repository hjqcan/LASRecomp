"""Corrections to the room logic (room_logic.py) from playing the rooms (room review, 2026-11-02; notes in
room_review_answers.txt). The tile grid can't tell a rail from a ledge, a block from a wall or cracked floor from a
pit, and doesn't know which crystal colour is down - these say what the game does. Tiles are (row, col) of the 8 x 10
grid; areas are looked up from them, so the entries survive changes to how areas are found.

ROOMS[room]:
  solid   tiles made wall (blocks that seal a part off, rails)
  floor   tiles made floor at height 0 (cracked floor, shallow channels read as deep water)
  cut     [(a, b)]: no move from a's area to b's (rails under a ledge, a hookshot that doesn't work)
  cut2    the same, both ways
  add     [(a, b, req)]: a move from a's area to b's
  add2    the same, both ways
  check   (tile, req): the room's checks also need req (a chest behind pots, one that appears for an arrow)
  door    {(side, slot): req | None | {'in': .., 'out': ..}}: passing that doorway also needs req (None: never)
PASSAGES[{room, room}]: what the side-view passage between their stairs needs (default: room_logic.PASSAGE)

req: alternatives joined by '|', each a set of atoms joined by '&' ('free' = nothing). Aliases below.

Approximate: crystal blocks (orange / blue) - a crossing that needs one colour down needs something that hits
crystal switches (rs-switch-shutter; the switch can be hit anywhere in the dungeon, the state isn't tracked)."""
ALIAS = {'jump': {'rs-jump'}, 'longjump': {'rs-long-jump'}, 'rooster': {'rooster', 'bad-pets'},
         'switch': {'rs-switch-shutter'}, 'pot': {'break-pot'}, 'crystal': {'break-crystal'}, 'arrows': {'shoot-arrows'},
         'fight': {'rs-shutter'}, 'free': set()}


def alts(s):
    """'a & b | c' -> [frozenset({a, b}), frozenset({c})]"""
    out = []
    for alt in s.split('|'):
        atoms = set()
        for a in alt.split('&'):
            a = a.strip()
            atoms |= ALIAS.get(a, {a} if a else set())
        out.append(frozenset(atoms))
    return out


ROOMS = {
    # Tail Cave
    'Lv01TailCave_05C': {'cut': [((1, 7), (1, 3))]},  # the raised east part: no way down to the floor
    'Lv01TailCave_05D': {'cut': [((1, 0), (6, 4))]},  # the chest's floor and the strip below: no way between
    # Bottle Grotto
    'Lv02BottleGrotto_02D': {'cut2': [((2, 1), (5, 1))], 'add2': [((2, 1), (5, 1), 'jump')]},  # (pots: no hookshot)
    'Lv02BottleGrotto_05G': {'door': {('N', 10): 'jump'}},  # the gap before the Nightmare door
    'Lv02BottleGrotto_07F': {'solid': [(3, 3), (4, 3)]},  # blocks seal the west pocket (its key comes from the west)
    # Key Cavern
    'Lv03KeyCavern_02B': {'door': {('E', 6): 'bombs'}},  # a bombable wall, not a shutter
    'Lv03KeyCavern_02D': {'cut': [((2, 7), (3, 3))], 'solid': [(4, 7), (5, 7)],  # the walkway: no jump down; the chest
                          'add2': [((3, 8), (6, 8), 'hookshot | longjump')]},  # end only by hookshot / a long jump
    'Lv03KeyCavern_03A': {'cut': [((0, 8), (3, 4))]},  # the north-east corner: no jump down
    'Lv03KeyCavern_03D': {'solid': [(2, 7), (3, 7), (4, 7), (5, 7), (6, 7)],
                          'add2': [((3, 8), (3, 1), 'longjump')]},  # the north steps <-> the bombable walls
    'Lv03KeyCavern_04B': {'solid': [(3, 2), (4, 2)], 'add': [((3, 1), (3, 3), 'switch')],  # corridor -> floor, never back
                          'check': ((3, 3), 'switch')},  # the chest inside crystal blocks
    'Lv03KeyCavern_06C': {'check': ((3, 5), 'switch')},  # the chest: blue blocks down
    # Angler's Tunnel
    'Lv04AnglersTunnel_03D': {'door': {('S', 4): None}},  # the south-west opening: a rail
    'Lv04AnglersTunnel_04F': {'cut': [((4, 0), (1, 0))]},  # the chest pen: railed off from the floor above
    'Lv04AnglersTunnel_05D': {'check': ((3, 6), 'flippers | rooster')},  # the chest on its water ledge
    'Lv04AnglersTunnel_05E': {'cut': [((0, 7), (0, 6)), ((0, 7), (7, 5))]},  # the east walkway: railed
    'Lv04AnglersTunnel_05F': {'solid': [(2, 4), (2, 6), (2, 7), (2, 8), (4, 3), (4, 4), (4, 6), (4, 7), (4, 8)],
                              'add2': [((1, 5), (6, 5), 'longjump | rooster'), ((3, 8), (3, 2), 'longjump | rooster'),
                                       ((3, 8), (6, 5), 'rooster'), ((3, 8), (1, 5), 'rooster')]},  # the pit splits it
    'Lv04AnglersTunnel_05G': {'check': ((3, 7), 'bombs')},  # the chest: bomb the cracked block
    'Lv04AnglersTunnel_06C': {'cut': [((1, 1), (4, 1))]},  # the stairs pen drops to the floor, never back
    'Lv04AnglersTunnel_06E': {'solid': [(5, 7), (5, 8), (5, 9)],  # the east walkway: railed off
                              'add': [((6, 1), (2, 2), 'free')]},  # the raised floor <-> the shallow water
    'Lv04AnglersTunnel_06F': {'add': [((1, 4), (3, 2), 'rooster')]},  # the chest island (the pot: no hookshot)
    'Lv04AnglersTunnel_08E': {'check': ((4, 4), 'crystal')},  # the chest: break the crystals
    # Catfish's Maw
    'Lv05CatfishsMaw_01C': {'door': {('W', 8): 'crystal'}},  # crystals between the door and the stairs
    'Lv05CatfishsMaw_02E': {'add2': [((1, 7), (2, 2), 'hookshot | rooster')]},  # the hook post makes a bridge
    'Lv05CatfishsMaw_02F': {'solid': [(1, 1), (1, 2), (2, 1), (2, 2), (2, 3), (3, 3), (3, 4), (4, 4), (4, 5), (5, 5),
                                      (5, 6), (6, 6), (6, 7), (6, 8)],
                            'add': [((1, 5), (6, 2), 'free')]},  # north / east push through to the south, never back
    'Lv05CatfishsMaw_02G': {'solid': [(2, 7), (2, 8)], 'add2': [((7, 8), (1, 8), 'hookshot | rooster')]},
    'Lv05CatfishsMaw_03G': {'solid': [(0, 6), (1, 6), (2, 6), (3, 6), (4, 6)],
                            'add2': [((1, 5), (1, 7), 'hookshot | rooster')]},  # the chest corner
    'Lv05CatfishsMaw_03H': {'check': ((3, 8), 'hookshot')},
    'Lv05CatfishsMaw_05E': {'solid': [(3, 2), (3, 3), (3, 4), (3, 5), (3, 6), (3, 7), (3, 8)],
                            'add': [((2, 4), (4, 3), 'pot')]},  # the pot gap: from the top down only
    'Lv05CatfishsMaw_06E': {'cut2': [((3, 3), (0, 8))], 'add': [((6, 8), (0, 8), 'hookshot | rooster'),
                                                               ((0, 8), (6, 8), 'rooster')]},
    'Lv05CatfishsMaw_08G': {'solid': [(2, 0), (3, 0), (3, 1), (3, 2), (3, 3), (3, 4), (3, 5), (3, 6), (3, 7), (3, 8)],
                            'add2': [((1, 1), (1, 7), 'hookshot | rooster')]},  # the ledge row; the chest across
    # Face Shrine
    'Lv06FaceShrine_03G': {'floor': [(6, 4), (6, 5), (6, 6), (6, 7), (7, 4), (7, 5), (7, 6), (7, 7)],
                           'check': ((2, 6), 'pot')},  # the chest behind pots; the channel is walked
    'Lv06FaceShrine_04A': {'door': {('N', 10): 'switch'}},  # blue down to reach the pots for the pot door
    'Lv06FaceShrine_04B': {'cut': [((0, 7), (3, 3)), ((6, 7), (3, 3))]},  # rails: no jumps down
    'Lv06FaceShrine_04C': {'cut': [((1, 0), (3, 1)), ((6, 0), (3, 1))]},
    'Lv06FaceShrine_04D': {'cut': [((1, 0), (3, 1)), ((6, 0), (3, 1))]},
    'Lv06FaceShrine_04G': {'floor': [(r, c) for r in range(8) for c in range(4, 7)]},
    'Lv06FaceShrine_05A': {'cut': [((4, 9), (4, 8))], 'add2': [((4, 9), (4, 8), 'switch')]},  # blue down, both ways
    'Lv06FaceShrine_05B': {'door': {('W', 9): 'switch'}},  # orange down joins the west side
    'Lv06FaceShrine_05G': {'floor': [(0, 6), (1, 6), (2, 5), (2, 6), (3, 5), (3, 6), (4, 5), (4, 6), (5, 2), (5, 3),
                                     (5, 4), (5, 5), (5, 6), (6, 2), (7, 2)]},
    'Lv06FaceShrine_06G': {'floor': [(0, 2), (1, 2), (2, 2), (3, 2), (4, 2), (5, 2), (5, 3), (5, 4), (5, 5)]},
    'Lv06FaceShrine_07B': {'add2': [((4, 2), (5, 2), 'free'), ((4, 7), (5, 7), 'free')],  # steps to the alcoves
                           'door': {('N', 10): 'bracelet:2'}},  # the elephants in front of the north door
    'Lv06FaceShrine_08B': {'door': {('N', 10): 'switch'}},  # the stairs in a ring of blue blocks
    'Lv06FaceShrine_08G': {'door': {('N', 17): 'switch & jump'}, 'add2': [((4, 1), (4, 3), 'switch & jump')]},
    # Eagle's Tower
    'Lv07EagleTower_02E': {'door': {('S', 7): 'switch'}},  # blue down joins the key block's side
    'Lv07EagleTower_02F': {'add': [((3, 2), (3, 7), 'hookshot')]},  # up the steps, the hookshot across
    'Lv07EagleTower_03B': {'solid': [(r, c) for r in range(1, 7) for c in (1, 3)]},  # the corridor between the columns
    'Lv07EagleTower_03G': {'solid': [(1, 7), (2, 7), (3, 4), (3, 5), (3, 6), (3, 7)],
                           'add': [((1, 3), (1, 4), 'free')]},  # the shutter pocket: only dropped into from the west
    'Lv07EagleTower_04B': {'solid': [(1, 3), (1, 4), (3, 1), (3, 2), (3, 3), (3, 4), (3, 6), (4, 6), (4, 7), (4, 8)]},
    'Lv07EagleTower_04D': {'door': {('N', 13): None}},  # a rail
    'Lv07EagleTower_04F': {'door': {('N', 3): {'out': None}}},  # a block pushed from the north only
    'Lv07EagleTower_05B': {'door': {('S', 5): 'switch', ('S', 15): 'switch'}},
    # Turtle Rock
    'Lv08TurtleRock_02E': {'cut2': [((3, 6), (1, 3))], 'add': [((6, 2), (1, 3), 'hookshot'), ((6, 2), (3, 6), 'rooster'),
                                                              ((6, 1), (3, 6), 'rooster'), ((1, 3), (3, 6), 'rooster')]},
    'Lv08TurtleRock_03C': {'add': [((1, 0), (1, 6), 'hookshot'), ((1, 0), (4, 4), 'free'), ((1, 6), (4, 4), 'free')]},
    'Lv08TurtleRock_03F': {'cut': [((1, 0), (4, 1))]},  # railed
    'Lv08TurtleRock_03G': {'floor': [(r, c) for r in range(1, 7) for c in range(1, 9) if (r, c) not in
                                     ((1, 1), (2, 1), (3, 1), (4, 1), (5, 1))],  # cracked floor, not pits
                           'add': [((2, 1), (2, 2), 'free')]},  # the west ledge hops down
    'Lv08TurtleRock_04C': {'check': ((3, 2), 'arrows')},  # its chest: stand on the block, an arrow at the statue
    'Lv08TurtleRock_04E': {'solid': [(2, c) for c in range(1, 9)], 'cut2': [((3, 1), (5, 3))],
                           'add': [((1, 1), (5, 3), 'hookshot'), ((1, 1), (3, 1), 'free'), ((5, 3), (3, 1), 'free')]},
    'Lv08TurtleRock_04F': {'cut2': [((1, 1), (1, 5))], 'add2': [((1, 1), (1, 5), 'jump')]},
    'Lv08TurtleRock_05C': {'cut': [((3, 8), (3, 1))]},  # east -> west: never
    'Lv08TurtleRock_06D': {'cut2': [((1, 1), (6, 4))], 'add2': [((1, 1), (6, 4), 'rooster')]},  # (or 06E's machine)
    'Lv08TurtleRock_07E': {'door': {('S', 10): 'pot', ('E', 8): 'pot'}},  # the switch under a pot
    'Lv08TurtleRock_07G': {'cut': [((3, 0), (1, 4))], 'add': [((3, 0), (1, 4), 'rooster'), ((1, 4), (3, 0), 'free')]},
}

PASSAGES = {
    frozenset({'Lv04AnglersTunnel_05C', 'Lv04AnglersTunnel_05D'}): 'flippers',
    frozenset({'Lv05CatfishsMaw_02E', 'Lv05CatfishsMaw_03F'}): 'flippers',
    frozenset({'Lv05CatfishsMaw_03E', 'Lv05CatfishsMaw_05E'}): 'feather & hookshot',
    frozenset({'Lv08TurtleRock_01E', 'Lv08TurtleRock_05G'}): 'feather & hookshot',
    frozenset({'Lv08TurtleRock_03B', 'Lv08TurtleRock_06E'}): 'feather & rod',
    frozenset({'Lv08TurtleRock_03D', 'Lv08TurtleRock_06F'}): 'feather & rod',
    frozenset({'Lv08TurtleRock_06H', 'Lv08TurtleRock_08E'}): 'feather & fight',  # 08E's stairs: after its enemies
}


def door(room, side, slot, way):
    """Extra requirement for passing a doorway ('in' / 'out'): frozenset, or None (never), or frozenset() (nothing)."""
    v = ROOMS.get(room, {}).get('door', {}).get((side, slot), frozenset())
    if isinstance(v, dict):
        v = v.get(way, frozenset())
    if v is None or isinstance(v, frozenset):
        return v
    a = alts(v)
    assert len(a) == 1, (room, side, slot)  # (a door requirement is one set of atoms: door_terms ANDs them in)
    return a[0]
