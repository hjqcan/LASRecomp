# Multi

This is the independently maintained edition in [hjqcan/LASRecomp](https://github.com/hjqcan/LASRecomp/tree/multi). Download Windows builds from [the fork releases](https://github.com/hjqcan/LASRecomp/releases). Multi builds use their own update channel.

**Multi Beta 5.3 now supports Animal Crossing: New Horizons 1.0.0 on Windows.**
**Multi Beta 5.3 现已支持《集合啦！动物森友会》1.0.0（Windows 测试版）。**
Current source also enables ACNH 3.0.1 installation and optional experimental
60 FPS + SpeedFix. Standard 30 FPS remains the default. Other updates and DLC
remain unsupported; see [ACNH setup and limits](ACNH-port.md).

## Names and compatibility / 名称与兼容性

`multi` is the maintained branch in the existing `hjqcan/LASRecomp` fork. The
repository and its default `main` branch have not been renamed. The public name
is **Multi**; upstream authorship and license notices remain intact.

Existing release tags use `enhanced-beta<N>.<revision>` because already installed
clients recognize that update channel. These tags and release URLs stay valid;
release titles and the application display **Multi Beta**. Keep this tag format
for compatible updates until a separately tested update-channel migration is
available. Renaming a display label must not silently move users to a channel
that their installed updater cannot see.

`lasrecomp.exe`, its import library, game libraries, environment variables,
configuration filenames, log names and data directories retain their existing
names for compatibility. In particular, Windows game libraries import symbols
from `lasrecomp.exe`. Renaming that binary requires a separate library migration.
Preserve existing installations, saves, seeds, settings and desktop shortcuts.

分支名改为 `multi`，对外显示 Multi；仓库和默认分支保持原样。历史版本标签、更新通道、
程序内部文件名及数据路径继续兼容，不需要因此移动存档或重新导入游戏。

## Branch policy / 分支维护规则

`main` and `beta_5` mirror upstream. `multi` merges upstream `beta_5` fixes and retains the fork's features. Features accepted upstream can use the upstream implementation after checking that no fork behavior is lost. Features upstream does not accept remain maintained here. Existing upstream PR branches are independent and are never rewritten as part of multi synchronization.

`main`、`beta_5` 只同步官方同名分支；`multi` 持续合入官方 `beta_5` 更新，并保留我们自己的全部增强功能。官方不接受的功能继续在这里维护；官方接受的功能以最新官方实现为基础，核对兼容性后整合。同步和发布不会改写已提交 PR 的分支。

## Included enhancements

| Feature | Scope |
| --- | --- |
| Game setup | Extracted folders, NSP, NSZ, XCI and XCZ base archives, optional supported update archives, local keys, read-only selection inspection |
| Versions | Link's Awakening 1.0.0/1.1.0 and Animal Crossing: New Horizons 1.0.0/3.0.1 (Windows beta), exact Build-ID checks, separate saves, libraries and compilation caches |
| ACNH frame rate | Standard 30 FPS by default; optional experimental 60 FPS + SpeedFix for 3.0.1, sharing that version's save slot |
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

See [shared game installations](multi-game.md), [ACNH support](ACNH-port.md),
[Z-A development status](pokemon-za-port.md), [local co-op](local-coop.md) and
[experimental capture tools](research-capture.md) for their current scope.

## Windows quick start / Windows 使用说明

When upgrading, preserve the existing game installations, saves, seeds and
settings. The launcher rebuilds a game library when its runtime or generator
identity has changed; completed compilation can be cancelled and resumed.

升级时保留现有游戏目录、存档、种子和设置。运行时或生成器标识变化时，启动器会重新编译游戏库，编译可取消后续编。

1. Extract the entire ZIP into a writable folder. Run `lasrecomp.exe`. Windows 10+ x64, an AVX2 CPU and a Vulkan 1.3 graphics driver are required. No separate Python or compiler installation is needed.
2. Open **Game Files** and select your extracted files or your own base archive. The launcher identifies the supported game and version from its executable modules. Add the matching update archive for Link's Awakening 1.1.0 or ACNH 3.0.1; encrypted inputs need your local keys.
3. Inspect the selection, import, then build. Allow at least 20 GiB of free space. The first compilation takes several minutes; cancelling preserves completed work.
4. Start the selected game, or use its generated desktop shortcut next time. Keep saves with their original game/version. Link's Awakening also supports version-specific randomizer seeds and its existing fork features.

完整解压 ZIP 到可写目录，双击 `lasrecomp.exe`。在“游戏文件”中提供自己的已提取文件或 NSP/NSZ/XCI/XCZ 本体包，启动器自动识别支持的游戏和版本。《织梦岛》1.1.0 和《动物森友会》3.0.1 需另选对应更新包。加密内容需要自己的本地密钥。检查选择后导入并编译，首次编译可取消后续编。请预留至少 20 GiB 空间。完成后可通过对应桌面图标直接启动；每个游戏、版本和随机种子使用独立存档。动森 3.0.1 的两种帧率模式共用该版本的存档；低于 60 FPS 时修正模式会变慢，可切回标准模式。

v1.1.0 Randomizer remains experimental: generation/startup checks do not establish full playthrough or parity of every option. Native macOS/Linux gameplay and Metal rendering are not certified by this Windows prerelease. Report fork-specific problems in [fork issues](https://github.com/hjqcan/LASRecomp/issues), including version, seed/settings and logs.

v1.1.0 随机化仍为实验功能，尚未验证完整通关和全部选项组合。本次仅发布并验证 Windows 版本；macOS/Linux 的实际游戏运行及 Metal 渲染仍需硬件验证。

## Repeatable release checks

1. Merge upstream `beta_5` into `multi` and run the relevant CI checks. Preserve the fork features and independent PR branches.
2. Build the runtime and both helpers from the same revision; apply `pe_no_cfg.py` to the Windows generator. Use `tools/package_multi_windows.py` to package only allowlisted files.
3. Before publishing, check setup, cancel/resume, gameplay, audio, save/reopen and the changed features with compatible copied saves. Keep unsupported ports and experimental randomizer options clearly marked.
4. Inspect the package contents and checksum, then publish it as a fork prerelease. Do not add per-run reports or machine-specific evidence to the repository.

The source tree contains no game assets, console keys, generated game code or personal saves. The package includes only distributable program/toolchain files and open fonts.
