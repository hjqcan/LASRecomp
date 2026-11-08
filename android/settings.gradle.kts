// The LAS Recomp Android app (android/app). Build: android/deps/build_deps.sh once, then ./gradlew assembleDebug here.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "LASRecomp"
include(":app")
