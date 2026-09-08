#!/usr/bin/env bash
# Compiles tools/modeling_shot.cpp against what ./build.sh has already made,
# and takes the modelling round's screenshots with it.
#
#   tools/build_modeling_shot.sh [OUTDIR] [W] [H] [PREFIX]
#
# WHY THIS IS NOT IN build.sh, which is where it belongs: build.sh and
# build_win.sh are frozen for this round - they are the one entry point and
# nobody may edit them - and a tool that build.sh does not name is a tool that
# is never compiled. So this script names it, using the SAME flags build.sh
# uses (they are copied, not guessed - keep them in step), and tools/run_tests.sh
# calls it. The day build.sh is editable again these fifteen lines move into
# it and this file goes away; until then a new tool has nowhere else to live.
#
# It links the archives build.sh produced. Without them it says so and stops,
# rather than compiling half of the engine a second time behind the build's
# back.
set -uo pipefail
cd "$(dirname "$0")/.."

OUTDIR=${1:-.gauntlet-shots}
W=${2:-1600}
H=${3:-900}
PREFIX=${4:-}

if [ ! -f build/libdaidalos_vk.a ] || [ ! -f build/libdaidalos.a ]; then
    echo "modeling_shot: build/libdaidalos*.a missing - run ./build.sh first"
    exit 1
fi

JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build}
TALOS=${TALOS:-/root/projects/talos}
AULOS=${AULOS:-/root/projects/aulos}
MNEMOSYNE=${MNEMOSYNE:-/root/projects/mnemosyne}

ARCH="-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse"
FLAGS="-std=c++17 -O3 -fno-rtti -fno-exceptions -ffp-contract=fast -pthread -Wall -Wno-unused-parameter"

AUDIO_LIB=""
[ -f "$AULOS/build/libaulos.a" ] && AUDIO_LIB="$AULOS/build/libaulos.a"
TALOS_LIB=""
[ -f "$TALOS/build/libtalos.a" ] && TALOS_LIB="$TALOS/build/libtalos.a"
X11_LIB=""
[ -f /usr/include/X11/Xlib.h ] && X11_LIB="-lX11"

# The script runtime: the bridge is a JS entry point, so without QuickJS the
# tool still builds and still photographs - it just cannot be driven. Said out
# loud rather than discovered in an empty picture.
SCRIPT_LIB=""
SCRIPT_DEF=""
if [ -f extern/quickjs/libquickjs.a ] && [ -f build/dai_script.o ]; then
    SCRIPT_LIB="build/dai_script.o extern/quickjs/libquickjs.a"
    SCRIPT_DEF="-DDAI_WITH_SCRIPT"
else
    echo "modeling_shot: no script runtime (extern/quickjs) - the bridge will refuse eval"
fi

ASSETS_INC=""
[ -f "$MNEMOSYNE/include/mnemosyne.h" ] && ASSETS_INC="-I$MNEMOSYNE/include"

VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a \
        $AUDIO_LIB $TALOS_LIB -L$JOLT_LIB -lJolt -lvulkan $X11_LIB -lpthread -lm -ldl"

echo "-- modeling_shot"
# shellcheck disable=SC2086
g++ $FLAGS $ARCH $SCRIPT_DEF -Iinclude $ASSETS_INC tools/modeling_shot.cpp \
    $SCRIPT_LIB $VKLIBS -o build/modeling_shot || exit 1

mkdir -p "$OUTDIR"
DAI_SHADER_DIR=shaders ./build/modeling_shot "$OUTDIR" "$W" "$H" ${PREFIX:+--prefix "$PREFIX"}
