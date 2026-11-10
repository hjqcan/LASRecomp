#!/usr/bin/env python3
"""Triforce hunt (LAS Recomp randomizer option "Triforce hunt", request keys triforce-hunt / triforce-extra).

The seed's goal is N Triforce pieces; the last one ends the game. Pieces placed = needed + extra (rounded up).
  pool      the pieces replace filler left after the vanilla locations took theirs (rupees, then traps, low-value refills
            and Chamber Stones; with pots / bushes / digs
            their filler too, most plentiful first; in the shuffler). Too little: generation stops with a clear message.
  logic     only the NEEDED count must be reachable: the shuffler (third_party/LAS-Randomizer shuffler.py, 'triforce'
            items) places that many with reachability checks, after making sure that many free reachable locations
            exist (as LAS does for seashells); the extras go anywhere. The beatable check counts reachable pieces
            instead of the instruments + the Nightmare.
  item      one Items.gsheet row (the Secret Medicine's, Piece of Power model, held up drawn gold by the runtime:
            runtime/hle/triforce.c), itemID ITEM_ID. Each placed piece has its own index (0..placed-1): the runtime's
            AddItem hook (runtime/hle/keysanity.c rt_add_item -> triforce.c) sets that piece's GlobalFlags bit, so the
            count is a popcount and the game's own save keeps it.
  get       the item-get chains (LAS item_get.insertTriforceGet, chests too) give the piece BEFORE the hold-up, so the
            runtime has already set the count flags (TriforceRemain0..5 = pieces still needed, binary; TriforceDone)
            when the chain picks the message entry: "You collected a Triforce piece!!" + "N pieces remain", or on the
            last needed piece "You've completed the Triforce!", then the Wind Fish's white fade, SaveForGameClear and
            GoToEnding (the game's own ending movie and staff roll). (The get sequence only shows its entry's first
            message: the choice is made in the giving flow.)
Runtime data: rando/<seed>/triforce.tsv (needed, placed, item id, first bit flag, remain / done flags, model spots).
"""
import glob
import math
import os

ITEM_KEY = 'TriforcePiece'
LAS_NAME = 'triforce-piece'
ITEM_ID = 71  # Items.gsheet itemID nothing else uses (vanilla gaps 71-78; souls 82-99 103 107, pets 79-81, shop 230+)
MODEL = {'model-path': 'ItemPowerPiece.bfres', 'model-name': 'PowerPiece', 'npc-key': 'ItemPowerPiece'}
REMAIN_BITS = 6  # remaining count 0..63 as flags TriforceRemain0..5
EXTRA_PCT = {0: 0, 25: 25, 50: 50}
_COLOR_ON = b'\x0e\x00\x00\x03\x00\x04\x00\xd9\x45\x6d\xff'  # (the game's item-name colour / plain white)
_COLOR_OFF = b'\x0e\x00\x00\x03\x00\x04\x00\xed\xed\xed\xff'
_PAGE = b'\x0e\x00\x00\x04\x00\x00\x00'


def counts(settings):
    """(needed, placed) or (0, 0) when the hunt is off."""
    needed = int(settings.get('triforce-hunt', 0) or 0)
    if needed <= 0:
        return 0, 0
    pct = int(settings.get('triforce-extra', 0) or 0)
    return needed, needed + math.ceil(needed * pct / 100)


def add_to_pool(item_defs, settings):
    """Register the piece item; the shuffler swaps `placed` filler items of the pool for pieces once the vanilla
    locations have taken theirs (and stops with a clear message if there isn't enough filler)."""
    needed, placed = counts(settings)
    item_defs[LAS_NAME] = dict(MODEL, **{'type': 'triforce', 'quantity': 0, 'item-key': ITEM_KEY})
    settings['triforce-needed'], settings['triforce-placed'] = needed, placed
    return f'{placed} pieces, {needed} needed'


def _alloc(sheet, names, out_dir):
    """Contiguous GlobalFlags indices after every one in use: below 2048 (the game's own save) when they fit."""
    used = [int(v['index']) for v in sheet['values']]
    for base, limit in ((1240, 2048), (2048, 3800)):
        nxt = max([base] + [u + 1 for u in used if base <= u < limit])
        if nxt + len(names) <= limit:
            if base >= 2048:
                with open(os.path.join(out_dir, 'flags_ext'), 'a') as f:
                    f.write('triforce hunt: event flags 2048+ (runtime/hle/flags_ext.c)\n')
            out = {}
            for i, n in enumerate(names):
                sheet['values'].append({'symbol': n, 'index': nxt + i})
                out[n] = nxt + i
            return out
    raise RuntimeError('out of GlobalFlags indices for the Triforce pieces')


def pieces_text(n, wait):
    head = b'You collected a ' + _COLOR_ON + b'Triforce piece' + _COLOR_OFF + b'!!' + wait
    if n <= 0:
        return head + b'\0'
    tail = b'1 piece remains.' if n == 1 else f'{n} pieces remain.'.encode()
    return head + _PAGE + tail + wait + b'\0'


def complete_text(wait):
    return b"You've completed the " + _COLOR_ON + b'Triforce' + _COLOR_OFF + b'!' + wait + b'\0'


def egg_text(wait):
    return (b'The egg stays silent...' + wait + _PAGE + b'This dream ends only when the\n' + _COLOR_ON + b'Triforce' +
            _COLOR_OFF + b' is whole.' + wait + b'\0')


def _egg_shut(event_tools, fc):
    """The egg's Ballad never opens it: every HasAllInstruments switch takes its "not all" case, and after the song
    (Event30, the HUD coming back) a message. Returns the switches changed."""
    n = 0
    for ev in fc.events:
        d = ev.data
        if getattr(d, 'actor_query', None) and d.actor_query.v.v == 'HasAllInstruments':
            if 0 in d.cases:
                event_tools.setSwitchEventCase(fc, ev.name, 1, d.cases[0].v.name)
            else:
                del d.cases[1]
            n += 1
    back = event_tools.findEvent(fc, 'Event30')
    assert back.data.actor_action.v.v == 'PlayInAnim', 'WindFishsEgg.bfevfl: Event30 is not the HUD coming back'
    msg = event_tools.createActionEvent(fc, 'Dialog', 'Show', {'message': 'System:TriforceEggShut'},
                                        back.data.nxt.v.name)
    event_tools.insertEventAfter(fc, 'Event30', msg)
    return n


def _flow_entries(event_tools, fc, needed):
    """Item.bfevfl entries the get sequence shows (one message each): TriforcePiece (no count), TriforcePiece<n>
    (n still needed), TriforceComplete. The giving flows pick the entry (LAS item_get.insertTriforceGet)."""
    names = ['TriforcePiece', 'TriforceComplete'] + [f'TriforcePiece{n}' for n in range(1, needed)]
    for n in names:
        event_tools.addEntryPoint(fc, n)
        event_tools.insertEventAfter(fc, n, event_tools.createActionEvent(fc, 'Dialog', 'Show', {'message': f'System:{n}'}))
    return len(names)


def apply(out_dir, romfs, oead_tools, event_tools, msbt, settings):
    """After LAS's ModsProcess: flags, the item row, the get messages and the ending, rando/<seed>/triforce.tsv."""
    needed, placed = counts(settings)
    out_romfs = os.path.join(out_dir, 'romfs')

    def src(rel):
        p = os.path.join(out_romfs, rel)
        return p if os.path.exists(p) else os.path.join(romfs, rel)

    def dst(rel):
        p = os.path.join(out_romfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    rel = 'region_common/datasheets/GlobalFlags.gsheet'
    sheet = oead_tools.readSheet(src(rel))
    names = [f'TriforceBit{i:02d}' for i in range(placed)] + [f'TriforceRemain{b}' for b in range(REMAIN_BITS)] + \
        ['TriforceDone']
    flags = _alloc(sheet, names, out_dir)
    oead_tools.writeSheet(dst(rel), sheet)

    rel = 'region_common/datasheets/Items.gsheet'
    sheet = oead_tools.readSheet(src(rel))
    assert not any(int(oead_tools.parseStruct(v)['itemID']) == ITEM_ID for v in sheet['values']), 'itemID in use'
    base = next(oead_tools.parseStruct(v) for v in sheet['values'] if v['symbol'] == 'SecretMedicine')
    row = dict(base, symbol=ITEM_KEY, itemID=ITEM_ID, gettingFlag='', npcKey=MODEL['npc-key'])
    sheet['values'].append(oead_tools.dictToStruct(row))
    oead_tools.writeSheet(dst(rel), sheet)

    rel = 'region_common/event/Item.bfevfl'
    flow = event_tools.readFlow(src(rel))
    n_leaves = _flow_entries(event_tools, flow.flowchart, needed)
    event_tools.writeFlow(dst(rel), flow)

    rel = 'region_common/event/WindFishsEgg.bfevfl'
    flow = event_tools.readFlow(src(rel))
    n_egg = _egg_shut(event_tools, flow.flowchart)
    event_tools.writeFlow(dst(rel), flow)

    import keysanity_text as kt
    n_msg = 0
    for f in glob.glob(os.path.join(romfs, 'region*', '*', 'message', 'System.msbt')):
        rel = os.path.relpath(f, romfs)
        m = msbt.Msbt(open(src(rel), 'rb').read())
        wait = next((r for k, g, r in reversed(kt._tokens(m.texts.get('GetBottle', b''))) if g == kt.WAIT), b'')
        m.set('TriforcePiece', pieces_text(0, wait))
        for n in range(1, needed):
            m.set(f'TriforcePiece{n}', pieces_text(n, wait))
        m.set('TriforceComplete', complete_text(wait))
        m.set('TriforceEggShut', egg_text(wait))
        with open(dst(rel), 'wb') as fh:
            fh.write(m.pack())
        n_msg += 1

    with open(os.path.join(out_dir, 'triforce.tsv'), 'w') as f:
        f.write(f'needed\t{needed}\nplaced\t{placed}\nitem\t{ITEM_ID}\nbits\t{flags["TriforceBit00"]}\n'
                f'remain\t{flags["TriforceRemain0"]}\ndone\t{flags["TriforceDone"]}\n')
    return (f'{placed} pieces, {needed} needed (flags {min(flags.values())}-{max(flags.values())}), '
            f'{n_leaves} message entries, {n_msg} message files, egg shut ({n_egg} switches)')


def write_spots(out_dir, leb, placements):
    """'spot' lines in triforce.tsv: where a piece's Piece of Power model stands in the world (standing items, the
    hidden items of pots / bushes: LAS-style item actors whose parameters are model file, model, location...): the
    runtime draws it gold there (real Pieces of Power elsewhere stay as they are). Run last: room shuffle moves rooms."""
    spots = []
    for f in sorted(glob.glob(os.path.join(out_dir, 'romfs', 'region_common', 'level', '*', '*.leb'))):
        level = os.path.basename(os.path.dirname(f))
        with open(f, 'rb') as fh:
            data = fh.read()
        if MODEL['model-path'].encode() not in data:
            continue
        for a in leb.Room(data).actors:
            p = a.parameters
            if len(p) > 2 and p[0] == MODEL['model-path'].encode() and isinstance(p[2], bytes):
                if placements.get(p[2].decode(errors='ignore'), '') == LAS_NAME:
                    spots.append(f'spot\t{level}\t{a.posX:.2f}\t{a.posY:.2f}\t{a.posZ:.2f}\n')
    for level, prefix in (('Shop', 'shop-slot'), ('CraneGame', 'trendy-prize')):  # (goods shown as models of their own)
        if any(str(k).startswith(prefix) and v == LAS_NAME for k, v in placements.items()):
            spots.append(f'zone\t{level}\n')
    with open(os.path.join(out_dir, 'triforce.tsv'), 'a') as fh:
        fh.writelines(spots)
    return len(spots)
