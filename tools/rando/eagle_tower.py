"""Eagle's Tower (Lv07EagleTower, D7) in dungeon room shuffle seeds: the tower starts already collapsed.

Vanilla: the Iron Ball breaks the four pillars on 2F, the top floor comes down onto 3F (event Lv7BrokenPillar) and
only then is the Nightmare door's 3F there. The ball puzzle can't survive a room shuffle (the ball never leaves its
floor, the pillar rooms must stay together), so room-shuffle seeds (Coupled / Decoupled) set the collapse flags when
the game starts (LAS's PlayerStart event, player_start.py) and the tower plays as after the collapse:

  944 / 945 / 947 / 948  TowerPillarBroken_L07_<02B 02C 03B 03C>  the four pillars (rubble)
  957  TowerPillarBrokenAll  2F 04C's stairs (Stairs.bfevfl Lv704C_UpStairs) lead to the post-collapse 3F (04G), the
                             1F 08B warp tile to 03H_Warp
  385  Lv7BrokenPillar       the collapse cutscene is done

Twin rooms: the pre-collapse 3F (files E-H rows 05-08) is never reached again. Its chests share their flags with the
post-collapse twins (952: 05G / 01G, 953: 06H / 02H) and so does the lock block (749: 06E / 02E); only 06E's pots are
lost (pots.MISSABLE_ROOMS), 02E's become checks.

Logic (LAS logic.yml): D7-2F is the tower top - the post-collapse 3F past the Nightmare door, the roof and the
instrument. Vanilla it needs the whole ball puzzle (kill-3-of-a-kind, bombs, hookshot or the flying rooster); collapsed,
Link walks 2F 03A -> 04A -> 04B -> 04C (no cracked wall, no Vegas on that line), climbs 04C's stairs to 04G and on to the
Nightmare door. Kept: the crystal switch (04G's stairs arrive boxed in by orange pegs: set them in 04C first) and the
hookshot / rooster (02F's raised walkway to the roof stairs in 03G; the instrument needs the hookshot anyway).
Checked in game (2026-10-25, muted copies): a seed's first house exit sets the flags (04C's stairs then lead to 04G, a
vanilla run's to 08G); 03A -> 04A -> 04B -> 04C walked without a sword; 03F's Nightmare door opens with the key and leads
into 02F; 02F's hole drops to 02B and on into 1F 06B (the game's own Lv07_06B_from_02B_02F start); with the boss flag
(949) 03G's north door opens and the Organ of Evening Calm is picked up in 02G.
"""

LEVEL = 'Lv07EagleTower'
COLLAPSE_FLAGS = ('TowerPillarBroken_L07_02B', 'TowerPillarBroken_L07_02C', 'TowerPillarBroken_L07_03B',
                  'TowerPillarBroken_L07_03C', 'TowerPillarBrokenAll', 'Lv7BrokenPillar')
# the pre-collapse 3F's pots (its twin 02E's are reachable)
MISSABLE_ROOMS_COLLAPSED = ('Lv07EagleTower_06E',)

COLLAPSED_2F = 'D7-2D & hit-crystal-switch & (hookshot | (rooster & bad-pets)) & nightmare-key-D7'


def collapsed(settings):
    """The seed starts with the tower collapsed: dungeon room shuffle is on."""
    return settings.get('dungeon-room-shuffle', 'off') not in ('off', None, False)


def apply_logic(logic_defs):
    """The collapsed tower's logic (before anything reads D7-2F: souls, pots, room shuffle)."""
    node = logic_defs['D7-2F']
    node['condition-basic'] = COLLAPSED_2F
    for lv in ('advanced', 'glitched', 'hell', 'trick'):
        node.pop('condition-' + lv, None)
    return 1
