#!/bin/bash
# Take every stairs / ladder of a seed's generated dungeons and check Link arrives in the room its locator names:
# tools/stairs_test.sh <seed> [level ...]. One hidden, muted game run per level; in it each case is a warp into the
# trigger's room (its own start - a side-view passage's is at its ladder top), then either the stick held up (a
# ladder up out of a side-view passage: put at its foot, then climbing) or Link put next to the trigger and walked
# into it (LARECOMP_PLACE: stairs in a room from each side, side-view exits at floor level from either side). PASS:
# Link was in the target room at some point of the case. Cases: tools/rando/stairs_cases.py.
# Results: build/stairs_<seed>.txt (PASS / FAIL per case) + the run logs build/run_shot_stairs_<seed>_<level>.log.
SEED=$1; shift
P=${STAIRS_PERIOD:-1000}  # frames per case
PY=${LAS_PY:-third_party/LAS-Randomizer/venv/bin/python3}
[ -x "$PY" ] || PY=python3
OUT=build/stairs_$SEED${STAIRS_JOB:+_$STAIRS_JOB}.txt; : > "$OUT"
CASES=$(mktemp); "$PY" tools/rando/stairs_cases.py extracted/romfs "rando/$SEED" > "$CASES" || exit 1
if [ -n "$STAIRS_ONLY" ]; then  # STAIRS_ONLY=<regex>: only the cases whose trigger room ("<room> <up|down>") matches
    awk -F'\t' -v re="$STAIRS_ONLY" '($3 " " $2) ~ re' "$CASES" > "$CASES.only"; mv "$CASES.only" "$CASES"
fi
levels=${*:-$(cut -f1 "$CASES" | uniq)}
for lv in $levels; do
    job="stairs_${SEED}_$lv${STAIRS_JOB:+_$STAIRS_JOB}"
    # warps, input and placements of this level's cases (case k starts at frame 2700 + k * P)
    plan=$(awk -F'\t' -v lv="$lv" -v P="$P" '$1 == lv {
        t = 2700 + n * P
        warp = warp (warp ? "," : "") t ":" lv ":" $3
        x = $4 + 0; z = $6 + 0
        if (z == 0 && $5 >= 5) {  # a side-view ladder up: at its foot, climb (not longer: he would walk on)
            place = place (place ? ";" : "") (t + 350) ":" x ":0"
            input = input "," (t + 450) ":STICK_UP:${STAIRS_CLIMB:-100}"
        } else if (z == 0 && $13 == "LadderDown") {  # a ladder down at a side-view floor: stand on it, press down
            place = place (place ? ";" : "") (t + 350) ":" x ":0:5:D:60"
        } else if (z == 0) {  # a side-view exit at floor level: walk into it from either side
            place = place (place ? ";" : "") (t + 350) ":" (x - 2) ":0:5:R:60;" (t + 500) ":" (x + 2) ":0:5:L:60"
        } else {  # stairs in a room: put him on them, then walk onto them from each side
            place = place (place ? ";" : "") (t + 350) ":" x ":" z ";" (t + 480) ":" x ":" (z + 2) ":5:U:40;" \
                (t + 600) ":" x ":" (z - 2) ":5:D:40;" (t + 720) ":" (x - 2) ":" z ":5:R:40;" (t + 840) ":" (x + 2) ":" z ":5:L:40"
        }
        if ($12 >= 0) flags = flags (flags ? "," : "") $12  # (stairs that appear once the room is cleared: shown)
        n++ } END { print warp "|" input "|" place "|" flags "|" n }' "$CASES")
    IFS='|' read -r warp input place flags n <<< "$plan"
    if [ -z "$STAIRS_NO_RUN" ]; then  # (STAIRS_NO_RUN=1: only read the last run's logs again)
    tools/fresh_save.sh "$job" >/dev/null 2>&1
    LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_CHEATS=hearts LARECOMP_RANDO_SEED=$SEED \
    LARECOMP_SAVE_DIR="build/$job" LARECOMP_WINDOW=640x360 SHOT_ID="$job" LARECOMP_LOG_LINK=10 \
    LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6$input" \
    LARECOMP_WARP_AT="$warp" LARECOMP_PLACE="$place" LARECOMP_SET_FLAGS="$flags" \
    tools/shot.sh $((2700 + n * P)) $((120 + (2700 + n * P) / 30)) </dev/null 2>/dev/null
    fi
    # Link's positions during each case: in the target room's box at some point?
    awk -F'\t' -v lv="$lv" -v P="$P" -v lf="build/run_shot_$job.log" '
        BEGIN { while ((getline l < lf) > 0) if (l ~ /^\[link\] f[0-9]+ /) {
                    split(l, a, /[ (),]+/); f = substr(a[2], 2) + 0; X[f] = a[3]; Y[f] = a[4]; Z[f] = a[5] } }
        $1 == lv { t = 2700 + n * P; n++; ok = 0; x = ""
            # in the target room at any time of the case (after the warp in): later he may walk on, out of it
            for (f = t + 100; f < t + P && !ok; f++) if (f in X) {
                x = X[f]; y = Y[f]; z = Z[f]
                ok = x >= $8 - 0.5 && x <= $10 + 0.5 && z >= $9 - 0.5 && z <= $11 + 0.5 }
            printf "%s %s %s -> %s: Link at (%s, %s, %s): %s%s\n", $3, $2, $4 "/" $6, $7, x, y, z, (ok ? "PASS" : "FAIL"),
                   ($12 >= 2048 ? " (gate flag " $12 ": not settable by the test)" : "") }' "$CASES" |
        tee -a "$OUT"
    grep -q '\[crash\]' "build/run_shot_$job.log" && echo "$lv: game stopped (crash) - see build/run_shot_$job.log" | tee -a "$OUT"
done
rm -f "$CASES"
echo "$(grep -c PASS "$OUT") pass / $(grep -c -v PASS "$OUT") other -> $OUT"
