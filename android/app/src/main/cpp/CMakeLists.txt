# libmain.so: the LAS Recomp runtime for Android (what the Makefile builds into the desktop executable), with the
# Vulkan backend and the ImGui UI, built with the moving guest window (LARECOMP_DYN_BASE: runtime/rt.h). The game code
# is not in it: it is built on the device from the player's own extracted game files. From LASRecompAndroid by mamcjr
# (MIT).
cmake_minimum_required(VERSION 3.22)
project(lasrecomp C CXX)

get_filename_component(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)   # the repository
set(DEPS ${R}/android/deps/out)                                                  # android/deps/build_deps.sh
if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "run android/deps/build_deps.sh first")
endif()

# ids the Makefile computes from the sources (runtime/gamelib.h, gamebuild.c, shader_prep.c). The game code flags (built
# on the device by runtime/gamebuild.c with zig for aarch64-linux-musl): the Makefile's GENFLAGS with the moving window,
# and from LASRecompAndroid by mamcjr (MIT): -dag-maps-huge-region=500 (the scheduler's dependency graph of huge runs of
# stores was quadratic: one file 4.3 s -> 0.7 s), no UBSan or stack protector (zig cc adds them on its own; memory a
# phone's build doesn't have). Linked -nostdlib against libmain.so: Android's linker resolves its libc calls through it.
set(GAMELIB_CFLAGS "-O1 -g0 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -mllvm -disable-licm-promotion -fPIC -DLARECOMP_DYN_BASE -mllvm -dag-maps-huge-region=500 -fno-sanitize=undefined -fno-stack-protector")
set(GAMELIB_SHARED "-shared -nostdlib -Wl,-z,max-page-size=16384")
function(las_id out len)
    set(t "")
    foreach(f ${ARGN})
        if(EXISTS ${R}/${f})
            file(READ ${R}/${f} c)
            string(APPEND t "${c}")
            # (configure again when it changes: an id that stayed put kept a stale pipeline cache or game code)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/${f})
        else()
            string(APPEND t "${f}")
        endif()
    endforeach()
    string(SHA256 h "${t}")
    string(SUBSTRING ${h} 0 ${len} h)
    set(${out} ${h} PARENT_SCOPE)
endfunction()
file(GLOB PATCH_TABLES RELATIVE ${R} ${R}/runtime/game_addresses/patches_v*.h)
list(SORT PATCH_TABLES)
las_id(GAMELIB_ID 16 tools/game_addresses.py runtime/game_addresses/versions.h ${PATCH_TABLES} tools/gen.py tools/lifter.py
       tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py tools/rando/patch_variants.json
       tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
       "${GAMELIB_CFLAGS}\n")
las_id(GAMELIB_BASE_ID 16 runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/fetch_zig.sh
       "${GAMELIB_CFLAGS} ${GAMELIB_SHARED}\n")
las_id(TRANSLATOR_ID 12 runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h)

# a text file as a C string, one "...\n" literal per line (as the Makefile makes them; CRs dropped)
function(las_c_string in out)
    file(READ ${in} t)
    string(REPLACE "\r" "" t "${t}")
    string(REPLACE "\\" "\\\\" t "${t}")
    string(REPLACE "\"" "\\\"" t "${t}")
    string(REGEX REPLACE "\n$" "" t "${t}")
    string(REPLACE "\n" "\\n\"\n\"" t "${t}")
    file(WRITE ${out} "\"${t}\\n\"\n${ARGN}")
endfunction()
set(INC ${CMAKE_BINARY_DIR}/inc)
las_c_string(${R}/third_party/smaa/SMAA.hlsl ${INC}/smaa_hlsl.inc)
las_c_string(${R}/runtime/nvn/shader_variants.txt ${INC}/shader_variants.inc "\"\"\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/third_party/smaa/SMAA.hlsl
             ${R}/runtime/nvn/shader_variants.txt)

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libSDL3.so)
add_library(shaderc_shared SHARED IMPORTED)
set_target_properties(shaderc_shared PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libshaderc_shared.so)
foreach(lib avcodec avformat avutil swscale)  # (FFmpeg, LGPL: the movies and the start screen's background frame)
    add_library(${lib} SHARED IMPORTED)
    set_target_properties(${lib} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${lib}.so)
endforeach()

file(GLOB RT_SRC ${R}/runtime/*.c ${R}/runtime/*.cpp ${R}/runtime/hle/*.c ${R}/runtime/services/*.c ${R}/runtime/nvn/*.c
     ${R}/runtime/nvn/maxwell/*.c)
file(GLOB VK_SRC ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
# libadrenotools (BSD-2; android/deps/build_deps.sh unpacks it): a custom Vulkan driver (Turnip) in a linker namespace of
# its own. Its hook libraries (hook_impl, main_hook, ...) are shared libraries of this build: they ship in the APK.
add_subdirectory(${DEPS}/adrenotools ${CMAKE_BINARY_DIR}/adrenotools)
file(GLOB UI_SRC ${R}/runtime/ui/*.mm)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

add_library(main SHARED ${RT_SRC} ${R}/runtime/platform/linux.c ${R}/runtime/platform/android_storage.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC} ${DEPS}/volk/volk.c)
# the UI sources are plain C++ apart from __APPLE__ blocks (as on Linux / Windows)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS
    "GAMELIB_ID=\"${GAMELIB_ID}\";GAMELIB_BASE_ID=\"${GAMELIB_BASE_ID}\";GAMELIB_CFLAGS=\"${GAMELIB_CFLAGS}\";GAMELIB_SHARED=\"${GAMELIB_SHARED}\"")
set_source_files_properties(${R}/runtime/nvn/shader_prep.c PROPERTIES COMPILE_DEFINITIONS
    "SHADER_TRANSLATOR_ID=\"${TRANSLATOR_ID}\"")

target_include_directories(main PRIVATE ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${DEPS}/include ${DEPS}/volk ${INC}
                           ${R}/third_party/smaa/Textures)
# every Vulkan function through volk (runtime/nvn/vulkan/vk.h), ImGui's and VMA's too
target_compile_definitions(main PRIVATE LARECOMP_DYN_BASE VK_USE_PLATFORM_ANDROID_KHR VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_USE_VOLK VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_compile_options(main PRIVATE -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format  # (as on Linux)
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
# -z nostart-stop-gc: tables in named sections found through
# __start_ / __stop_ symbols (runtime/hle.h, ipc.h, main.c); 16 KB pages (Android 15+)
target_link_libraries(main PRIVATE SDL3 shaderc_shared avcodec avformat avutil swscale adrenotools android log dl m)
target_link_options(main PRIVATE -Wl,-z,nostart-stop-gc -Wl,-z,max-page-size=16384)
