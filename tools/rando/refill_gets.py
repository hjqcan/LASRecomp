#!/usr/bin/env python3
"""Bomb / powder / arrow refills (pot filler, tools/rando/pots.py REFILLS) placed at real checks are refills, not the
item itself.

LAS gives a refill like the item: its get sets the item's found flag (bombs: BombsFound, powder: GetMagicPowder), and
the shared flows that give a chest's or a seashell reward's own item (TreasureBox, ShellMansionPresent) add Bomb /
MagicPowder / Arrow with index -1 - the runtime's refill check (runtime/hle/keysanity.c REFILL_INDEX: a blue rupee
while the item isn't owned) never saw those, so a bomb refill in a chest unlocked bombs. Here:
  - the shared flows add them with the chest's own itemIndex and set the found flag only when it isn't REFILL_INDEX
    (LAS patches CompareInt to test equality: case 0 = equal);
  - a refill's own get (index REFILL_INDEX) sets no found flag;
  - found bombs (shuffle-bombs): a bomb refill's message is the game's own bomb text once the Bomb Bag is found, before
    it "You got a Bomb! Too bad you have nowhere to put it.." (System:BombNoBag) - tools/rando/bomb_bag_text.py
    relabels only the real Bomb Bag;
  - found powder (shuffle-powder), the same: the game's own powder text once the Powder Bag is found, before it "You
    got some Magic Powder! But you have nowhere to put it and it blew away.." (System:PowderNoBag) -
    tools/rando/powder_bag_text.py relabels only the real Powder Bag.

    apply(out_dir, romfs, settings) -> number of flow events changed
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REFILL_INDEX = 80  # (tools/rando/pots.py)
FOUND = {'Bomb': 'unused0424', 'MagicPowder': 'GetMagicPowder', 'Arrow': None}  # (LAS data.BOMBS_FOUND_FLAG)
NO_BAG = {'Bomb': 'BombNoBag', 'MagicPowder': 'PowderNoBag'}  # (the refill's message before the bag)
BAG_SETTING = {'Bomb': 'shuffle-bombs', 'MagicPowder': 'shuffle-powder'}
_COLOR_ON = b'\x0e\x00\x00\x03\x00\x04\x00\xd9\x45\x6d\xff'  # (the game's item-name red / plain white)
_COLOR_OFF = b'\x0e\x00\x00\x03\x00\x04\x00\xed\xed\xed\xff'


def text(wait):
    return b'You got a ' + _COLOR_ON + b'Bomb' + _COLOR_OFF + b'! Too bad you have\nnowhere to put it..' + wait + b'\0'


def powder_text(wait):
    return (b'You got some ' + _COLOR_ON + b'Magic Powder' + _COLOR_OFF + b'! But you have\nnowhere to put it and it '
            b'blew away..' + wait + b'\0')


TEXT = {'Bomb': text, 'MagicPowder': powder_text}


def _action(e):
    try:
        return e.data.actor_action.v.v, e.data.params.data
    except AttributeError:
        return None, None


def _retarget(fc, old, new, skip):
    import keysanity_text as kt
    kt._retarget(fc, old, new, skip)
    for ep in fc.entry_points:
        if ep.main_event.v is old:
            ep.main_event.v = new


def apply(out_dir, romfs, settings):
    import evfl
    from RandomizerCore.Tools import event_tools
    import bomb_bag_text
    found = {k for k, s in BAG_SETTING.items() if settings.get(s)}  # (items that must be found: bombs / powder)
    ev_dir = os.path.join(out_dir, 'romfs', 'region_common', 'event')
    n, no_bag = 0, set()
    for p in sorted(glob.glob(os.path.join(ev_dir, '*.bfevfl'))):
        flow = event_tools.readFlow(p)
        fc = flow.flowchart
        events = list(fc.events)
        shared = any(_action(e)[0] == 'AddItemByKey' and isinstance(_action(e)[1].get('index'), evfl.common.Argument)
                     and _action(e)[1]['index'] == 'itemIndex' for e in events)
        changed = False
        for a in events:
            act, prm = _action(a)
            if act != 'AddItemByKey' or prm.get('itemKey') not in FOUND:
                continue
            item, index = prm['itemKey'], prm.get('index')
            if index == REFILL_INDEX:
                refill = None  # (always a refill)
            elif index == -1 and shared and item in ('Bomb', 'MagicPowder', 'Arrow'):
                prm['index'] = evfl.common.Argument('itemIndex')  # (the flow's argument, as its own give)
                refill = 'itemIndex'  # (a refill when the chest's index says so)
            else:
                continue
            changed = True
            n += 1
            # the found flag: skipped for a refill
            flag = next((e for e in events if _action(e)[0] == 'SetFlag' and FOUND[item] and
                         _action(e)[1].get('symbol') == FOUND[item] and getattr(e.data, 'nxt', None) is not None and
                         e.data.nxt.v is a), None)
            if flag is not None:
                if refill is None:
                    _retarget(fc, flag, a, set())
                else:
                    s = event_tools.findEvent(fc, event_tools.createSwitchEvent(
                        fc, 'FlowControl', 'CompareInt', {'value1': 'itemIndex', 'value2': REFILL_INDEX},
                        {0: a.name, 1: flag.name}))
                    _retarget(fc, flag, s, {s})
            # found bombs / powder: a refill's own message (the bag's or not)
            get = a.data.nxt.v
            gact, gprm = _action(get) if get is not None else (None, None)
            if not (item in found and gact == 'GenericItemGetSequenceByKey' and gprm.get('itemKey') == item):
                continue
            after = get.data.nxt.v.name if get.data.nxt.v is not None else None
            # (Item.bfevfl 'Bomb' / 'MagicPowder': the game's own refill text)
            if refill is None:  # (the get itself: the bag found)
                gprm['messageEntry'] = item
                have = get.name
            else:
                have = event_tools.createActionEvent(fc, 'Link', 'GenericItemGetSequenceByKey',
                                                     {'itemKey': item, 'keepCarry': False, 'messageEntry': item},
                                                     after)
            none = event_tools.createActionEvent(fc, 'Link', 'GenericItemGetSequenceByKey',
                                                 {'itemKey': item, 'keepCarry': False, 'messageEntry': NO_BAG[item]},
                                                 after)
            msg = event_tools.createSwitchEvent(fc, 'EventFlags', 'CheckFlag', {'symbol': FOUND[item]},
                                                {0: none, 1: have})
            if refill is None:
                a.data.nxt.v = event_tools.findEvent(fc, msg)
            else:
                a.data.nxt.v = event_tools.findEvent(fc, event_tools.createSwitchEvent(
                    fc, 'FlowControl', 'CompareInt', {'value1': 'itemIndex', 'value2': REFILL_INDEX},
                    {0: msg, 1: get.name}))
            no_bag.add(item)
        if changed:
            event_tools.writeFlow(p, flow)
    for item in sorted(no_bag):
        bomb_bag_text.add_message(out_dir, romfs, NO_BAG[item], TEXT[item])
    return n
