# Multi

This is the independently maintained edition in [hjqcan/LASRecomp](https://github.com/hjqcan/LASRecomp/tree/multi). Download Windows builds from [the fork releases](https://github.com/hjqcan/LASRecomp/releases). Multi builds use their own update channel.

**Multi Beta 5.4 adds Animal Crossing: New Horizons 3.0.1 and optional 60 FPS + SpeedFix on Windows.**
**Multi Beta 5.4 新增《集合啦！动物森友会》3.0.1 与可选的 60 FPS＋速度修正（Windows 测试版）。**
The 60 FPS mode remains experimental. Standard 30 FPS remains the default. Other updates and DLC
remain unsupported; see [ACNH setup and limits](ACNH-port.md).

## Names and compatibility / 名称与兼容性

`multi` is the maintained branch in the existing `hjqcan/LASRecomp` fork. The
repository and its default `main` branch have not been renamed. The public name
is **Multi**; upstream authorship and license notices remain intact.

New releases use `multi-beta<N>.<revision>`, starting with `multi-beta5.4`.
The updater understands the new tags and the numeric ordering of historical
`enhanced-beta*` tags. Existing release tags and URLs stay valid. Older installed
clients only recognize the historical channel, so download this first Multi
package manually from the releases page. Its updater recognizes future Multi
releases. Extract the new program files into the existing application folder
after closing the old application, or copy the existing data folders into the
new folder; do not replace saves or settings with empty folders.

`lasrecomp.exe`, its import library, game libraries, environment variables,
configuration filenames, log names and data directories retain their existing
names for compatibility. In particular, Windows game libraries import symbols
from `lasrecomp.exe`. Renaming that binary requires a separate library migration.
Preserve existing installations, saves, seeds, settings and desktop shortcuts.

分支名为 `multi`，新版本使用 `multi-beta*` 标签。旧版更新器无法发现新标签，首次需从
Release 页面手动下载 Multi Beta 5.4；之后可正常检查 Multi 更新。历史发布链接保持有效。
关闭旧程序后更新程序文件，保留游戏目录、存档、设置和快捷方式，无需重新导入游戏。

## Branch policy / 分支维护规则

`main` and `beta_5` mirror upstream. `multi` merges upstream `beta_5` fixes and retains the fork's features. Features accepted upstream can use the upstream implementation after checking that no fork behavior is lost. Features upstream does not accept remain maintained here. Existing upstream PR branches are independent and are never rewritten as part of multi synchronization.

`main`、`beta_5` 只同步官方同名分支；`multi` 持续合入官方 `beta_5` 更新，并保留我们自己的全部增强功能。官方不接受的功能继续在这里维护；官方接受的功能以最新官方实现为基础，核对兼容性后整合。同步和发布不会改写已提交 PR 的分支。

## Included enhancements

| Feature | Scope |
| --- | --- |
| Game setup | Already-extracted folders, executable identity checks, isolated installation candidates and local compilation |
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

See [game-file setup and versions](game-files-and-versions.md), [photo/mod/randomizer details](photo-mods-randomizer.md), [mod format](resource-mods.md) and [workshop/replay/performance](workshop.md).

See [shared game installations](multi-game.md), [ACNH support](ACNH-port.md),
[Z-A development status](pokemon-za-port.md), [local co-op](local-coop.md) and
[experimental capture tools](research-capture.md) for their current scope.

## Windows quick start / Windows 使用说明

When upgrading, preserve the existing game installations, saves, seeds and
settings. The launcher rebuilds a game library when its runtime or generator
identity has changed; completed compilation can be cancelled and resumed.

升级时保留现有游戏目录、存档、种子和设置。运行时或生成器标识变化时，启动器会重新编译游戏库，编译可取消后续编。

1. Extract the entire ZIP into a writable folder. Run `lasrecomp.exe`. Windows 10+ x64, an AVX2 CPU and a Vulkan 1.3 graphics driver are required. No separate Python or compiler installation is needed.
2. Open **Game Files** and select a complete, already-extracted folder containing `exefs` and `romfs`. The launcher identifies the supported game and version from its executable modules. Provide complete extracted files for the desired version; game archives and keys are not accepted.
3. Choose **Set up game files** to copy and build. Allow space for the complete game folder and its build cache. The first compilation takes several minutes; cancelling preserves completed work.
4. Start the selected game, or use its generated desktop shortcut next time. Keep saves with their original game/version. Link's Awakening also supports version-specific randomizer seeds and its existing fork features.

完整解压 ZIP 到可写目录，双击 `lasrecomp.exe`。在“游戏文件”中选择包含 `exefs` 和 `romfs` 的完整已解包目录，启动器自动识别支持的游戏和版本。更新版本也须提供该版本的完整已解包文件；不接受游戏归档包、密钥或单独的更新包。选择“设置游戏文件”后复制并编译，首次编译可取消后续编。请预留完整游戏副本和编译缓存所需的空间。完成后可通过对应桌面图标直接启动；每个游戏、版本和随机种子使用独立存档。动森 3.0.1 的两种帧率模式共用该版本的存档；低于 60 FPS 时修正模式会变慢，可切回标准模式。

v1.1.0 Randomizer remains experimental: generation/startup checks do not establish full playthrough or parity of every option. Native macOS/Linux gameplay and Metal rendering are not certified by this Windows prerelease. Report fork-specific problems in [fork issues](https://github.com/hjqcan/LASRecomp/issues), including version, seed/settings and logs.

v1.1.0 随机化仍为实验功能，尚未验证完整通关和全部选项组合。本次仅发布并验证 Windows 版本；macOS/Linux 的实际游戏运行及 Metal 渲染仍需硬件验证。

## Repeatable release checks

1. Merge upstream `beta_5` into `multi` and run the relevant CI checks. Preserve the fork features and independent PR branches.
2. Build the runtime and both helpers from the same revision; apply `pe_no_cfg.py` to the Windows generator. Use `tools/package_multi_windows.py` to package only allowlisted files.
3. Before publishing, check setup, cancel/resume, gameplay, audio, save/reopen and the changed features with compatible copied saves. Keep unsupported ports and experimental randomizer options clearly marked.
4. Inspect the package contents and checksum, then publish it as a fork prerelease. Do not add per-run reports or machine-specific evidence to the repository.

The source tree contains no game assets, console keys, generated game code or personal saves. The package includes only distributable program/toolchain files and open fonts.
