#!/usr/bin/env bash
# Daidalos build.
#
#   ./build.sh            engine + physics backends + renderer + tests + examples
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

echo "-- scene layer"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_scene.cpp -o build/dai_scene.o

ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o \
       build/dai_audio.o build/dai_scene.o

echo "-- shaders"
if command -v glslangValidator >/dev/null 2>&1; then
    for s in mesh.vert mesh.frag shadow.vert sky.vert sky.frag; do
        glslangValidator -V "shaders/$s" -o "shaders/$s.spv" >/dev/null
    done
else
    echo "   glslangValidator missing - using the .spv files already in shaders/"
fi

VK_OK=0
if [ -f /usr/include/vulkan/vulkan.h ]; then
    echo "-- renderer: vulkan 1.3"
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan.cpp       -o build/rhi_vulkan.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_frame.cpp -o build/rhi_vulkan_frame.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_texture.cpp -o build/rhi_vulkan_texture.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_meshgen.cpp      -o build/dai_meshgen.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_image.cpp        -o build/dai_image.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_inflate.cpp      -o build/dai_inflate.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_json.cpp         -o build/dai_json.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf.cpp         -o build/dai_gltf.o
    ar rcs build/libdaidalos_vk.a build/rhi_vulkan.o build/rhi_vulkan_frame.o build/rhi_vulkan_texture.o \
           build/dai_meshgen.o build/dai_image.o build/dai_inflate.o build/dai_json.o build/dai_gltf.o
    VK_OK=1
else
    echo "-- renderer: skipped (no vulkan headers)"
fi

LIBS="build/libdaidalos.a $AUDIO_LIB -L$JOLT_LIB -lJolt -lpthread -lm"
VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB -L$JOLT_LIB -lJolt -lvulkan -lpthread -lm"

# --- backend leak tests -------------------------------------------------
# Same idea as the Jolt one, for the renderer: the RHI must be swappable for
# D3D12/Metal/an emitter into someone else's engine by replacing rhi_*.cpp.
echo "-- leak test: no Vulkan outside src/rhi_vulkan*"
if grep -l "vulkan/vulkan.h\|VkDevice\|vkCmd" src/*.cpp src/*.hpp include/*.h 2>/dev/null | grep -v "^src/rhi_vulkan"; then
    echo "   !! a Vulkan type escaped the backend (files listed above)"; exit 1
fi
echo "-- leak test: engine + scene link without a renderer"
cat > build/_norender.cpp <<'EOT'
#include "daidalos.h"
#include "dai_scene.h"
int main() {
    dai_config c{}; dai_world *w = nullptr;
    if (dai_create(&c, &w) != DAI_OK) return 1;
    dai_scene *s = dai_scene_create(w);
    dai_entity_desc d = dai_entity_desc_default();
    d.body.shape = DAI_SHAPE_SPHERE; d.body.motion = DAI_DYNAMIC; d.body.half_extent = { 1,0,0 };
    dai_scene_spawn(s, &d);
    dai_step(w);
    dai_render_instance inst[8];
    uint32_t n = dai_scene_instances(s, inst, 8, 0.0f);
    dai_scene_destroy(s); dai_destroy(w);
    return n == 1 ? 0 : 2;
}
EOT
g++ $FLAGS $ARCH -Iinclude build/_norender.cpp $LIBS -o build/_norender    # note: no -lvulkan
./build/_norender || { echo "   !! scene layer cannot run without a renderer"; exit 1; }
rm -f build/_norender build/_norender.cpp

echo "-- tests"
g++ $FLAGS $ARCH -Iinclude tests/test_daidalos.cpp $LIBS -o build/test_daidalos
g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_image.cpp src/dai_inflate.cpp -o build/test_image
if [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude tests/test_render_visual.cpp $VKLIBS -o build/test_render_visual
    g++ $FLAGS $ARCH -Iinclude tests/test_gltf.cpp $VKLIBS -o build/test_gltf
fi

echo "-- examples"
g++ $FLAGS $ARCH -Iinclude examples/hello_daidalos.cpp $LIBS -o build/hello_daidalos
if [ "$VK_OK" = "1" ]; then
    for ex in sandbox_demo vehicle_demo model_viewer; do
        [ -f "examples/$ex.cpp" ] || continue
        g++ $FLAGS $ARCH -Iinclude "examples/$ex.cpp" $VKLIBS -o "build/$ex"
    done
fi

echo "-- ok"
ls -la build/*.a build/test_daidalos build/hello_daidalos 2>/dev/null | sed 's/^/   /'
