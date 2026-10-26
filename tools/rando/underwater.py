#!/usr/bin/env python3
"""Underwater checks: the five sunken Heart Pieces and the two sunken Secret Seashells hold randomized items, shown with
the item's own model, and Link still dives for them (as in LAS-Randomizer's 0.5 dev build, which this follows).

Only the game's heart-piece actor (ItemHeartPiece, 0xB0) can be dived for (the SinkingSword actor LAS uses for dry
heart pieces ignores a diving Link), so the sunken spots keep it and its Npc.gsheet entry takes everything from the
actor's own parameters ($n = parameter n, a feature of the game's datasheets):
  $1 / $2  model file / model name of the item it holds
  $3       entry point in SinkingSword.bfevfl run on pickup (actor "SinkingSword": Destroy, then the item get)
  $4       flag set on pickup; the actor only appears while it's off (layout condition)
  $5       'true' when it holds a Secret Seashell (the Seashell Sensor rings for it)
The sunken seashells (ItemSecretSeashell 0x87) become heart-piece actors the same way. Items.gsheet's HeartPiece uses a
copy of the original entry (PatchHeartPiece) so a heart piece held up anywhere else keeps its model. The runtime skips
the item actor's "already in the inventory?" check for heart-piece actors in seeds made with this (settings.json
"underwater-items"; tools/patches.py UNDERWATER_OWNED_SITE): with a randomized item a heart piece owned elsewhere
mustn't hide it - the flag does. Below the water only models whose materials have the shader option under_water are
seen (the heart piece's do); in these seeds every material counts as one (tools/patches.py UNDERWATER_VIEW_SITE).

    apply(mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data) -> spoiler lines
"""
import os

import seed_tables

# location -> (level folder, room, actor type it has in vanilla)
SPOTS = {
    'bay-passage-sunken': ('MadBattersWellEnter', 'MadBattersWellEnter_01B', 0xB0),
    'river-crossing-cave': ('AnimalVillageEnter', 'AnimalVillageEnter_01B', 0xB0),
    'kanalet-moat-south': ('Field', 'Field_08J', 0xB0),
    'south-bay-sunken': ('Field', 'Field_15J', 0xB0),
    'taltal-east-drop': ('EagleTowerExit', 'EagleTowerExit_01A', 0xB0),
    'kanalet-moat-north': ('Field', 'Field_04I', 0x87),
    'taltal-sunken': ('Field', 'Field_02M', 0x87),
}
# flags: the heart pieces use LAS's own (data.HEART_FLAGS, made by flags.py); the seashells get two new ones
SHELL_FLAGS = {'kanalet-moat-north': 'KanaletSunkenShellGet', 'taltal-sunken': 'TaltalSunkenShellGet'}
RAISE = 0.75  # how far above the vanilla floor spot the sunken items float (apply())
# model sizes underwater (others 1.5, the heart piece 1): small or big-at-1 models picked so they read as well as the key
SIZES = {'FullMoonCello': 2.5, 'Mamu': 0.75, 'ManboTamegoro': 0.9}


def _level_dir(romfs, level):
    for d in (level, level.rstrip('0123456789')):
        if os.path.isdir(os.path.join(romfs, 'region_common', 'level', d)):
            return d
    return level


def apply(mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data):
    import oead  # (here: location_rooms.py imports this module for SPOTS alone)
    common_out = os.path.join(out_dir, 'romfs', 'region_common')
    common_in = os.path.join(romfs, 'region_common')

    def path(rel):
        p = os.path.join(common_out, rel)
        return p if os.path.exists(p) else os.path.join(common_in, rel)

    def out(rel):
        p = os.path.join(common_out, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    # flags for the seashells (after every other flag: existing indices stay)
    sheet = oead_tools.readSheet(path('datasheets/GlobalFlags.gsheet'))
    names = {str(v['symbol']) for v in sheet['values']}
    for f in SHELL_FLAGS.values():
        if f not in names:
            sheet['values'].append({'symbol': f, 'index': seed_tables.next_flag(sheet, out_dir)})
    oead_tools.writeSheet(out('datasheets/GlobalFlags.gsheet'), sheet)

    flow = event_tools.readFlow(path('event/SinkingSword.bfevfl'))
    spoiler = []
    for name, (level, room_name, vtype) in SPOTS.items():
        if name not in mods.placements:
            continue
        flag = SHELL_FLAGS.get(name) or data.HEART_FLAGS[name]
        item_key, item_index, model_path, model_name = mods.getItemInfo(name, mods.trap_models)
        if item_key[:3] == 'Rup':
            get_anim = event_tools.createActionEvent(flow.flowchart, 'Inventory', 'AddItemByKey',
                                                     {'itemKey': item_key, 'count': 1, 'index': item_index,
                                                      'autoEquip': False})
        else:
            get_anim = item_get.insertItemGetAnimation(flow.flowchart, item_key, item_index)
        event_tools.addEntryPoint(flow.flowchart, name)
        event_tools.createActionChain(flow.flowchart, name, [
            ('SinkingSword', 'Destroy', {}),
            ('EventFlags', 'SetFlag', {'symbol': flag, 'value': True})], get_anim)

        rel = f'level/{_level_dir(romfs, level)}/{room_name}.leb'
        with open(path(rel), 'rb') as fh:
            room = leb.Room(fh.read())
        acts = [a for a in room.actors if a.type == vtype]
        if not acts:
            raise RuntimeError(f'{name}: no actor {vtype:#x} in {room_name}')
        act = acts[0]
        act.type = 0xB0
        # Seen through the water (a blurred low-resolution view), items on the floor are hard to make out: every one is
        # raised RAISE (the pickup box is lowered as much below, in Npc.gsheet: it stays where the heart piece's was), and
        # other models than the heart piece are drawn half again as large (a few at their own sizes: SIZES, MODEL_SIZES).
        act.posY += RAISE
        if model_name not in ('HeartPiece', 'HeartContainer'):
            act.scaleX = act.scaleY = act.scaleZ = SIZES.get(model_name, data.MODEL_SIZES.get(model_name, 1.5))
        while len(act.parameters) < 8:
            act.parameters.append(b'')
        act.parameters[1] = bytes(model_path, 'utf-8')
        act.parameters[2] = bytes(model_name, 'utf-8')
        act.parameters[3] = bytes(name, 'utf-8')
        act.parameters[4] = bytes(flag, 'utf-8')
        act.parameters[5] = b'true' if item_key == 'Seashell' else b'false'
        if model_name in data.MODEL_ROTATIONS:
            act.rotY = data.MODEL_ROTATIONS[model_name]
        with open(out(rel), 'wb') as fh:
            fh.write(room.repack())
        spoiler.append(f'{name}: {mods.placements[name]}')
    event_tools.writeFlow(out('event/SinkingSword.bfevfl'), flow)

    # Npc.gsheet: ItemHeartPiece from its parameters; PatchHeartPiece = the original, for heart pieces held up elsewhere
    npcs = oead_tools.readSheet(path('datasheets/Npc.gsheet'))
    vals = npcs['values']
    hp = next(v for v in vals if v['symbol'] == 'ItemHeartPiece')
    patch = None
    if not any(v['symbol'] == 'PatchHeartPiece' for v in vals):
        patch = oead_tools.parseStruct(hp)  # (a copy of the original, appended after the edit: an append moves the
        patch['symbol'] = 'PatchHeartPiece'  # array, so `hp` would no longer point into it)
    hp['graphics']['path'] = '$1'
    hp['graphics']['model'] = '$2'
    hp['eventInfo'] = {'eventAsset': 'SinkingSword.bfevfl', 'actorName': 'SinkingSword'}
    hp['eventTriggers'][0]['entryPoint'] = '$3'
    hp['layoutConditions'].append({'category': 1, 'parameter': '$4', 'layoutID': -1})
    hp['shellSensor'].append({'category': 9, 'parameter': '$5'})
    if patch is not None:
        vals.append(oead_tools.dictToStruct(patch))
    # A dive collects the item when Link's dive reaches its collision box (shape 3: parameters = the box's full x / y / z
    # size, centred offset.y above the actor; the actor's scale doesn't change it). Measured 2026-10-24: the heart
    # piece's 0.8 x 0.65 x 0.6 is collected diving within about 0.6 (x) / 0.5 (z) of the centre, and a dive reaches
    # about 1.25 below the water surface, just at the top of that box on the floor of Martha's Bay (15J): dives there
    # missed even dead centre. 3 x 1.3 x 3 is collected within about one tile (1.5) on every side, and its top is still
    # 0.7+ below a swimming Link (swimming over it doesn't collect it). Its centre stays where the heart piece's was
    # (offset 0.25 above the floor spot; the actor is RAISE higher). (After the append, which moves the array; a new
    # buffer, as PatchHeartPiece's copy - dry heart pieces, unchanged - shared the old one's data.)
    hp = next(v for v in vals if v['symbol'] == 'ItemHeartPiece')
    hp['collision']['parameters'] = oead.BufferF32([3.0, 1.3, 3.0])
    hp['collision']['offset']['y'] = 0.25 - RAISE
    oead_tools.writeSheet(out('datasheets/Npc.gsheet'), npcs)
    items = oead_tools.readSheet(path('datasheets/Items.gsheet'))
    for it in items['values']:
        if it['symbol'] == 'HeartPiece':
            it['npcKey'] = 'PatchHeartPiece'
    oead_tools.writeSheet(out('datasheets/Items.gsheet'), items)
    return spoiler
