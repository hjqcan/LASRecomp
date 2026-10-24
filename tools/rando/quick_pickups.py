#!/usr/bin/env python3
"""Quick pickups: the randomizer's ground items (SinkingSword item actors: pots, bushes, digs, Secret Seashell spots)
whose pickup is only a few plain steps, given by the runtime instead of an event.

Picking up one of those items runs its entry in SinkingSword.bfevfl, and any event the game starts holds Link still
while it runs: running over a bush's rupee he stopped dead for 2-3 frames, every time (user, 2026-10-21). Entries
that only set their flag, play a system sound and add an item (a rupee, a recovery heart, a refill - and with
silent pickups every minor item) need nothing an event gives. This writes them to quick_pickups.tsv; the runtime
(runtime/hle/quick_pickup.c) does those steps itself when such an item is picked up, removes the item actor and
skips the event. Every other entry (an item-get cutscene, a message, a branch) keeps its event.

quick_pickups.tsv, one line per entry: entry name, event flag index (-1: none), Items.gsheet itemID, count, index,
system sound label ('-': none).

Runs after every flow edit.

    apply(out_dir, romfs) -> number of quick entries
"""
import os

STEPS = {('EventFlags', 'SetFlag'), ('Audio', 'PlayOneshotSystemSE'), ('Inventory', 'AddItemByKey'),
         ('SinkingSword', 'Destroy')}


def _src(out_dir, romfs, rel):
    p = os.path.join(out_dir, 'romfs', rel)
    return p if os.path.exists(p) else os.path.join(romfs, rel)


def _action(e):
    d = e.data
    if type(d).__name__ != 'ActionEvent':
        return None
    return d.actor.v.identifier.name, d.actor_action.v.v, dict(d.params.data) if d.params else {}


def quick(ep):
    """(flag symbol or None, sound label or None, AddItemByKey params) when the entry is only the quick steps, each
    once, with an add and a Destroy (pots.py chains SetFlag -> Destroy -> AddItemByKey); else None"""
    e, seen, flag, se, add = ep.main_event.v, set(), None, None, None
    while e is not None:
        a = _action(e)
        if a is None or (a[0], a[1]) not in STEPS or (a[0], a[1]) in seen:
            return None
        seen.add((a[0], a[1]))
        actor, act, prm = a
        if act == 'SetFlag':
            if prm.get('value') is not True:
                return None
            flag = prm.get('symbol')
        elif act == 'PlayOneshotSystemSE':
            se = prm.get('label')
        elif act == 'AddItemByKey':
            if prm.get('autoEquip') or not isinstance(prm.get('itemKey'), str):
                return None
            add = prm
        e = e.data.nxt.v
    return (flag, se, add) if add is not None and ('SinkingSword', 'Destroy') in seen else None


def apply(out_dir, romfs):
    from RandomizerCore.Tools import event_tools, oead_tools
    flow = event_tools.readFlow(_src(out_dir, romfs, 'region_common/event/SinkingSword.bfevfl'))
    flags = {v['symbol']: int(v['index'])
             for v in oead_tools.readSheet(_src(out_dir, romfs, 'region_common/datasheets/GlobalFlags.gsheet'))['values']}
    items = {}
    for v in oead_tools.readSheet(_src(out_dir, romfs, 'region_common/datasheets/Items.gsheet'))['values']:
        r = oead_tools.parseStruct(v)
        items.setdefault(r['symbol'], int(r['itemID']))
    lines = []
    for ep in flow.flowchart.entry_points:
        q = quick(ep)
        if q is None:
            continue
        flag, se, add = q
        if add['itemKey'] not in items or (flag is not None and flag not in flags):
            continue
        lines.append('%s\t%d\t%d\t%d\t%d\t%s\n' % (ep.name, flags[flag] if flag else -1, items[add['itemKey']],
                                                   int(add.get('count', 1)), int(add.get('index', -1)), se or '-'))
    with open(os.path.join(out_dir, 'quick_pickups.tsv'), 'w') as f:
        f.writelines(lines)
    return len(lines)


if __name__ == '__main__':  # quick_pickups.py <seed dir> <romfs>: (re)write an existing seed's table
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'third_party', 'LAS-Randomizer'))
    print(apply(sys.argv[1], sys.argv[2]), 'quick entries')
