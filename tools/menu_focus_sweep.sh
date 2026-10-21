#!/bin/bash
# The settings menu's controller cursor on every tab: tools/menu_focus_sweep.sh. One hidden, muted run on a scratch
# save, in game. The menu opens with Back + Start.
#  1. Every tab: Down through every row and past the bottom, then Up through every row and past the top onto the tab
#     bar (STEPS presses each way, default 75).
#  2. From every tab to every other tab, both ways (R / L, wrapping round) and every distance: a switch with the
#     shoulder buttons - half the cases after holding Down on the tab it leaves (the cursor deep in a scrolled page),
#     half from the tab bar - then Down once and Up once.
#  3. The menu closes and opens again (Home), then Down once.
# The game checks every fresh Down / Up press (LARECOMP_MENU_FOCUS_LOG, runtime/ui/menu.mm focus_check) against the
# page's rows in drawing order: the first Down after a switch or the menu opening lands on the first enabled row, Down
# on the next row (the last one stays), Up on the row before, Up from the top row on the selected tab.
# Result: the checks and an OK / FAIL count; the log is build/run_shot_focus_sweep.log. A heavily loaded Mac (a few fps)
# can show a held key's repeat as a step too many.
cd "$(dirname "$0")/.."
N=10  # tabs in game: Controls, Video, Audio, Gameplay, Cosmetics, Randomizer, Cheats, Warp, Save Editor, Quit
read -r END SPEC < <(python3 - "$N" "${STEPS:-75}" <<'EOF'
import sys
n, steps = int(sys.argv[1]), int(sys.argv[2])
ev, f, cur = [], 2600, 0
def press(btn, frames=2, gap=12):
    global f
    ev.append(f"{f}:sdlpad:{btn}:{frames}")
    f += frames + gap
ev.append(f"{f}:sdlpad:back:30"); ev.append(f"{f + 5}:sdlpad:start:10"); f += 80  # open the menu (Controls)
press("dpad_down", gap=20); press("dpad_up", gap=20)  # (first Down after opening; Up back to the tab bar)
# every tab: Down through every row past the bottom, then Up through every row past the top (onto the tab)
for t in range(n):
    if t: press("rshoulder", gap=12)
    for _ in range(steps): press("dpad_down", gap=10)
    for _ in range(steps): press("dpad_up", gap=10)
cur = n - 1
case = 0
for d in (1, -1):
    for k in range(1, n):
        for s in range(n):
            # to tab s (the shorter way round)
            r = (s - cur) % n
            for _ in range(r if r <= n // 2 else n - r):
                press("rshoulder" if r <= n // 2 else "lshoulder", gap=10)
            if case % 2 == 0:  # deep in the page it leaves
                press("dpad_down", gap=10)
                ev.append(f"{f}:sdlpad:dpad_down:100"); f += 115
            for _ in range(k):
                press("rshoulder" if d > 0 else "lshoulder", gap=10)
            press("dpad_down", gap=20)
            press("dpad_up", gap=20)
            cur = (s + d * k) % n
            case += 1
ev.append(f"{f}:sdlpad:guide:2"); f += 60  # close (Home)
ev.append(f"{f}:sdlpad:guide:2"); f += 60  # open again
press("dpad_down", gap=30)
print(f + 30, ",".join(ev))
EOF
)
tools/fresh_save.sh focus_sweep >/dev/null
LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_SAVE_DIR=build/focus_sweep LARECOMP_WINDOW=960x540 \
LARECOMP_MENU_FOCUS_LOG=1 SHOT_ID=focus_sweep \
LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6" \
LARECOMP_HOST_INPUT="$SPEC" tools/shot.sh "$END" $((120 + END / 30)) </dev/null
L=build/run_shot_focus_sweep.log
grep "\[menu\] focus check" "$L"
echo "$(grep -c '\[menu\] focus check.* OK$' "$L") OK / $(grep -c '\[menu\] focus check.* FAIL$' "$L") FAIL /" \
     "$(grep -c '\[menu\] focus check.*SKIP' "$L") without option rows (checked by eye)"
