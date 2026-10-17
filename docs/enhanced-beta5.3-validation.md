# Enhanced Beta 5.3 validation

## Supported releases

Enhanced Beta 5.3 enables **Animal Crossing: New Horizons 1.0.0** for normal
Windows installation and play. Link's Awakening 1.0.0 and 1.1.0 remain supported.

增强版 Beta 5.3 现已支持《集合啦！动物森友会》1.0.0（Windows 测试版），
并保留《塞尔达传说：织梦岛》1.0.0／1.1.0 支持。动森不再需要开发开关即可安装。

The launcher identifies supported games from their executable modules, imports
the selected files, compiles a native game library and creates a separate
Windows desktop shortcut. Games and versions retain separate libraries, caches
and saves. Game files and any needed local keys are supplied by the player.

## Acceptance evidence, 2026-10-18

Tests ran on Windows 11 with an Intel Arc Pro 140T Vulkan GPU. Gameplay used
independent save copies, normal shader preparation and strict HLE/IPC checks.
The user's original saves remained unchanged. These observations qualify the
tested scenes and machine, not every hardware configuration or a full playthrough.

| Check | Observed result |
| --- | --- |
| Real imports and compilation | The packaged frozen generator and bundled Zig compiler completed ACNH's 729 objects and both LAS versions' 646 objects each. Module identities, compilation receipts, published installations and actual desktop shortcuts were verified. |
| Archive formats | Complete real-game imports used locally supplied NSZ base/update archives. NSP, NSZ, XCI and XCZ parsing, encrypted sections, counter boundaries, automatic identity selection and rejection of unsupported/mixed modules have synthetic regression coverage. No real NSP/XCI/XCZ full-game run is claimed. |
| ACNH extended play | A 48.2-minute session covered outdoor travel, the 05:00 day rollover, Nook's broadcast, rain, mail, the player's tent, resident services, NPC dialogue and the airport. It completed 15 multi-commits including Save and End, exited normally and passed all 24 internal save checksums. A later desktop launch read the saved progress. |
| ACNH presentation timing | The extended run exposed an ignored NVN presentation interval. The runtime now honors the game's requested interval of 2 against its 60 Hz time base. A separate 9.5-minute corrected run exercised indoor/outdoor movement, inventory, tent transitions and saving at about 29.7-29.8 FPS. Four commits and all 24 save checksums passed. |
| Final package and reopen | A further 4.3-minute run loaded the progress saved with corrected timing, moved around the rainy island and completed Save and End. It exited normally after three multi-commits; all 24 checksums passed. Median steady presentation was 29.8 FPS. |
| LAS regression | Both versions launched through their actual desktop shortcuts, loaded saves, entered Marin and Tarin's house and returned to Mabe Village at about 60 FPS. LAS 1.0.0 committed its autosave and screenshot. LAS 1.1.0 completed a manual save with data and screenshot commits; its initial short scene run did not trigger an autosave. All processes exited normally. |
| Default installation gate | Source and frozen-helper tests import supported ACNH data without a development override. The final GUI identified a real NSZ as ACNH 1.0.0 from verified executable modules and enabled normal setup. Other development adapters remain gated. |
| Timing regression | A controlled-clock harness exercises the production NVN presentation entry points, including rendering cost, late wakes, stalls, changing intervals, separate windows, clock rollback and the LAS path. The former implementation fails the regression. |
| Save integrity and preservation | Test saves passed all 24 ACNH internal checksums. The original player save tree matched its pre-test SHA-256 manifest before normal interactive play was restored. |

Detailed graphics, audio, save and Vulkan validation evidence is retained in
[the ACNH port record](ACNH-port.md). Earlier checks for the existing photo,
mod, backup, workshop, local co-op and randomizer features remain in
[the Beta 5.2 record](enhanced-beta5.2-validation.md); they are historical
evidence, not a claim that every earlier scenario was repeated for this release.

The gameplay acceptance source is `1cc17a8b94939e9dad1337f1cb270c4791358720`.
Beta 5.3 release preparation changes its version labels and documentation,
without changing the accepted gameplay implementation or native-library ABI.
Both distributable helpers are rebuilt from the release source. Packaging uses
explicit allowlisted inputs, checks the actual LGPL FFmpeg binary and records
every distributed file's SHA-256. The ZIP and checksum are published together.
The rebuilt frozen game helper passed all adapter import and ARM64 generation
checks. The rebuilt randomizer helper generated a fresh default seed for each
LAS version with matching sampled logic/spheres output; these two generation
checks do not repeat the earlier 24-case matrix or certify full-game parity.

## Reproduction and limits

Follow the [release checklist](enhanced.md#repeatable-release-checks) and
[multi-game setup](multi-game.md). Use independent saves, inspect full module
Build IDs, complete the native build and launch the generated shortcut. Verify
scene changes, saving and reopening; a catalog entry or title screen alone does
not qualify a game. The frozen adapter, save multi-commit and NVN interval tests
are asset-free entry points under `tools/`.

ACNH support is limited to the 1.0.0 base release. Updates and DLC are unsupported;
online play and amiibo are unavailable. Full-game progression and additional
hardware, macOS and Linux gameplay remain unverified. First-use shader waits can
still occur. LAS 1.1.0 randomizer and local Link + BowWow co-op remain experimental.

Upgrade by preserving existing game data, saves, seeds and settings. The launcher
rebuilds native libraries when their runtime/generator identity changes. No game
assets, generated guest code/libraries, keys, saves, seeds, test screenshots or
personal settings are included in the release files.
