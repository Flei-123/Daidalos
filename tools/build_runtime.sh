#!/usr/bin/env bash
# Builds the RUNTIME TEMPLATE - the shipped game binary - and the packer.
#
#   tools/build_runtime.sh            both targets, whichever is possible
#   tools/build_runtime.sh linux      build/daidalos_runtime, build/dai_pack
#   tools/build_runtime.sh windows    build-win/daidalos_runtime.exe, dai_pack.exe
#
# This is deliberately NOT part of build.sh or build_win.sh: those two are
# someone else's file right now. It links against the archives they produce, so
# the runtime is built from exactly the objects the editor is built from - same
# flags, same Jolt defines, same renderer. Run ./build.sh (or ./build_win.sh)
# first; this script says so if you have not.
#
# What the runtime template is FOR: dai_project_export copies it and appends
# the project archive to the copy. Nothing is compiled at export time. That is
# the Godot model, and the reason a user needs no toolchain to ship a game.
#
#   ./build.sh
#   tools/build_runtime.sh linux
#   build/dai_pack export projects/MyGame build/daidalos_runtime /tmp/MyGame
#   /tmp/MyGame                      <- one file, runs anywhere
set -euo pipefail
cd "$(dirname "$0")/.."

WHICH=${1:-both}
JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build}
JOLT_LIB_WIN=${JOLT_LIB_WIN:-/root/projects/jolt-build-win}
TALOS=${TALOS:-/root/projects/talos}
AULOS=${AULOS:-/root/projects/aulos}

# The engine's own flags. These MUST match build.sh: Jolt's headers change
# structure layout on its defines, so a mismatch links fine and then crashes.
FLAGS="-std=c++17 -O3 -fno-rtti -fno-exceptions -ffp-contract=fast -pthread -Wall -Wno-unused-parameter"
ARCH="-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse"

built_any=0

# ---------------------------------------------------------------- linux
build_linux() {
    if [ ! -f build/libdaidalos.a ] || [ ! -f build/libdaidalos_vk.a ]; then
        echo "-- linux: skipped (no build/libdaidalos*.a - run ./build.sh first)"
        return 1
    fi
    echo "-- linux runtime"
    mkdir -p build

    AUDIO_LIB=""
    [ -f "$AULOS/build/libaulos.a" ] && AUDIO_LIB="$AULOS/build/libaulos.a"
    TALOS_LIB=""
    [ -f "$TALOS/build/libtalos.a" ] && TALOS_LIB="$TALOS/build/libtalos.a"
    X11_LIB=""
    [ -f /usr/include/X11/Xlib.h ] && X11_LIB="-lX11"

    # The archive layer and the exporter, compiled here rather than taken from
    # build.sh - they are new files and build.sh does not know about them yet.
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_vfs.cpp    -o build/dai_vfs.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_export.cpp -o build/dai_export.o

    SCRIPT_OBJ=""
    SCRIPT_DEF=""
    SCRIPT_INC=""
    if [ -f extern/quickjs/libquickjs.a ]; then
        g++ $FLAGS $ARCH -Iinclude -Isrc -Iextern/quickjs -c src/dai_script.cpp -o build/dai_script.o
        SCRIPT_OBJ="build/dai_script.o extern/quickjs/libquickjs.a"
        SCRIPT_DEF="-DDAI_WITH_SCRIPT"
        SCRIPT_INC="-Iextern/quickjs"
    else
        echo "   scripting: skipped (extern/quickjs/libquickjs.a missing)"
    fi

    VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a \
            $AUDIO_LIB $TALOS_LIB -L$JOLT_LIB -lJolt -lvulkan $X11_LIB -lpthread -lm"

    g++ $FLAGS $ARCH $SCRIPT_DEF -Iinclude -Isrc $SCRIPT_INC \
        examples/runtime_main.cpp build/dai_vfs.o $SCRIPT_OBJ $VKLIBS \
        -o build/daidalos_runtime
    echo "   ok: build/daidalos_runtime"

    # The packer. No renderer, no physics: it reads directories and writes an
    # archive, so it links four files and nothing else.
    g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Iinclude -Isrc \
        tools/dai_pack.cpp src/dai_vfs.cpp src/dai_export.cpp src/dai_project.cpp \
        -o build/dai_pack
    echo "   ok: build/dai_pack"

    # The archive format's own test. Run here rather than merely built: the
    # byte layout, the append and the path rules are things that break silently
    # and only show up on the machine the game was handed to.
    g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Iinclude -Isrc \
        tests/test_vfs.cpp src/dai_vfs.cpp src/dai_export.cpp src/dai_project.cpp \
        -o build/test_vfs
    ./build/test_vfs

    # A self check that costs a second and catches the one mistake that would
    # otherwise only show up on someone else's machine: a runtime template that
    # cannot find an archive appended to it.
    if [ -d projects/Untitled ]; then
        ./build/dai_pack export projects/Untitled build/daidalos_runtime \
            build/_runtime_selftest >/dev/null
        ./build/dai_pack verify build/_runtime_selftest | sed 's/^/   selftest: /'
        rm -f build/_runtime_selftest build/_runtime_selftest.log
    fi
    return 0
}

# -------------------------------------------------------------- windows
build_windows() {
    CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
    if ! command -v "$CXX" >/dev/null 2>&1; then
        echo "-- windows: skipped (no $CXX)"
        return 1
    fi
    if [ ! -f build-win/libdaidalos.a ] || [ ! -f build-win/libdaidalos_vk.a ]; then
        echo "-- windows: skipped (no build-win/libdaidalos*.a - run ./build_win.sh first)"
        return 1
    fi
    echo "-- windows runtime"
    OUT=build-win

    # Same flags build_win.sh uses, including -DDAI_NO_AUDIO: Aulos is a
    # separate project and is not part of what the export proves.
    WFLAGS="-std=c++17 -O2 -fno-rtti -fno-exceptions -Wall -Wno-unused-function \
            -DUNICODE -D_UNICODE -DDAI_NO_AUDIO"
    VKINC=/tmp/dai_vkinc
    mkdir -p "$VKINC" && cp -r /usr/include/vulkan "$VKINC/" 2>/dev/null || true
    mkdir -p "$VKINC/vk_video" && cp -r /usr/include/vk_video/* "$VKINC/vk_video/" 2>/dev/null || true

    if [ ! -f "$OUT/libvulkan-1.a" ]; then
        x86_64-w64-mingw32-dlltool -d thirdparty/win/vulkan-1.def -l "$OUT/libvulkan-1.a" -D vulkan-1.dll
    fi

    $CXX $WFLAGS -Iinclude -Isrc -I"$VKINC" -c src/dai_vfs.cpp    -o "$OUT/dai_vfs.o"
    $CXX $WFLAGS -Iinclude -Isrc -I"$VKINC" -c src/dai_export.cpp -o "$OUT/dai_export.o"

    SCRIPT_OBJ=""
    SCRIPT_DEF=""
    SCRIPT_INC=""
    QJS_WIN=${QJS_WIN:-extern/quickjs/libquickjs-win.a}
    if [ -f "$QJS_WIN" ]; then
        # dai_script.o is already inside build_win.sh's libdaidalos.a; only the
        # library itself has to come along.
        SCRIPT_OBJ="$QJS_WIN"
        SCRIPT_DEF="-DDAI_WITH_SCRIPT"
        SCRIPT_INC="-Iextern/quickjs"
    else
        echo "   scripting: skipped (no $QJS_WIN)"
    fi

    TALOS_LINK=""
    [ -f "$TALOS/build-win/libtalos.a" ] && TALOS_LINK="$TALOS/build-win/libtalos.a"

    LIBS="$OUT/libdaidalos_vk.a $OUT/libdaidalos.a $OUT/libdaidalos_vk.a \
          ${TALOS_LINK:-} -L$JOLT_LIB_WIN -lJolt -L$OUT -lvulkan-1 -lwinhttp -lgdi32 \
          -luser32 -lshell32 -lcomdlg32 $SCRIPT_OBJ -static -static-libgcc -static-libstdc++ -lpthread"

    # -mwindows is the whole Windows finish: SUBSYSTEM:WINDOWS, so a double
    # clicked game does not flash a black console box behind its window. The
    # log file next to the exe (see runtime_main.cpp) is what replaces stdout.
    $CXX $WFLAGS $SCRIPT_DEF -Iinclude -Isrc $SCRIPT_INC -I"$VKINC" \
        examples/runtime_main.cpp "$OUT/dai_vfs.o" $LIBS -mwindows \
        -o "$OUT/daidalos_runtime.exe"
    echo "   ok: $OUT/daidalos_runtime.exe"

    # The packer runs on Windows too - it is what an editor there calls out to
    # when it does not want to link the exporter in.
    $CXX -std=c++17 -O2 -Wall -Wno-unused-function -Iinclude -Isrc \
        tools/dai_pack.cpp src/dai_vfs.cpp src/dai_export.cpp src/dai_project.cpp \
        -static -static-libgcc -static-libstdc++ -o "$OUT/dai_pack.exe"
    echo "   ok: $OUT/dai_pack.exe"
    return 0
}

case "$WHICH" in
linux)   build_linux && built_any=1 ;;
windows) build_windows && built_any=1 ;;
both)
    build_linux   && built_any=1 || true
    build_windows && built_any=1 || true
    ;;
*) echo "usage: $0 [linux|windows|both]"; exit 2 ;;
esac

[ "$built_any" = "1" ] || { echo "-- nothing built"; exit 1; }
ls -la build/daidalos_runtime build/dai_pack build-win/daidalos_runtime.exe build-win/dai_pack.exe 2>/dev/null | sed 's/^/   /'
echo "-- ok"
