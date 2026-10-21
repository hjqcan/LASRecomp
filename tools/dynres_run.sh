#!/bin/bash
# Dynamic resolution measurement: one muted run through outdoor areas with Dynamic on, walking back and forth in each
# (9 warps, 30 s apart, ~5.5 min), then the summary (tools/dynres_log.py): render-scale changes, hitches, fps.
#   tools/dynres_run.sh <binary> <tag> [window WxH] [extra settings lines...]   (log: build/dyn_<tag>/run.log)
# e.g. tools/dynres_run.sh build/lasrecomp after 7680x4320 "antialias = smaa". LARECOMP_HIDDEN=1 for a window larger
# than the screen (the drawable keeps its size). SAVE_FROM: the save_0 folder to start from (a scratch copy is played).
# Timing-sensitive: nothing else should use the GPU meanwhile.
cd "$(dirname "$0")/.."
BIN=$1 TAG=$2 WIN=${3:-1920x1080}; shift 3 2>/dev/null
O=build/dyn_$TAG; rm -rf "$O"; mkdir -p "$O/save/save_0"
SAVE_FROM=${SAVE_FROM:-save_backup_20261007/save_0}
cp "$SAVE_FROM"/savedata0.bin "$SAVE_FROM"/screenshot0.bin "$O/save/save_0/"
{ printf 'aspect = unlocked\nfullscreen = 0\nshow_fps = 0\nresolution = dynamic\naspect_asked = 1\nmenu_tip_seen = 1\n'
  for l in "$@"; do echo "$l"; done; } > "$O/settings.ini"
ROOMS=(Field_11C Field_03E Field_07M Field_01O Field_06C Field_11C Field_07M Field_03E Field_06C)
START=2400 STEP=1800  # (keep in step with tools/dynres_log.py)
IN=$(for f in $(seq 600 100 2000); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 50)); done)
AT=
for i in "${!ROOMS[@]}"; do
    f=$((START + i * STEP)); [ "$i" -gt 0 ] && AT+="$f:Field:${ROOMS[$i]},"
    for k in 0 1 2 3 4 5; do
        b=$((f + 200 + k * 260)); IN+="$b:STICK_LEFT:80,$((b + 90)):STICK_RIGHT:80,$((b + 180)):STICK_UP:35,$((b + 220)):STICK_DOWN:35,"
    done
done
export LARECOMP_SETTINGS=$O/settings.ini LARECOMP_SAVE_DIR=$O/save LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 \
    LARECOMP_NO_LAUNCHER=1 LARECOMP_INPUT="${IN%,}" LARECOMP_WARP_ON_LOAD="Field:${ROOMS[0]}" LARECOMP_WARP_AT="${AT%,}" \
    LARECOMP_WINDOW=$WIN LARECOMP_LOG_HITCH_MS=40 LARECOMP_FPS_INTERVAL=1 LARECOMP_LOG_DYNRES=1 \
    LARECOMP_WINDOW_SHOT=$((START + ${#ROOMS[@]} * STEP)) LARECOMP_EXIT_AFTER_SHOT=1
[ "$(uname)" = Darwin ] && . tools/game_slot.sh  # (waits for 10 GB free)
"$BIN" extracted > "$O/run.log" 2>&1 &
PID=$!
for i in $(seq 1 900); do sleep 1; kill -0 $PID 2>/dev/null || break; done
kill -9 $PID 2>/dev/null && echo "(still running after 15 min, killed)"
python3 tools/dynres_log.py "$O/run.log"
