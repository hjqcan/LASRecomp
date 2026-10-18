# LAS Recomp Enhanced

This is the independently maintained edition in [hjqcan/LASRecomp](https://github.com/hjqcan/LASRecomp/tree/enhanced). Download Windows builds from [the fork releases](https://github.com/hjqcan/LASRecomp/releases). Enhanced builds use their own update channel.

**Enhanced Beta 5.3 now supports Animal Crossing: New Horizons 1.0.0 on Windows.**
**增强版 Beta 5.3 现已支持《集合啦！动物森友会》1.0.0（Windows 测试版）。**
Normal installation is enabled for this base release; updates and DLC remain unsupported.

## Branch policy / 分支维护规则

`main` and `beta_5` mirror upstream. `enhanced` merges upstream `beta_5` fixes and retains the fork's features. Features accepted upstream can use the upstream implementation after checking that no enhanced behavior is lost. Features upstream does not accept remain maintained here. Existing upstream PR branches are independent and are never rewritten as part of enhanced synchronization.

`main`、`beta_5` 只同步官方同名分支；`enhanced` 持续合入官方 `beta_5` 更新，并保留我们自己的全部增强功能。官方不接受的功能继续在这里维护；官方接受的功能以最新官方实现为基础，核对兼容性后整合。同步和发布不会改写已提交 PR 的分支。

## Included enhancements

| Feature | Scope |
| --- | --- |
| Game setup | Extracted folders, NSP, NSZ, XCI and XCZ base archives, optional supported update archives, local keys, read-only selection inspection |
| Versions | Link's Awakening 1.0.0/1.1.0 and Animal Crossing: New Horizons 1.0.0 (Windows beta), exact Build-ID checks, separate saves, libraries and compilation caches |
| UI | English, Simplified Chinese and Traditional Chinese settings; upstream language/font fixes; CRLF settings and HK/MO/Hant locale handling |
| Compilation | Cancel/resume, SHA-256 source/object receipts, split compiler/generator identities, safe library publication, Windows per-helper job objects |
| Saves | Atomic file commits including extended flags, automatic/manual snapshots after commit, 20-snapshot retention, validated launcher restore, separation by installation and randomizer seed |
| Photos | F2 freeze, aspect-ratio crops, composition guides, color presets/contrast/vignette, F12 PNG plus settings recipe, Vulkan and Metal implementations |
| Resource mods | Local pack import, metadata, version-specific enable lists, shareable version-checked presets, conflict paths, save backup before selection changes |
| Randomizer | Existing upstream options plus experimental version-specific v1.1.0 generation and validated patches/event adaptation |
| Challenge workshop | Shareable destination/time/damage rules, isolated save snapshots, result and observed-route recording |
| Local co-op prototype | Vanilla 1.0.0 Link + player-controlled BowWow, separate P2 input, native movement/combat and isolated saves |
| Route viewer | Local CSV loading, playback/scrubbing, per-level X/Z map, health and controller samples |
| Performance | Bounded gameplay frame-interval captures, CSV export, percentile and 1% low statistics |
| Research tools | Isolated HID/update input capture, observed start gates, RNG/NPC/clock/callback observations, optional startup-seed control, save-byte/I/O comparison, scene-transition scripts and explicit divergence reports; disabled in ordinary play |
| Addresses | Canonical v1.0.0 address table, checked updated-executable relocation, instruction guards |

See [import and versions](NSZ-and-v110.md), [photo/mod/randomizer details](photo-mods-randomizer.md), [mod format](resource-mods.md) and [workshop/replay/performance](workshop.md).

Enhanced Beta 5.2 includes the workshop, extended photo controls, mod presets and frame captures. Their scope and checks are recorded in [third/fourth-round validation](enhanced-stage34-validation.md). The [local companion co-op prototype](local-coop.md) adds same-PC Link + BowWow play in isolated 1.0.0 sessions. High-refresh simulation, deterministic game replay and online co-op remain research work.

The subsequent [research probes](research-capture.md) now measure final HID input, clock behavior and scene camera matrices in isolated saves. See the [2026-10-17 findings](research-findings-2026-10-17.md) for actual dual-version replay divergence and presentation-mode controls.

[Update-dispatch experiments](research-update-boundary.md) add a guarded update boundary and repeatable capture counts while retaining explicit state-comparison limits.

[Semantic observations](research-semantic-observations.md) expose RNG and placed-actor differences that matching player positions can miss, and extend the input scenario across scene transitions.

The [fourth research round](research-fourth-round.md) separates startup seed input from RNG consumption and compares exact save-window bytes and observed loading activity in both supported versions.

[Store tracing](research-store-tracing.md) builds an optional diagnostic library to locate differences in observed RNG writes, with explicit source coverage, buffer limits and state-chain validation.

The [Enhanced Beta 5.3 validation record](enhanced-beta5.3-validation.md) covers
ACNH acceptance and the latest two-version LAS regression. The
[Beta 5.2 record](enhanced-beta5.2-validation.md) and
[Beta 5.1 record](enhanced-beta5.1-validation.md) retain the earlier enhanced-feature checks.

[Multi-game development](multi-game.md) now includes the shared module runtime,
reviewed per-game adapter definitions, catalog-generated installation selection,
release-scoped cache identities and Windows desktop shortcuts. Managed setup
imports and builds a candidate before atomically selecting it; failed builds keep
the previous installation. ABI 4 requires a one-time game-library rebuild while
preserving saves and launch IDs. ACNH 1.0.0 is enabled as a Windows beta after
local island, day-rollover, save/reopen and extended-play checks; see
[its validation record and limits](ACNH-port.md). Other development adapters
remain unavailable for normal installation. The shared runtime
also includes audio effects, guest nonlocal-return handling, compute
workgroup memory and screen-attachment scaling fixes. Vulkan prepares shaders
ahead of use and shares identical stages while preserving every required draw.
Unseen variants can still require first-use waits.

## Windows quick start / Windows 使用说明

When upgrading, preserve the existing game installations, saves, seeds and
settings. The launcher rebuilds a game library when its runtime or generator
identity has changed; completed compilation can be cancelled and resumed.

升级时保留现有游戏目录、存档、种子和设置。运行时或生成器标识变化时，启动器会重新编译游戏库，编译可取消后续编。

1. Extract the entire ZIP into a writable folder. Run `lasrecomp.exe`. Windows 10+ x64, an AVX2 CPU and a Vulkan 1.3 graphics driver are required. No separate Python or compiler installation is needed.
2. Open **Game Files** and select your extracted files or your own base archive. The launcher identifies the supported game and version from its executable modules. Add the supported update archive for Link's Awakening 1.1.0; encrypted inputs need your local keys. ACNH supports the 1.0.0 base release only.
3. Inspect the selection, import, then build. Allow at least 20 GiB of free space. The first compilation takes several minutes; cancelling preserves completed work.
4. Start the selected game, or use its generated desktop shortcut next time. Keep saves with their original game/version. Link's Awakening also supports version-specific randomizer seeds and its existing enhanced features.

完整解压 ZIP 到可写目录，双击 `lasrecomp.exe`。在“游戏文件”中提供自己的已提取文件或 NSP/NSZ/XCI/XCZ 本体包，启动器自动识别支持的游戏和版本。《织梦岛》1.1.0 另选支持的更新包；《动物森友会》目前仅支持 1.0.0 本体。加密内容需要自己的本地密钥。检查选择后导入并编译，首次编译可取消后续编。请预留至少 20 GiB 空间。完成后可通过对应桌面图标直接启动；每个游戏、版本和随机种子使用独立存档。

v1.1.0 Randomizer remains experimental: generation/startup checks do not establish full playthrough or parity of every option. Native macOS/Linux gameplay and Metal rendering are not certified by this Windows prerelease. Report fork-specific problems in [fork issues](https://github.com/hjqcan/LASRecomp/issues), including version, seed/settings and logs.

v1.1.0 随机化仍为实验功能，尚未验证完整通关和全部选项组合。本次仅发布并验证 Windows 版本；macOS/Linux 的实际游戏运行及 Metal 渲染仍需硬件验证。

## Repeatable release checks

1. Fetch upstream; fast-forward mirror branches; merge upstream `beta_5` into `enhanced`. Record the upstream commit and confirm all open PR branch heads remain unchanged.
2. Run the settings, import/NSP/XCI, build cache/resume/Windows process, save backup, mod, photo, address/hook, localization and randomizer profile/event tests. CI uses synthetic data only.
3. Rebuild both helpers from this branch, including the required `pe_no_cfg.py` step for Windows `lasgen`. Generate and compile both supported executables with all patch instruction guards enabled.
4. Use a fresh output directory and separate compatible saves for each version. Test actual NSZ base/update import, folder startup, default audio, an opening-room gameplay segment, photo freeze/export/resume and automatic backups. Inspect screenshots and level logs.
5. Exercise cancel/resume through the launcher with the packaged compiler. Check object hashes and only the build's child processes. Check launcher backup restore and mod import/conflict/disable with scratch saves/resources.
6. Generate fresh version-specific seeds with the frozen helper. Check runtime patch loading and gameplay. Track keysanity, shops, digs, souls, bosses/minibosses, entrance/dungeon shuffles separately; do not claim full parity from smoke coverage.
7. Build the package from explicit allowlisted files using `tools/package_enhanced_windows.py`. Inspect its file list; publish ZIP, SHA-256 checksum and precise validation/limitations as a prerelease on this fork.

The source tree contains no game assets, console keys, generated game code or personal saves. The package includes only distributable program/toolchain files and open fonts.
