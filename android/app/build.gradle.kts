// The app: SDL's activity running the runtime (libmain.so, src/main/cpp/CMakeLists.txt). Nothing of the game is in
// it: the player picks their own extracted game files on the device. From LASRecompAndroid by mamcjr (MIT).
plugins {
    id("com.android.application")
}

val repo: File = rootDir.resolve("..").canonicalFile
val deps: File = repo.resolve("android/deps/out")  // android/deps/build_deps.sh
val gen = layout.buildDirectory.dir("generated/lasrecomp")

android {
    namespace = "com.lasrecomp"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.lasrecomp.android"
        minSdk = 30
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-dev"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.4"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/java", deps.resolve("java"))  // (SDL's Java side, the same version as libSDL3.so)
            jniLibs.srcDirs(gen.get().asFile.resolve("jniLibs"))
            assets.srcDirs(gen.get().asFile.resolve("assets"))
            res.srcDirs("src/main/res", gen.get().asFile.resolve("res"))
        }
    }

    packaging {
        jniLibs {
            // (extracted to nativeLibraryDir at install: the game code builder's programs run from there)
            useLegacyPackaging = true
            pickFirsts += listOf("**/libSDL3.so", "**/libshaderc_shared.so")
        }
    }

    androidResources {
        noCompress += "zip"  // (gen.zip is compressed already)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// What the APK carries from the repository besides libmain.so: the prebuilt libraries and the shipped resources the
// desktop builds put next to the executable (the start screen's font, the fallback fonts, the icon; their licences).
val prepareLasRecomp by tasks.registering {
    val res = mapOf(
        "Baloo2-SemiBold.ttf" to "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf",
        "Baloo2-OFL.txt" to "third_party/fonts/Baloo2/OFL.txt",
        "BIZUDPGothic-Regular.ttf" to "third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf",
        "BIZUDPGothic-OFL.txt" to "third_party/fonts/BIZUDPGothic/OFL.txt",
        "DejaVuSans.ttf" to "third_party/fonts/DejaVu/DejaVuSans.ttf",
        "DejaVuSans-Bold.ttf" to "third_party/fonts/DejaVu/DejaVuSans-Bold.ttf",
        "DejaVu-LICENSE.txt" to "third_party/fonts/DejaVu/LICENSE",
        "icon.png" to "docs/icon/icon_1024.png").mapValues { repo.resolve(it.value) }
    val libs = listOf("libSDL3.so", "libshaderc_shared.so", "libavcodec.so", "libavformat.so", "libavutil.so",
                      "libswscale.so").map { deps.resolve("lib/$it") } +
        // the game code builder's programs and libraries (android/deps/build_gen_deps.sh): Python and its launcher,
        // the generator's native libraries, zig - lib*.so, the only files Android runs from an app
        listOf("python/libpymain.so", "python/libpython3.14.so", "lib/libcapstone.so", "lib/libunicorn.so",
               "lib/liblz4.so", "zig/libzig.so").map { deps.resolve("gen/$it") }
    inputs.files(res.values, libs)
    outputs.dir(gen)
    doLast {
        val out = gen.get().asFile
        for (lib in libs) lib.copyTo(out.resolve("jniLibs/arm64-v8a/${lib.name}"), overwrite = true)
        for ((name, f) in res) f.copyTo(out.resolve("assets/res/$name"), overwrite = true)
        res.getValue("icon.png").copyTo(out.resolve("res/mipmap-xxxhdpi/ic_launcher.png"), overwrite = true)
    }
}
// The game code builder's files, unpacked by the activity into <internal files>/gen (runtime/gamebuild.c): Python's
// standard library, the generator's bindings, zig's lib/, the generator and the tables it reads, and the headers and
// entry file the generated code is compiled with. Nothing of the game: the player's own files are the input.
val genZip by tasks.registering(Zip::class) {
    val g = deps.resolve("gen")
    archiveFileName.set("gen.zip")
    destinationDirectory.set(gen.map { it.dir("assets") })
    from(g.resolve("python/lib")) { into("python/lib") }
    from(g.resolve("python/LICENSE-python.txt")) { into("python") }
    from(g.resolve("site")) { into("site") }
    from(g.resolve("zig/lib")) { into("zig/lib") }
    from(g.resolve("zig/LICENSE-zig.txt")) { into("zig") }
    from(repo.resolve("tools")) {
        include("gen.py", "lifter.py", "analyze.py", "nso.py", "passes.py", "patches.py", "simd_c.py", "game_addresses.py",
                "rando/patch_variants.json")
        into("tools")
    }
    from(repo.resolve("runtime/game_addresses")) { include("*.h"); into("runtime/game_addresses") }
    from(repo.resolve("runtime")) { include("cpu.h", "simd.h", "simd_fp.h", "gamelib.h", "hle/hle_symbols.txt"); into("gamelib-src")
        eachFile { if (name == "hle_symbols.txt") path = "gamelib-src/hle_symbols.txt" } }
    from(repo.resolve("tools/gamelib_entry.c")) { into("gamelib-src") }
    includeEmptyDirs = false
}
tasks.named("preBuild") { dependsOn(prepareLasRecomp, genZip) }
