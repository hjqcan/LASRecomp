# Local companion co-op prototype / 本地伙伴合作原型

This enhanced-only prototype lets two people share one 1.0.0 vanilla game: player 1 controls Link and player 2 controls BowWow. It is an asymmetric companion mode, with one camera, one story and one isolated host save. It is not two Links or network multiplayer.

## Start

1. Select **Vanilla 1.0.0** in the launcher and open **Workshop**.
2. Enable **Local companion co-op (1.0.0 prototype)**, then **Arm isolated attempt** and **Play**. Each attempt copies the selected saves into a new Workshop run; the source saves are not overwritten.
3. P1 uses the existing controls. P2 uses the **second gamepad**, left stick to move and its **south button** to attack, or **numpad 8 / 4 / 5 / 6** to move and **numpad 0** to attack. Numpad movement is reserved for P2 while enabled.
4. P1 controls conversations, menus and room transitions. P2 stops receiving movement commands while a menu or photo mode captures input. Losing window focus clears P2 input; disconnecting one pad does not assign the other player's pad to it.

启动器选择 **原版 1.0.0 → 挑战工坊 → 本地伙伴合作 → 准备隔离挑战 → 开始游戏**。P1 操作林克；P2 用第二只手柄的左摇杆和下方按键，或小键盘 8/4/5/6 与 0 操作汪汪。每次启动使用独立存档副本，进度保留在对应 `workshop/runs` 文件夹中，不自动写回主存档。

BowWow uses native pathfinding/collision and the game's target selection, attack wind-up, dash, hit and recovery states. The attack button requests a bite of an eligible nearby enemy; it is not a free sword swing. Moving too far away triggers a return toward Link. The existing companion restrictions still apply, including rooms that exclude companions. P2 does not have independent hearts, inventory, conversations or progression. Story events requiring another companion, dungeons, water, bosses and a complete playthrough have not been accepted for this prototype. Physical controller testing remains necessary; automated controller checks use SDL virtual gamepads.

## Implementation boundaries

- Disabled by default. Activation requires an isolated Workshop save, vanilla 1.0.0, and matching instructions at seven native entry points. Other versions and randomizer runs reject activation.
- Only the current player's BowWow idle/follow callbacks are overridden. Ownership, vtable and heap ranges are checked every callback, including after room changes and allocator address reuse.
- Movement goes through the native movement component. Attacks retain the native state machine; the host does not set enemy HP or write actor positions.
- P2 input is sampled independently of P1 HID. Native attack states finish normally; P2 has no menu authority. Reset hooks discard guest pointers and control state when returning to the launcher.
- The normal game-code library is used. No generated game code or game assets belong in source commits or release packages.

## Repeatable checks

Asset-free checks:

```sh
python tools/test_coop_gate.py
python tools/test_coop_input.py
```

The input check needs SDL3 development headers/libraries and `pkg-config`, or `SDL_FLAGS` (for example `-lSDL3` in UCRT64). It tests the real input router with two SDL virtual pads, keyboard separation, focus, disconnect and default-off behavior.

Native checks use the existing isolated research runner:

```sh
python tools/coop_run.py --case attack --exe /path/lasrecomp.exe \
  --game /path/extracted-1.0.0 --library /path/lasgame.dll \
  --save-copy /path/opening-house/save_0 --output /path/new-output
```

Cases: `move`, `keyboard`, `attack`, `idle-enemy`, `transition`, `off`. Every output must be a new directory. Each exports `report.json`, `coop-report.json`, a runtime log and screenshot, and checks the original save's hashes. `attack` and `idle-enemy` create one native Octorok only in that test run: the former requires a native attack and death observation; the latter requires continued survival without attacks. The fixture retains its factory reference until death, so observing it after removal from the actor registry is valid. `transition` drives the opening house / Field route and checks reacquisition of the companion; actor addresses may be reused between rooms.

For a manual isolated CLI run, set `LARECOMP_COOP=1` together with a valid `LARECOMP_CHALLENGE_PRESET`. Optional `LARECOMP_COOP_TRACE=1` logs positions. `LARECOMP_COOP_INPUT=start:RIGHT|LEFT|UP|DOWN|ATTACK|WAIT:length,...` is a bounded test script indexed by controllable idle/follow updates, not render frames. `LARECOMP_COOP_TEST_ENEMY=1` is the explicit native enemy fixture and must not be used for normal play.

## Acceptance record (2026-10-17)

The Windows 1.0.0 prototype entered native attack state 2, defeated the test Octorok, and left that enemy alive in the no-attack control. Both source-save hashes remained unchanged. A 4,400-update run observed `MarinTarinHouse → Field → MarinTarinHouse`, with 973 Field position samples, P1 movement in all four Field windows and three companion ownership epochs. The allocator reused the same actor address; the controller successfully revalidated ownership. Maximum sampled partner distance in that route was 5.021 world units. Asset-free guard/reset and SDL two-pad input checks passed.

These are scoped prototype checks. They do not establish full-game compatibility, online synchronization or deterministic replay.
