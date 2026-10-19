# Challenge workshop, route replay and frame timing

These are local practice and research tools on the `multi` branch. They do not provide competitive verification, deterministic game replays, higher-rate simulation or online co-op.

## Create and run a challenge

1. Start a fresh Multi process and select the game installation and Vanilla/randomizer seed you want to practice. The selected save should belong to that version and seed.
2. Open **Settings → Workshop → Challenge rules**. Choose a lowercase filename, game version, destination level, time limit (10–3,600 seconds), damage multiplier (1–8), optional one-hit KO and optional failure on health loss.
3. **Save challenge** writes a `.laschallenge` preset under `<data root>/workshop/presets/`. Existing files are preserved; use another name for a revision. **Load challenge** accepts a shared preset. Names use lowercase ASCII letters, digits, `-` or `_`, begin with a letter and have at most 63 characters.
4. **Arm isolated attempt** copies the selected save mounts to a new `<data root>/workshop/runs/run-…/save/` folder. The source saves are read only. The current game/seed selection is pinned; changing it cancels preparation. Return to the launcher and press **Play**.
5. The timer starts once a living player and a gameplay level are observed. Reach the destination before the deadline. A save already in that level completes immediately. The timer includes dialogue, loading, in-game menus, the settings overlay and photo mode. Death, the deadline or health loss under the no-hit rule ends the attempt.
6. Use **Finish and return to launcher** when done. Even after success/failure, game saves continue to go to the isolated folder until the game is torn down. Restart the application to arm another attempt. A normal window close saves a cancelled route; a crash or forced termination may lose the unfinished route.

The damage rule is a temporary runtime override and does not modify the saved Gameplay settings. Other enabled gameplay options and resource mods still apply. Cheat toggles, warp/item tools and opening the live save editor mark an active attempt as assisted. This flag is advisory: external tools, every setting change, file contents and game progression are not certified. No-hit detection observes health decreases; it is not a hook on every possible damage event.

Snapshot creation accepts ordinary `save_0`…`save_9` mounts, up to 512 files and 64 MiB total, with fewer than eight nested levels. Backup archives are excluded. Linked/reparse paths are refused. The run contains its own automatic save backups. Keep the run's save data private when sharing a preset or route.

## Preset format

UTF-8/ASCII, at most 4 KiB; all fields are required and unknown/duplicate fields are rejected:

```ini
format=1
name=leave-house
game_version=1.0.0
goal_level=Field
seconds=300
damage=1
no_hit=0
ohko=0
```

`game_version` is `1.0.0` or `1.1.0`. The editor offers known level IDs. Imported IDs must contain only letters, digits and `_`; an unknown destination simply cannot be reached. `no_hit` and `ohko` are `0`/`1`. This format defines a destination challenge, not new maps, enemies or scripts.

## Route viewer

Each completed or cancelled attempt writes `route.csv` and `result.ini` beside its preset and isolated save. Route capture observes controller state, health, level and collision-component position, typically around 10 samples per second; transient missing positions are flagged. It is bounded to 40,000 samples. The game state is never restored or driven by these samples.

Open **Workshop → Open route CSV**, or **View latest route**. Play/pause the timeline or drag the time slider. The viewer draws X/Z positions for the selected sample's level, shows health and stick/button samples, and avoids connecting different levels or missing positions. The gold line is the observed route up to that time; muted lines show later samples. The time slider ends at the last observation, which may precede the result timestamp slightly.

Routes can be opened from the launcher. The reader validates the CSV schema, numeric bounds, time order and a 12 MiB limit. A failed import preserves the current viewer. Share `route.csv` and the preset separately from `save/`; these files do not contain game resources. They do expose the recorded game route and inputs.

`result.ini` status values: `3` success, `4` failure, `5` cancelled. Failure reasons: `1` time limit, `2` depleted health, `3` observed health loss. An armed but unstarted snapshot initially has `status=armed`.

## Performance capture

During gameplay, open **Settings → Performance**, choose 10–300 seconds and press **Start benchmark**. Recording excludes the settings overlay, photo mode and periods without a gameplay player. In-game dialogue/inventory can still produce measured frames. The duration counts captured intervals. **Stop and export** finishes early; returning to the launcher also finishes the current capture.

`benchmarks/capture-…/frames.csv` contains presentation frame IDs and intervals in milliseconds. `summary.ini` records game version, seed, average FPS, median/p95/p99/max intervals and counts over 16.667/33.333 ms. Average FPS is `1000 / mean interval`; 1% low FPS is `1000 / mean of the slowest ceil(N/100) intervals`. Percentiles use nearest rank. Capture stops at 60,000 intervals even if the requested duration has not elapsed.

These are presentation intervals, including waits and CPU work between presentations. They are not GPU timers or proof that the simulation runs at the reported rate. Compare captures under the same hardware, settings, save, seed, mods and route, with other workloads stopped. This does not change the game's fixed timing or enable 120 FPS gameplay.

## Development

`python tools/test_workshop.py` covers challenge state, preset parsing, copied
saves, route data and frame statistics without game assets. Experimental capture
and replay tools are described in [research-capture.md](research-capture.md).
