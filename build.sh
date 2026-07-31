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
    # A failed shader compile used to leave the previous .spv in place, so the
    # renderer silently kept running the OLD shader - which cost an afternoon
    # of "why do the lights do nothing". Fail loudly instead.
    for s in mesh.vert mesh.frag shadow.vert sky.vert sky.frag particle.vert particle.frag ui.vert ui.frag; do
        if ! glslangValidator -V "shaders/$s" -o "shaders/$s.spv.new" >/tmp/glsl_$s.log 2>&1; then
            echo "   !! shader $s failed to compile:"; sed -n '1,12p' /tmp/glsl_$s.log; exit 1
        fi
        mv "shaders/$s.spv.new" "shaders/$s.spv"
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
    # Window backend: DAI_WINDOW=x11|wayland|none (default: x11 if available).
    # Exactly one is linked - they define the same four entry points, which is
    # the same "one .cpp per platform" rule the renderer itself follows.
    WINDOW_OBJ=""
    X11_LIB=""
    DAI_WINDOW=${DAI_WINDOW:-auto}
    if [ "$DAI_WINDOW" = "auto" ]; then
        if [ -f /usr/include/X11/Xlib.h ]; then DAI_WINDOW=x11; else DAI_WINDOW=none; fi
    fi
    case "$DAI_WINDOW" in
    x11)
        echo "-- window backend: X11"
        g++ $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_window.cpp -o build/rhi_vulkan_window.o
        WINDOW_OBJ=build/rhi_vulkan_window.o
        X11_LIB="-lX11"
        ;;
    wayland)
        echo "-- window backend: Wayland"
        gcc $ARCH -O2 -Iinclude -Isrc -c src/generated/xdg-shell-protocol.c -o build/xdg-shell-protocol.o
        g++ $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_window_wayland.cpp -o build/rhi_vulkan_window.o
        WINDOW_OBJ="build/rhi_vulkan_window.o build/xdg-shell-protocol.o"
        X11_LIB="-lwayland-client"
        ;;
    win32)
        # Cross compiled with mingw-w64 to prove it BUILDS; it has not been run
        # on Windows from here. Produces an object file, not a linked binary.
        echo "-- window backend: win32 (cross compile check only)"
        mkdir -p /tmp/vkinc && cp -r /usr/include/vulkan /tmp/vkinc/ 2>/dev/null || true
        x86_64-w64-mingw32-g++ -std=c++17 -O2 -fno-rtti -fno-exceptions \
            -Iinclude -Isrc -I/tmp/vkinc -c src/rhi_vulkan_window_win32.cpp -o build/rhi_vulkan_window_win32.o
        echo "   ok: build/rhi_vulkan_window_win32.o"
        ;;
    *)
        echo "-- window backend: none (headless only)"
        ;;
    esac
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_meshgen.cpp      -o build/dai_meshgen.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_image.cpp        -o build/dai_image.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_inflate.cpp      -o build/dai_inflate.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_json.cpp         -o build/dai_json.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf.cpp         -o build/dai_gltf.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_particles.cpp    -o build/dai_particles.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_font.cpp          -o build/dai_font.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_ui.cpp            -o build/dai_ui.o
    ar rcs build/libdaidalos_vk.a build/rhi_vulkan.o build/rhi_vulkan_frame.o build/rhi_vulkan_texture.o \
           $WINDOW_OBJ build/dai_meshgen.o build/dai_image.o build/dai_inflate.o build/dai_json.o \
           build/dai_gltf.o build/dai_particles.o build/dai_font.o build/dai_ui.o
    VK_OK=1
else
    echo "-- renderer: skipped (no vulkan headers)"
fi

LIBS="build/libdaidalos.a $AUDIO_LIB -L$JOLT_LIB -lJolt -lpthread -lm"
VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB -L$JOLT_LIB -lJolt -lvulkan ${X11_LIB:-} -lpthread -lm"

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
g++ $FLAGS $ARCH -Iinclude tests/test_merge.cpp $LIBS -o build/test_merge
g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_font.cpp src/dai_font.cpp -o build/test_font
if [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude tests/test_render_visual.cpp $VKLIBS -o build/test_render_visual
    g++ $FLAGS $ARCH -Iinclude tests/test_gltf.cpp $VKLIBS -o build/test_gltf
    g++ $FLAGS $ARCH -Iinclude tests/test_particles.cpp $VKLIBS -o build/test_particles
    g++ $FLAGS $ARCH -Iinclude tests/test_skinning.cpp $VKLIBS -o build/test_skinning
    g++ $FLAGS $ARCH -Iinclude tests/test_ui.cpp $VKLIBS -o build/test_ui
    [ -n "${X11_LIB:-}" ] && g++ $FLAGS $ARCH -Iinclude tests/test_window.cpp $VKLIBS -o build/test_window
fi

echo "-- examples"
g++ $FLAGS $ARCH -Iinclude examples/hello_daidalos.cpp $LIBS -o build/hello_daidalos
if [ "$VK_OK" = "1" ]; then
    for ex in sandbox_demo vehicle_demo model_viewer window_demo particles_demo; do
        [ -f "examples/$ex.cpp" ] || continue
        g++ $FLAGS $ARCH -Iinclude "examples/$ex.cpp" $VKLIBS -o "build/$ex"
    done
fi

echo "-- ok"
ls -la build/*.a build/test_daidalos build/hello_daidalos 2>/dev/null | sed 's/^/   /'
