#!/usr/bin/env bash
# Daidalos build.
#
#   ./build.sh            engine + both physics backends + tests + examples
#   ./build.sh noaudio    build without Aulos
#
# The interesting part is the "leak test": dai_engine.cpp is compiled WITHOUT
# the Jolt include path. If a Jolt header ever sneaks into the engine core,
# this build fails - which is the whole point of dai_physics.hpp.
set -euo pipefail
cd "$(dirname "$0")"

JOLT_SRC=${JOLT_SRC:-/root/projects/JoltPhysics}
JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build}
AULOS=${AULOS:-/root/projects/aulos}

# These MUST match how libJolt.a was compiled. A mismatch is not a link error,
# it is a runtime crash: the JPH_USE_* defines change the layout of Vec3/Mat44.
JOLT_DEFS="-DJPH_DEBUG_RENDERER -DJPH_OBJECT_STREAM -DJPH_PROFILE_ENABLED \
 -DJPH_USE_AVX -DJPH_USE_AVX2 -DJPH_USE_CPU_COMPUTE -DJPH_USE_F16C -DJPH_USE_FMADD \
 -DJPH_USE_LZCNT -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 -DJPH_USE_TZCNT -DNDEBUG"
ARCH="-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse"
FLAGS="-std=c++17 -O3 -fno-rtti -fno-exceptions -ffp-contract=fast -pthread -Wall -Wno-unused-parameter"

AUDIO_FLAGS="-I$AULOS/include"
AUDIO_LIB="$AULOS/build/libaulos.a"
if [ "${1:-}" = "noaudio" ] || [ ! -f "$AULOS/build/libaulos.a" ]; then
    echo "-- building WITHOUT audio"
    AUDIO_FLAGS="-DDAI_NO_AUDIO"
    AUDIO_LIB=""
fi

mkdir -p build

echo "-- engine core (no Jolt include path - this is the leak test)"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_engine.cpp -o build/dai_engine.o

echo "-- physics backend: null"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/physics_null.cpp -o build/physics_null.o

echo "-- physics backend: jolt"
g++ $FLAGS $ARCH $JOLT_DEFS -Iinclude -Isrc -I"$JOLT_SRC" -c src/physics_jolt.cpp -o build/physics_jolt.o

echo "-- audio"
g++ $FLAGS $ARCH $AUDIO_FLAGS -Iinclude -Isrc -c src/dai_audio.cpp -o build/dai_audio.o

ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o build/dai_audio.o

echo "-- shaders"
if command -v glslangValidator >/dev/null 2>&1; then
    glslangValidator -V shaders/mesh.vert -o shaders/mesh.vert.spv >/dev/null
    glslangValidator -V shaders/mesh.frag -o shaders/mesh.frag.spv >/dev/null
fi

VK_OK=0
if [ -f /usr/include/vulkan/vulkan.h ]; then
    echo "-- renderer: vulkan 1.3"
    g++ $FLAGS $ARCH -Iinclude -c src/rhi_vulkan.cpp -o build/rhi_vulkan.o
    ar rcs build/libdaidalos_vk.a build/rhi_vulkan.o
    VK_OK=1
else
    echo "-- renderer: skipped (no vulkan headers)"
fi

LIBS="build/libdaidalos.a $AUDIO_LIB -L$JOLT_LIB -lJolt -lpthread -lm"

echo "-- tests"
g++ $FLAGS $ARCH -Iinclude tests/test_daidalos.cpp $LIBS -o build/test_daidalos

echo "-- examples"
g++ $FLAGS $ARCH -Iinclude examples/hello_daidalos.cpp $LIBS -o build/hello_daidalos
if [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude examples/vehicle_demo.cpp \
        build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB \
        -L"$JOLT_LIB" -lJolt -lvulkan -lpthread -lm -o build/vehicle_demo
    g++ $FLAGS $ARCH -Iinclude examples/render_demo.cpp \
        build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB \
        -L"$JOLT_LIB" -lJolt -lvulkan -lpthread -lm -o build/render_demo
fi

echo "-- ok"
ls -la build/*.a build/test_daidalos build/hello_daidalos 2>/dev/null | sed 's/^/   /'
