#!/usr/bin/env bash
# Compiles tools/blockout_shot.cpp against what ./build.sh has already made,
# and takes the blockout round's screenshots with it.
#
#   tools/build_blockout_shot.sh [OUTDIR] [W] [H] [PREFIX]
#
# Same reason this is not in build.sh as tools/build_modeling_shot.sh gives:
# build.sh and build_win.sh are frozen for this round, and a tool build.sh does
# not name is a tool that is never compiled. The flags are the ones build.sh
# uses - copied from build_modeling_shot.sh, not guessed - minus the script
# runtime: this tool builds its room through dai_doc in C++ and needs no JS.
#
# It links the archives build.sh produced. Without them it says so and stops,
# rather than compiling half of the engine a second time behind the build's
# back. A non-zero exit is a red line in tools/run_tests.sh.
set -uo pipefail
cd "$(dirname "$0")/.."

OUTDIR=${1:-.gauntlet-shots}
W=${2:-1600}
H=${3:-900}
PREFIX=${4:-}

if [ ! -f build/libdaidalos_vk.a ] || [ ! -f build/libdaidalos.a ]; then
    echo "blockout_shot: build/libdaidalos*.a missing - run ./build.sh first"
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

ASSETS_INC=""
[ -f "$MNEMOSYNE/include/mnemosyne.h" ] && ASSETS_INC="-I$MNEMOSYNE/include"

VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a \
        $AUDIO_LIB $TALOS_LIB -L$JOLT_LIB -lJolt -lvulkan $X11_LIB -lpthread -lm -ldl"

echo "-- blockout_shot"
# shellcheck disable=SC2086
g++ $FLAGS $ARCH -Iinclude $ASSETS_INC tools/blockout_shot.cpp \
    $VKLIBS -o build/blockout_shot || exit 1

mkdir -p "$OUTDIR"
DAI_SHADER_DIR=shaders ./build/blockout_shot "$OUTDIR" "$W" "$H" ${PREFIX:+--prefix "$PREFIX"}
