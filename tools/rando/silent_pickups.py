#!/usr/bin/env python3
"""Silent pickups (prototype): minor items picked up from the ground skip the item-get cutscene, everywhere.

Pieces of Heart, Heart Containers, Secret Seashells, recovery hearts and rupees that Link picks up from the ground -
the item actors of SinkingSword.bfevfl: standing items, pots, bushes, digs, Secret Seashell spots, tree / bomb-hole
fly-outs, in dungeons too - are given without Link.GenericItemGetSequenceByKey (Link holding it up, the fanfare, the
message box): a short sound of the game's own instead (SE below), and for heart pieces / containers / seashells a small
card (runtime/hle/silent_pickups.c, runtime/ui/pickup_flyin.mm): the heart filling in quarters, "+1 Heart" (and a refill),
the seashell count. Rupees and hearts have no card (the HUD shows them).

Small keys, dungeon items (maps, compasses, beaks, Nightmare Keys), Triforce pieces, souls, capacity upgrades, refills
(their "nowhere to put it" message) and every inventory / progression item keep the cutscene, and chests
(TreasureBox.bfevfl) are untouched.

The give itself stays the same Inventory.AddItemByKey (flags, the save, the heart-piece-to-container count all work
as before); the card items carry index SILENT_BASE + 1 + their index, which the runtime's AddItem hook
(runtime/hle/keysanity.c rt_add_item -> rt_silent_add) turns back before the game adds it.

The game's own item actors (ItemCommon.bfevfl: a Secret Seashell buried on a digsanity tile, at a Seashell spot, the
vanilla shells) get the same for Secret Seashells: patch_item_common (their add marked with MARK_FLAG instead).

Runs after every other flow edit (keysanity_text, refill_gets, bomb / powder bag text).

    apply(out_dir, romfs) -> (gets made silent, ItemCommon entries changed)
"""
import os

SILENT_BASE = 3000  # (runtime/hle/silent_pickups.c)
CARD = ('HeartPiece', 'HeartContainer', 'Seashell')  # (the runtime's card)
# the pickup sound (Audio.PlayOneshotSystemSE, the game's own system SEs): the small item / rupee / heart pickup sounds
SE = {'HeartPiece': 'SE_SY_GET_ITEM', 'Seashell': 'SE_SY_GET_ITEM', 'HeartContainer': 'SE_SY_HEART_UP',
      'Heart': 'SE_SY_HEART_RECOVERY', 'Apple': 'SE_SY_HEART_RECOVERY', 'GreenApple': 'SE_SY_HEART_RECOVERY'}
SE_RUPEE = 'SE_SY_GET_RUPY'


def _se(key):
    return SE_RUPEE if key.startswith('Rupee') else SE[key]


def _action(e):
    d = e.data
    if type(d).__name__ != 'ActionEvent':
        return None, None, None
    return d.actor.v.identifier.name, d.actor_action.v.v, d.params.data if d.params else {}


def _wanted(key):
    return isinstance(key, str) and (key in SE or key.startswith('Rupee'))


def _retarget(fc, old, new):
    """everything leading to `old` leads to `new`"""
    for x in fc.events:
        t = type(x.data).__name__
        if t in ('ActionEvent', 'SubFlowEvent', 'JoinEvent') and x.data.nxt.v is old:
            x.data.nxt.v = new
        elif t == 'SwitchEvent':
            for c in x.data.cases.values():
                if c.v is old:
                    c.v = new
        elif t == 'ForkEvent':
            for f in x.data.forks:
                if f.v is old:
                    f.v = new
    for q in fc.entry_points:
        if q.main_event.v is old:
            q.main_event.v = new


def _sound(fc, event_tools, add, key):
    """the pickup sound right before `add`"""
    se = event_tools.findEvent(fc, event_tools.createActionEvent(
        fc, 'Audio', 'PlayOneshotSystemSE', {'label': _se(key), 'volume': 1.0, 'pitch': 1.0}, add.name))
    _retarget(fc, add, se)
    se.data.nxt.v = add


def patch(fc, event_tools):
    """one flowchart (SinkingSword.bfevfl): returns the number of gives made silent"""
    n, done = 0, set()
    for ep in fc.entry_points:
        path, e, add = [], ep.main_event.v, None  # the straight chain up to the first wanted AddItemByKey
        while e is not None and len(path) < 16:
            actor, act, prm = _action(e)
            if actor is None:
                break
            path.append(e)
            if act == 'AddItemByKey' and _wanted(prm.get('itemKey')):
                add = e
                break
            e = e.data.nxt.v
        if add is None or id(add) in done:
            continue
        done.add(id(add))
        prm = _action(add)[2]
        key = prm['itemKey']
        for g in path:
            if _action(g)[1] == 'GenericItemGetSequenceByKey' and _action(g)[2].get('itemKey') == key:
                _retarget(fc, g, g.data.nxt.v)
        idx = prm.get('index', -1)
        if key in CARD and isinstance(idx, int) and -1 <= idx < 1000:
            prm['index'] = SILENT_BASE + 1 + idx
        _sound(fc, event_tools, add, key)
        n += 1
    return n


MARK_FLAG = 'unused0432'  # set around the add of a native Secret Seashell (runtime/hle/silent_pickups.c MARK_FLAG)


def patch_item_common(fc, event_tools):
    """ItemCommon.bfevfl 'get' (the game's own item actors: a Secret Seashell buried on a digsanity tile or at a
    Seashell spot, the vanilla shells): Deactivate -> PlayTailorGettingChannel -> GenericItemGetSequenceByKey ->
    AddItemByKey -> Destroy, all with the actor's itemKey / itemCount / itemIndex. A Secret Seashell skips the jingle
    and the get sequence (the pickup sound instead); its add is marked (MARK_FLAG) for the runtime's card. Returns the number of entries changed."""
    n = 0
    for ep in fc.entry_points:
        first = ep.main_event.v
        actor, act, _ = _action(first) if first is not None else (None, None, None)
        if act != 'Deactivate':
            continue
        chain, e = [], first.data.nxt.v
        while e is not None and _action(e)[0] is not None and len(chain) < 8:
            chain.append(e)
            e = e.data.nxt.v
        add = next((x for x in chain if _action(x)[1] == 'AddItemByKey'), None)
        if add is None or not any(_action(x)[1] == 'GenericItemGetSequenceByKey' for x in chain):
            continue
        prm = dict(_action(add)[2])
        after = add.data.nxt.v.name if add.data.nxt.v is not None else None
        off = event_tools.createActionEvent(fc, 'EventFlags', 'SetFlag', {'symbol': MARK_FLAG, 'value': False}, after)
        give = event_tools.createActionEvent(fc, 'Inventory', 'AddItemByKey', prm, off)
        on = event_tools.createActionEvent(fc, 'EventFlags', 'SetFlag', {'symbol': MARK_FLAG, 'value': True}, give)
        on = event_tools.createActionEvent(fc, 'Audio', 'PlayOneshotSystemSE',
                                           {'label': SE['Seashell'], 'volume': 1.0, 'pitch': 1.0}, on)
        sw = event_tools.createSwitchEvent(fc, 'FlowControl', 'CompareString', {'value1': 'itemKey', 'value2': 'Seashell'},
                                           {0: on, 1: chain[0].name})
        first.data.nxt.v = event_tools.findEvent(fc, sw)
        n += 1
    return n


def _flow(out_dir, romfs, name, fn, event_tools):
    rel = os.path.join('region_common', 'event', name)
    p = os.path.join(out_dir, 'romfs', rel)
    if not os.path.exists(p):
        p = os.path.join(romfs, rel)
    flow = event_tools.readFlow(p)
    n = fn(flow.flowchart, event_tools)
    out = os.path.join(out_dir, 'romfs', rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    event_tools.writeFlow(out, flow)
    return n


def apply(out_dir, romfs):
    """-> (SinkingSword gives made silent, ItemCommon entries changed)"""
    from RandomizerCore.Tools import event_tools
    return (_flow(out_dir, romfs, 'SinkingSword.bfevfl', patch, event_tools),
            _flow(out_dir, romfs, 'ItemCommon.bfevfl', patch_item_common, event_tools))
