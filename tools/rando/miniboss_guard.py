"""Overworld copies of a mini-boss whose own flow gives a check (Lanmola: LAS puts Yarna Desert's item in
Lanmola.bfevfl, mod_generator.lanmolaChanges; Armos Knight: the armos-knight check in DeguArmos.bfevfl,
armosKnightChanges) must not give it again when beaten (a copy keeps coming back), and copies that would show a hint
on a hit (QUIET) stay silent: each copy's switch 2 = a new global flag (OverworldMinibossMarker) that the runtime sets
on the Field (runtime/hle/miniboss.c, settings.json "miniboss-marker-flag"); the flow asks
CheckSawTheFirstEvent(switchIndex 2) before the item get (or the hint) - the original (no switch 2: 0) gets it as
before, a copy (the marker set: 1) goes straight past it.

    apply(out_dir, romfs, leb, event_tools, oead_tools, settings) -> summary
"""
import os

# mini-boss type -> (event flow, actor name in it, the event LAS's item get follows, the event after it (None: the
# flow ends), the original's room in the Field (None: not on the Field - every Field one is a copy))
GUARDS = {
    0x57: ('Lanmola.bfevfl', 'Lanmola', 'Event34', 'Event9', 'Field_13O'),
    0x5B: ('DeguArmos.bfevfl', 'DeguArmos', 'Event47', None, None),
}
# Copies that would talk on a hit: the Giant Buzz Blob's flow shows "Your sword won't work! Try something else!" (its
# BossBlobNoDamage entry) - a hint for its own arena. A copy (switch 2 = the marker) shrugs the sword off silently: the
# entry asks CheckSawTheFirstEvent(switchIndex 2) first, the original (0) still shows it. (The other mini-boss flows
# only talk in their start / end events.) type -> (event flow, actor name in it, entry point)
QUIET = {
    0x61: ('BossBlob.bfevfl', 'BossBlob', 'BossBlobNoDamage'),
}
# Copies' deaths (user 2026-10-18): a mini-boss's <Name>End event turns the camera to it (LookAtGimmick), hides the
# HUD, stops the music, opens its arena's doors and autosaves - and, as any event, holds Link. A copy (switch 2 = the
# marker) takes a short path instead: its death effect (PlayTailorOtherChannelEx, as the original's: Dead_Boss, 4 s -
# the event holds Link for it, user 2026-10-18: accepted for now), its item drop and hiding its models, with no camera,
# HUD, music, door or autosave; the camera stays on Link. The original in its own room plays the whole event as before. Shadow Link's end only looks
# at him: a copy's ends at once. Master Stalfos copies fight his last fight (enemies.py: parameter 3 - the earlier ones
# end with his escape and a line of text) and carry the marker on switch 3 (his switch 2 is his own: MARK_SWITCH).
# type -> (event flow, actor name in it, entry point)
DEATH = {
    0x53: ('Hinox.bfevfl', 'Hinox', 'HinoxEnd'),
    0x51: ('Rola.bfevfl', 'Rola', 'RolaEnd'),
    0x56: ('Dodongo.bfevfl', 'Dodongo', 'DodongoEnd'),
    0x61: ('BossBlob.bfevfl', 'BossBlob', 'BossBlobEnd'),
    0x5B: ('DeguArmos.bfevfl', 'DeguArmos', 'DeguArmosEnd'),
    0x57: ('Lanmola.bfevfl', 'Lanmola', 'LanmolaEnd'),
    0x52: ('Bossblin.bfevfl', 'Bossblin', 'BossblinEnd'),
    0x5F: ('MightPunch.bfevfl', 'MightPunch', 'MightPunchEnd'),
    0x5C: ('Jacky.bfevfl', 'Jacky', 'JackyEnd'),
    0x60: ('StoneHinox.bfevfl', 'StoneHinox', 'StoneHinoxEnd'),
    0x58: ('HiploopHover.bfevfl', 'HiploopHover', 'HiploopHoverEnd'),
    0x21E: ('ShadowLink.bfevfl', 'ShadowLink', 'ShadowLinkEnd'),
    0x59: ('MasterStalfon.bfevfl', 'MasterStalfon', 'MasterStalfon4End'),
}
MARK_SWITCH = {0x59: 3}  # (the switch that carries the marker, if not 2)
# Also plain enemies in dungeons, caves and houses (enemies.yml, Include mini-bosses): copies there too. (The
# Ball-and-chain Soldier 0x55 has no flow - nothing to guard.) The originals in their own rooms (ORIGINAL_ROOMS, by
# key: the dungeon room shuffle keeps keys) keep their whole events: Blaino's end opens his ring's shutters.
ANYWHERE = {0x21E, 0x5F}
ORIGINAL_ROOMS = {0x5F: ('Lv08TurtleRock', 'Lv08TurtleRock_03H')}
MARKED = set(GUARDS) | set(QUIET) | set(DEATH)
MARKER = 'OverworldMinibossMarker'


def _reachable(fc, entry_event):
    seen, stack = {}, [entry_event]
    while stack:
        ev = stack.pop()
        if ev is None or ev.name in seen:
            continue
        seen[ev.name] = ev
        d = ev.data
        kind = type(d).__name__
        if kind in ('ActionEvent', 'JoinEvent', 'SubFlowEvent'):
            stack.append(d.nxt.v)
        elif kind == 'SwitchEvent':
            stack += [c.v for c in d.cases.values()]
        elif kind == 'ForkEvent':
            stack += [f.v for f in d.forks] + [d.join.v]
    return seen.values()


def death_path(event_tools, fc, actor, entry):
    """The copy's short end: (the original's) death effect, item drop and models hidden. Returns its first event or
    None."""
    ep = event_tools.findEntryPoint(fc, entry)
    effect, drop, hide = None, None, {}
    for ev in _reachable(fc, ep.main_event.v):
        d = ev.data
        if type(d).__name__ != 'ActionEvent' or d.actor.v.identifier.name != actor:
            continue
        act, params = d.actor_action.v.v, dict(d.params.data) if d.params else {}
        if act == 'ModelVisibility' and not params.get('visible', True):
            hide.setdefault(params.get('modelIndex', 0), params)
        elif act == 'BossItemDrop' and drop is None:
            drop = params
        elif act == 'PlayTailorOtherChannelEx' and effect is None:
            effect = params
    nxt = None
    for idx in sorted(hide, reverse=True):  # (built back to front: effect, drop, then each model hidden)
        nxt = event_tools.createActionEvent(fc, actor, 'ModelVisibility', hide[idx], nxt)
    if drop is not None:
        nxt = event_tools.createActionEvent(fc, actor, 'BossItemDrop', drop, nxt)
    if effect is not None:
        nxt = event_tools.createActionEvent(fc, actor, 'PlayTailorOtherChannelEx', effect, nxt)
    return nxt


def apply(out_dir, romfs, leb, event_tools, oead_tools, settings):
    common_out = os.path.join(out_dir, 'romfs', 'region_common')
    common_in = os.path.join(romfs, 'region_common')

    def path(rel):
        p = os.path.join(common_out, rel)
        return p if os.path.exists(p) else os.path.join(common_in, rel)

    originals = set()  # (the original's own key: he keeps his item)
    for t, (_, _, _, _, room_name) in GUARDS.items():
        if room_name is None:
            continue
        van = leb.Room(open(os.path.join(common_in, 'level', 'Field', f'{room_name}.leb'), 'rb').read())
        originals |= {a.key for a in van.actors if a.type == t}
    for t, (lv, room_name) in ORIGINAL_ROOMS.items():
        van = leb.Room(open(os.path.join(common_in, 'level', lv, f'{room_name}.leb'), 'rb').read())
        originals |= {a.key for a in van.actors if a.type == t}
    # enemy mode Random: the runtime may roll these anywhere at any load (runtime/hle/enemy_random.c gives each the
    # marker on switch 2, enemies_random.tsv 'mark'): their flows are guarded even if no seeded copy needs it
    rolled = ANYWHERE if settings.get('enemy-mode') == 'random' and settings.get('randomize-enemies') else set()
    levels = os.path.join(common_out, 'level')
    rooms = []
    for lv in sorted(os.listdir(levels)) if os.path.isdir(levels) else []:
        if not os.path.isdir(os.path.join(levels, lv)) or lv.startswith('Panel'):  # (Dampe's chamber: his own)
            continue
        types = MARKED if lv == 'Field' else ANYWHERE
        for f in sorted(os.listdir(os.path.join(levels, lv))):
            if not f.endswith('.leb'):
                continue
            room = leb.Room(open(os.path.join(levels, lv, f), 'rb').read())
            if any(a.type in types and a.key not in originals for a in room.actors):
                rooms.append((os.path.join(levels, lv, f), room, types))
    if not rooms and not rolled:
        return 'no copies'
    # the marker flag (after every other flag: existing indices stay)
    rel = 'datasheets/GlobalFlags.gsheet'
    sheet = oead_tools.readSheet(path(rel))
    have = {str(v['symbol']): int(v['index']) for v in sheet['values']}
    if MARKER in have:
        idx = have[MARKER]
    else:
        import seed_tables
        idx = seed_tables.next_flag(sheet, out_dir)
        sheet['values'].append({'symbol': MARKER, 'index': idx})
        os.makedirs(os.path.join(common_out, 'datasheets'), exist_ok=True)
        oead_tools.writeSheet(os.path.join(common_out, rel), sheet)
    settings['miniboss-marker-flag'] = idx
    kinds, copies = set(rolled), 0
    for f, room, types in rooms:
        for a in room.actors:
            if a.type in types and a.key not in originals:
                sw = list(a.switches)
                sw[MARK_SWITCH.get(a.type, 2)] = (1, idx)
                a.switches = sw
                kinds.add(a.type)
                copies += 1
        with open(f, 'wb') as fh:
            fh.write(room.repack())
    for t in sorted(kinds & set(QUIET)):
        name, actor, entry = QUIET[t]
        flow = event_tools.readFlow(path(f'event/{name}'))
        fc = flow.flowchart
        first = event_tools.findEntryPoint(fc, entry).main_event.v.name
        sw = event_tools.createSwitchEvent(fc, actor, 'CheckSawTheFirstEvent', {'switchIndex': 2}, {0: first})
        event_tools.insertEventAfter(fc, entry, sw)
        os.makedirs(os.path.join(common_out, 'event'), exist_ok=True)
        event_tools.writeFlow(os.path.join(common_out, f'event/{name}'), flow)
    for t in sorted(kinds & set(DEATH)):
        name, actor, entry = DEATH[t]
        flow = event_tools.readFlow(path(f'event/{name}'))
        fc = flow.flowchart
        first = event_tools.findEntryPoint(fc, entry).main_event.v.name
        short = death_path(event_tools, fc, actor, entry)
        sw = event_tools.createSwitchEvent(fc, actor, 'CheckSawTheFirstEvent', {'switchIndex': MARK_SWITCH.get(t, 2)},
                                           {0: first, 1: short} if short else {0: first})
        event_tools.insertEventAfter(fc, entry, sw)
        os.makedirs(os.path.join(common_out, 'event'), exist_ok=True)
        event_tools.writeFlow(os.path.join(common_out, f'event/{name}'), flow)
    for t in sorted(kinds & set(GUARDS)):
        name, actor, before, after, _ = GUARDS[t]
        flow = event_tools.readFlow(path(f'event/{name}'))
        fc = flow.flowchart
        first = event_tools.findEvent(fc, before).data.nxt.v.name  # (LAS's item get, inserted after `before`)
        sw = event_tools.createSwitchEvent(fc, actor, 'CheckSawTheFirstEvent', {'switchIndex': 2},
                                           {0: first, 1: after} if after else {0: first})
        event_tools.insertEventAfter(fc, before, sw)
        os.makedirs(os.path.join(common_out, 'event'), exist_ok=True)
        event_tools.writeFlow(os.path.join(common_out, f'event/{name}'), flow)
    return f'{copies} copies, marker flag {idx}'
