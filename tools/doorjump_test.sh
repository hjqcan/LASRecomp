#!/bin/bash
# (Link is warped to the dungeon's entrance (ENTRANCE overrides), then placed at the doorway: a cell's centre can be a pit)
# Walk every mismatched doorway of a generated-dungeon seed (rando/<seed>/door_jumps.tsv) and check Link ends up in
# the room it leads to: tools/doorjump_test.sh <seed> [first] [count]. One game run per doorway (warp into the
# cell, LARECOMP_DOORJUMP_TEST starts after the dungeon title card (Link is held while it shows: jumps undone), puts Link 1.2 inside the doorway and pushes the stick outward). Fresh scratch saves only.
# Same-side doorways (dungeon_gen.chaos_pairs: every doorway on one wall of a single-screen room leads to the same
# place) are checked first, without the game: a wall whose doorways lead to different rooms is a GROUP FAIL.
# DJ_GROUPS=1: walk only those walls' doorways.
# Results: build/doorjump_<seed>.txt (+ a screenshot per doorway in build/doorjump_<seed>/).
SEED=$1; FIRST=${2:-1}; COUNT=${3:-999}
JOB=${DJ_JOB:-$SEED}  # DJ_JOB=<name>: several jobs of one seed in parallel (own saves, shots, results)
OUT=build/doorjump_$JOB.txt; SHOTS=build/doorjump_$JOB
rm -rf "$SHOTS"; mkdir -p "$SHOTS"; : > "$OUT"
PY=third_party/LAS-Randomizer/venv/bin/python
[ -x "$PY" ] || PY=python3
JOBS=$SHOTS/jobs.tsv
# the doorways to walk (level, cell, side, centre, cell he must end up in) + the same-side check
"$PY" - "rando/$SEED" "$JOBS" "${DJ_GROUPS:-0}" <<'PY' | tee -a "$OUT"
import json, os, sys
sys.path.insert(0, 'tools/rando')
sys.path.insert(0, 'third_party/LAS-Randomizer')
import dungeon_gen as dg, rooms
seed_dir, jobs_path, only_groups = sys.argv[1], sys.argv[2], sys.argv[3] == '1'
cat = json.load(open('tools/rando/room_catalog.json'))
rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(seed_dir, 'door_jumps.tsv')) if l.strip()]
cells = {}
for l in open(os.path.join(seed_dir, 'dungeon_map.tsv')):
    lv, cell = l.split('\t')[:2]
    cells.setdefault(lv, set()).add(cell)


def neighbour(cell, side):
    c, r = dg.cell_pos('x_' + cell)
    dc, dr = rooms.STEP[side]
    return dg.cell_name('x', c + dc, r + dr)[2:]


def target(lv, cell, side, slot):  # (cell, side) the doorway leads into
    for w in rows:
        if w[0] == lv and w[1] == cell and w[2] == side and int(w[3]) == slot:
            return (w[5], w[6]) if len(w) > 6 else (neighbour(cell, side), rooms.OPP[side])
    return neighbour(cell, side), rooms.OPP[side]


groups = []  # [(level, [(cell, side, centre)])]
for lv in sorted(cells):
    # one cell's side with several doorways that jump (Chaos / Madness)
    sides = {}
    for w in rows:
        if w[0] == lv:
            sides.setdefault((w[1], w[2]), set()).add(int(w[3]))
    for (cell, s), slots in sorted(sides.items()):
        if len(slots) > 1:
            groups.append((lv, [(cell, s, x) for x in sorted(slots)]))
bad = 0
for lv, members in groups:
    ts = {target(lv, c, s, x) for c, s, x in members}
    bad += len(ts) != 1
    print('[group] %s %s -> %s: %s' % (lv, ' '.join('%s%s%d' % m for m in members),
                                       ' / '.join('%s %s' % t for t in sorted(ts)), 'OK' if len(ts) == 1 else 'GROUP FAIL'))
print('[group] %d walls with several doorways, %d lead to more than one place' % (len(groups), bad))


def floor(lv, cell, side, slot):  # the floor in the doorway in the seed's room (raised doorways: 1.5)
    import struct
    from RandomizerCore.Tools import leb
    room = '%s_%s' % (lv, cell)
    p = rooms._leb_path(os.path.join(seed_dir, 'romfs'), room)
    g = rooms._grid(leb.Room(open(p if os.path.exists(p) else rooms._leb_path('extracted/romfs', room), 'rb').read()))
    g = g[b'data'].data
    a = int(slot) // 2
    for i in range(4):  # the first walkable tile from the edge inward (as dungeon_gen.landing_depth, edge included)
        r, c = {'N': (i, a), 'S': (7 - i, a), 'W': (min(a, 7), i), 'E': (min(a, 7), 9 - i)}[side]
        o = 0x10 * (min(r, 7) * 10 + min(c, 9))
        t, h = g[o:o + 3], struct.unpack_from('<f', g, o + 12)[0]
        if not (t[0] & 2 and not t[2] & 8):
            return h if h > 0.3 else 0.0
    return 0.0


with open(jobs_path, 'w') as f:
    if only_groups:
        seen = set()
        import room_logic
        for lv, members in groups:
            for c, s, x in members:
                room = '%s_%s' % (lv, c)  # (the walls' rooms stay home)
                if room in cat and room_logic.door_terms(room_logic.own_doors(cat, room, s, x), 1,
                                                         room_types=cat[room].get('types', ())) is None:
                    print('[group] %s %s%s%d: a one-way door - no way out here (not walked)' % (lv, c, s, x))
                    continue
                if (lv, c, s, x) not in seen:
                    seen.add((lv, c, s, x))
                    f.write('%s\t%s\t%s\t%d\t%s\t%g\n' % (lv, c, s, x, target(lv, c, s, x)[0], floor(lv, c, s, x)))
    else:
        for w in rows:
            f.write('%s\t%s\t%s\t%s\t%s\t%g\n' % (w[0], w[1], w[2], w[3], w[5] if len(w) > 6 else neighbour(w[1], w[2]),
                                                floor(w[0], w[1], w[2], w[3])))
PY
n=0
while IFS=$'\t' read -r level cell side slot expect fl; do
    n=$((n + 1))
    [ $n -lt "$FIRST" ] && continue
    [ $n -ge $((FIRST + COUNT)) ] && break
    case $level in Lv02*|Lv10*) ent=08C;; Lv03*|Lv07*) ent=08B;; Lv05*) ent=08H;; *) ent=08D;; esac  # dungeon_gen ENTRANCES
    tools/fresh_save.sh "djtest_$JOB" >/dev/null 2>&1  # (per seed: suites of different seeds run in parallel)
    LARECOMP_DOORJUMP_TEST="3100:$cell:$side:$slot:$fl" LARECOMP_CHEATS=hearts LARECOMP_RANDO_SEED=$SEED \
    LARECOMP_SAVE_DIR="build/djtest_$JOB" LARECOMP_WINDOW=640x360 SHOT_ID="dj_$JOB" \
    LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6" \
    LARECOMP_WARP_AT="2700:$level:${level}_${ENTRANCE:-$ent}" tools/shot.sh 4000 600 </dev/null 2>/dev/null
    cp "build/window_dj_$JOB.png" "$SHOTS/${level:2:2}-$cell$side$slot.png"
    res=$(grep '\[doorjump-test\].*->' "build/run_shot_dj_$JOB.log" | tail -1)
    [ -z "$res" ] && res="[doorjump-test] $cell $side -> no result (crash? $(grep -c '\*\*\* signal' "build/run_shot_dj_$JOB.log") signals)"
    echo "$level $slot (into $expect) $res" | tee -a "$OUT"
done < "$JOBS"
# contact sheet: every doorway's last frame (Link must be visible in the room he walked into)
python3 - "$SHOTS" <<'PY'
import os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
fs = sorted(f for f in os.listdir(d) if f.endswith('.png') and f != 'sheet.png')
if fs:
    w, h, cols = 480, 270, 3
    sheet = Image.new('RGB', (w * cols, h * ((len(fs) + cols - 1) // cols)))
    for i, f in enumerate(fs):
        im = Image.open(os.path.join(d, f)).resize((w, h))
        ImageDraw.Draw(im).text((8, 30), f[:-4], fill=(255, 255, 0))
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(os.path.join(d, 'sheet.png'))
PY
walked=$(grep -c '^Lv.*doorjump-test' "$OUT"); pass=$(grep -c 'doorjump-test.*PASS' "$OUT")
echo "$pass pass / $((walked - pass)) other; $(grep -c 'GROUP FAIL' "$OUT") wall(s) leading to more than one place -> $OUT"
