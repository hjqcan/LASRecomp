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
            pickFirsts += listOf("**/libSDL3.so", "**/libshaderc_shared.so")
        }
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
    val libs = listOf("libSDL3.so", "libshaderc_shared.so").map { deps.resolve("lib/$it") }
    inputs.files(res.values, libs)
    outputs.dir(gen)
    doLast {
        val out = gen.get().asFile
        for (lib in libs) lib.copyTo(out.resolve("jniLibs/arm64-v8a/${lib.name}"), overwrite = true)
        for ((name, f) in res) f.copyTo(out.resolve("assets/res/$name"), overwrite = true)
        res.getValue("icon.png").copyTo(out.resolve("res/mipmap-xxxhdpi/ic_launcher.png"), overwrite = true)
    }
}
tasks.named("preBuild") { dependsOn(prepareLasRecomp) }
