#!/usr/bin/env python3
"""Found Magic Powder ("Powder bag", LAS shuffle-powder): its get-item message says what it is, in the game's own
message box - "You found your Powder Bag! Now you can carry Magic Powder again." (the game's own text, "You got some
magic powder! Try to sprinkle it on many things!", reads like a refill). The powder twin of bomb_bag_text.py.

With shuffle-powder the only MagicPowder get (Link.GenericItemGetSequenceByKey) with the default message in the seed's
flows is the found one: the game's other powder gives (the witch, the crane) use GenericItemGetSequence or their own
messages, and a powder refill at a check has its own message (tools/rando/refill_gets.py). Each gets the messageEntry
`PowderBag`: an Item.bfevfl entry showing System:PowderBag, our own text in every language's System.msbt.

Link already holds up the Magic Powder sack for it, so no tint (unlike the Bomb Bag).

    apply(out_dir, romfs, settings) -> number of get events changed (0: not a found-powder seed)
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bomb_bag_text import _COLOR_ON, _COLOR_OFF, add_message  # noqa: E402

ENTRY = 'PowderBag'


def text(wait):
    return (b'You found your ' + _COLOR_ON + b'Powder Bag' + _COLOR_OFF + b'! Now you can\ncarry Magic Powder again.'
            + wait + b'\0')


def apply(out_dir, romfs, settings):
    if not settings.get('shuffle-powder'):
        return 0
    from RandomizerCore.Tools import event_tools
    ev_dir = os.path.join(out_dir, 'romfs', 'region_common', 'event')
    n = 0
    for p in sorted(glob.glob(os.path.join(ev_dir, '*.bfevfl'))):
        flow = event_tools.readFlow(p)
        changed = False
        for e in flow.flowchart.events:
            try:
                act, prm = e.data.actor_action.v.v, e.data.params.data
            except AttributeError:
                continue
            # (a refill's get has its own message: tools/rando/refill_gets.py)
            if act == 'GenericItemGetSequenceByKey' and prm.get('itemKey') == 'MagicPowder' and \
                    not prm.get('messageEntry'):
                prm['messageEntry'] = ENTRY
                changed = True
                n += 1
        if changed:
            event_tools.writeFlow(p, flow)
    if n:
        add_message(out_dir, romfs, ENTRY, text)
    return n
