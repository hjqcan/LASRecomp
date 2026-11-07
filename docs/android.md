# Android

The Android port is coming in pieces, from [mamcjr's LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)
(MIT). What is here so far: the app opens the start screen, the player picks their game files folder with Android's own
folder picker, and the setup copies them in and builds the game code on the device; the game starts, movies included.
Still to come: touch controls and per-GPU Vulkan settings.

Nothing from the game ships in the app. The game code is built on the device from the player's own extracted game
files, as on the desktop.

## Building

Needs the Android SDK with NDK 30.0.16248370, CMake 3.30.4 and platform 36, JDK 17 or newer, bash, curl and Python 3.

```
bash android/deps/build_deps.sh        # SDL3, shaderc, volk, Vulkan headers, FFmpeg for arm64 (once)
bash android/deps/build_gen_deps.sh    # the game code builder: Python, capstone, unicorn, lz4, zig (once)
cd android && ./gradlew assembleDebug  # android/app/build/outputs/apk/debug/app-debug.apk
```

## How it differs from Linux

- `runtime/platform/linux.c` builds for Android too (`__ANDROID__` parts): the log goes to logcat (tag `LASRecomp`)
  as well as `lasrecomp.log`, backtraces come from the unwinder, and the process can't restart itself.
- The guest window moves (`LARECOMP_DYN_BASE`, `runtime/rt.h`): an app process is forked with its libraries already
  mapped, so `runtime/main.c` picks a free spot from `/proc/self/maps`.
- The data folder is `Android/data/com.lasrecomp.android/files`. The fonts and icon are copied from the APK to the
  app's internal storage at start, where `rt_resource` finds them.
- Every Vulkan function comes through volk: Android's loader exports only Vulkan 1.1 before API 33.
- The swapchain is left unrotated and the compositor turns it, so presents report `VK_SUBOPTIMAL_KHR` by design.
- The game files folder comes from Android's folder picker (a `content://` folder, not a path): the setup copies
  exefs and romfs through the storage framework into the data folder.
- The game code is built on the device as on the desktop (`runtime/gamebuild.c`): the generator runs on Python for
  Android, and zig compiles for `aarch64-linux-musl` and links against `libmain.so`. Android runs programs only from
  the app's native library folder, so Python's launcher and zig ship there as `lib*.so`; the rest is unpacked from
  `assets/gen.zip` at the first start after an install or update. The library is loaded from a copy in internal
  storage, since shared storage is mounted noexec.

## Testing in the emulator

An arm64 system image (API 35, Google APIs) with host GPU runs the start screen on a Mac with Apple Silicon:

```
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity
adb logcat -s LASRecomp
```

The desktop's debug switches ([debug-hooks.md](debug-hooks.md)) are passed with the launch (debug builds only):
`adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es LARECOMP_LOG_SKIPS 1`.

The emulator draws the start screen and the movies, but not the game's world: its Vulkan goes to the Mac through
gfxstream and MoltenVK, whose translation of the game's shaders to Metal fails to compile ("call to 'fmin' is
ambiguous"), so those draws are skipped. A phone's GPU compiles them directly; the game itself runs there.
