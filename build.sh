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
TALOS=${TALOS:-/root/projects/talos}
AULOS=${AULOS:-/root/projects/aulos}
MNEMOSYNE=${MNEMOSYNE:-/root/projects/mnemosyne}

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

echo "-- physics backend availability"
# Deciding this BEFORE the engine core is compiled, because dai_engine.cpp is
# what refuses DAI_PHYSICS_TALOS when the backend was not linked in.
if [ -f "${TALOS:-/root/projects/talos}/TalC/talos.h" ] && [ -f "${TALOS:-/root/projects/talos}/build/libtalos.a" ]; then
    ENGINE_DEFS=""
else
    ENGINE_DEFS="-DDAI_NO_TALOS"
fi

echo "-- engine core (no Jolt include path - this is the leak test)"
g++ $FLAGS $ARCH $ENGINE_DEFS -Iinclude -Isrc -c src/dai_engine.cpp -o build/dai_engine.o

echo "-- physics backend: null"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/physics_null.cpp -o build/physics_null.o

echo "-- physics backend: jolt"
g++ $FLAGS $ARCH $JOLT_DEFS -Iinclude -Isrc -I"$JOLT_SRC" -c src/physics_jolt.cpp -o build/physics_jolt.o

# Second real backend: Talos through its C API. Optional the same way audio is
# - without it the engine still builds and DAI_PHYSICS_TALOS is refused rather
# than silently answered with Jolt.
TALOS_OBJ=""
TALOS_LIB=""
if [ -f "$TALOS/TalC/talos.h" ] && [ -f "$TALOS/build/libtalos.a" ]; then
    echo "-- physics backend: talos ($TALOS)"
    g++ $FLAGS $ARCH -Iinclude -Isrc -I"$TALOS/TalC" -c src/physics_talos.cpp -o build/physics_talos.o
    TALOS_OBJ=build/physics_talos.o
    TALOS_LIB="$TALOS/build/libtalos.a"
else
    echo "-- physics backend: talos SKIPPED (no $TALOS/build/libtalos.a - run talos/build.sh)"
fi

echo "-- audio"
g++ $FLAGS $ARCH $AUDIO_FLAGS -Iinclude -Isrc -c src/dai_audio.cpp -o build/dai_audio.o

echo "-- scene layer"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_scene.cpp -o build/dai_scene.o
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_input.cpp -o build/dai_input.o
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_editor.cpp -o build/dai_editor.o

echo "-- scene document (editor truth: stable ids, generic undo)"
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc.cpp -o build/dai_doc.o
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc_text.cpp -o build/dai_doc_text.o
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc_sync.cpp -o build/dai_doc_sync.o

ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o ${TALOS_OBJ} \
       build/dai_audio.o build/dai_scene.o build/dai_input.o build/dai_editor.o \
       build/dai_doc.o build/dai_doc_text.o build/dai_doc_sync.o

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
        # The updater's Windows half (WinHTTP, the rename trick) is only
        # compiled on this path, so check it here rather than discover it on
        # the first Windows build.
        x86_64-w64-mingw32-g++ -std=c++17 -O2 -fno-rtti -fno-exceptions \
            -Iinclude -Isrc -c src/dai_update.cpp -o build/dai_update_win32.o
        echo "   ok: build/rhi_vulkan_window_win32.o, build/dai_update_win32.o"
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
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf_geom.cpp    -o build/dai_gltf_geom.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_fracture.cpp     -o build/dai_fracture.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf_write.cpp   -o build/dai_gltf_write.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_particles.cpp    -o build/dai_particles.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_font.cpp          -o build/dai_font.o
    # Vector icons: the SVG rasteriser and the atlas it packs. Same reasoning
    # as the TrueType loader next to it - a few hundred lines instead of a
    # dependency, and icons that are sharp at whatever size the display wants.
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_svg.cpp           -o build/dai_svg.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_icons.cpp         -o build/dai_icons.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_ui.cpp            -o build/dai_ui.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_update.cpp       -o build/dai_update.o
    g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_editor_ui.cpp     -o build/dai_editor_ui.o
    ar rcs build/libdaidalos_vk.a build/rhi_vulkan.o build/rhi_vulkan_frame.o build/rhi_vulkan_texture.o \
           $WINDOW_OBJ build/dai_meshgen.o build/dai_image.o build/dai_inflate.o build/dai_json.o \
           build/dai_gltf.o build/dai_gltf_geom.o build/dai_gltf_write.o build/dai_fracture.o build/dai_particles.o build/dai_font.o build/dai_svg.o build/dai_icons.o build/dai_ui.o build/dai_update.o \
           build/dai_editor_ui.o
    VK_OK=1
else
    echo "-- renderer: skipped (no vulkan headers)"
fi

# ---- asset layer: Mnemosyne (where bytes come from) + glTF (what they mean)
#
# Optional the same way audio is: without it the engine still builds, scenes
# still save their asset paths, and nothing resolves them. Mnemosyne is
# compiled with ITS flags, not ours - it is a separate library with a C API,
# and forcing -fno-exceptions on someone else's std::vector is not our call.
ASSETS_LIB=""
if [ "$VK_OK" = "1" ] && [ -f "$MNEMOSYNE/include/mnemosyne.h" ]; then
    echo "-- assets: Mnemosyne + glTF ($MNEMOSYNE)"
    MNE_FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread"
    for f in mne_path mne_vfs mne_pack mne_registry; do
        g++ $MNE_FLAGS -I"$MNEMOSYNE/include" -I"$MNEMOSYNE/src" -c "$MNEMOSYNE/src/$f.cpp" -o "build/$f.o"
    done
    g++ $FLAGS $ARCH -Iinclude -Isrc -I"$MNEMOSYNE/include" -c src/dai_assets.cpp -o build/dai_assets.o
    ar rcs build/libdaidalos_assets.a build/dai_assets.o \
           build/mne_path.o build/mne_vfs.o build/mne_pack.o build/mne_registry.o
    ASSETS_LIB="build/libdaidalos_assets.a"
else
    echo "-- assets: skipped (no Mnemosyne at $MNEMOSYNE)"
fi

LIBS="build/libdaidalos.a $AUDIO_LIB ${TALOS_LIB:-} -L$JOLT_LIB -lJolt -lpthread -lm"
VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB ${TALOS_LIB:-} -L$JOLT_LIB -lJolt -lvulkan ${X11_LIB:-} -lpthread -lm"

# --- backend leak tests -------------------------------------------------
# Same idea as the Jolt one, for the renderer: the RHI must be swappable for
# D3D12/Metal/an emitter into someone else's engine by replacing rhi_*.cpp.
echo "-- leak test: no talos.h outside src/physics_talos.cpp"
if grep -l "talos\.h\|tal_world\|tal_body_id" src/*.cpp src/*.hpp include/*.h 2>/dev/null | grep -v "^src/physics_talos.cpp"; then
    echo "   !! a Talos type escaped the backend (files listed above)"; exit 1
fi
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

# Scripting is optional: it is the only part that needs a vendored library
QJS=extern/quickjs
if [ -f "$QJS/libquickjs.a" ]; then
    echo "-- scripting: quickjs"
    g++ $FLAGS $ARCH -Iinclude -Isrc -I"$QJS" -c src/dai_script.cpp -o build/dai_script.o
    SCRIPT_LIB="build/dai_script.o $QJS/libquickjs.a"
else
    echo "-- scripting: skipped (extern/quickjs not built)"
    SCRIPT_LIB=""
fi

echo "-- tests"
g++ $FLAGS $ARCH -Iinclude tests/test_daidalos.cpp $LIBS -o build/test_daidalos
g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_image.cpp src/dai_inflate.cpp -o build/test_image
g++ $FLAGS $ARCH -Iinclude tests/test_merge.cpp $LIBS -o build/test_merge
g++ $FLAGS $ARCH -Iinclude tests/test_save.cpp $LIBS -o build/test_save
g++ $FLAGS $ARCH -Iinclude tests/test_input.cpp $LIBS -o build/test_input
g++ $FLAGS $ARCH -Iinclude tests/test_editor.cpp $LIBS -o build/test_editor
g++ $FLAGS $ARCH -Iinclude tests/test_doc.cpp $LIBS -o build/test_doc
g++ $FLAGS $ARCH -Iinclude tests/test_play.cpp $LIBS -o build/test_play
g++ $FLAGS $ARCH -Iinclude tests/test_cam.cpp $LIBS -o build/test_cam
if [ -n "${TALOS_LIB:-}" ]; then
    # The second real backend, held to the same claims as the first.
    g++ $FLAGS $ARCH -Iinclude tests/test_talos.cpp $LIBS -o build/test_talos
fi
if [ -n "$SCRIPT_LIB" ] && [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude -Isrc -Iextern/quickjs tests/test_script.cpp $SCRIPT_LIB $VKLIBS -o build/test_script
fi
g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_font.cpp src/dai_font.cpp -o build/test_font
# The SVG rasteriser: no renderer, no font, no window - it turns text into
# coverage, so the test reads the coverage back.
g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_svg.cpp src/dai_svg.cpp src/dai_icons.cpp \
    -o build/test_svg && ./build/test_svg
if [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude tests/test_render_visual.cpp $VKLIBS -o build/test_render_visual
    # Cheap and load bearing: dai_key must stay bit identical to the X11
    # keysyms it is defined as, or the X11 backend silently stops matching.
    g++ $FLAGS $ARCH -Iinclude tests/test_keys.cpp -o build/test_keys && ./build/test_keys
    # Looks at the pixels: text that covers ~100%% of its own box is boxes, not
    # glyphs, which is how a broken font binding hid for so long.
    g++ $FLAGS $ARCH -Iinclude tests/test_ui_text.cpp $VKLIBS -o build/test_ui_text
    g++ $FLAGS $ARCH -Iinclude tests/test_gltf.cpp $VKLIBS -o build/test_gltf
    # No renderer: fracture is arithmetic on triangles, so the test runs
    # anywhere, including a machine with no GPU and no display.
    g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_fracture.cpp src/dai_fracture.cpp \
        src/dai_gltf_geom.cpp src/dai_gltf_write.cpp src/dai_json.cpp -o build/test_fracture
    g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_update.cpp src/dai_update.cpp \
        src/dai_json.cpp -o build/test_update
    g++ $FLAGS $ARCH -Iinclude tests/test_particles.cpp $VKLIBS -o build/test_particles
    g++ $FLAGS $ARCH -Iinclude tests/test_skinning.cpp $VKLIBS -o build/test_skinning
    g++ $FLAGS $ARCH -Iinclude tests/test_ui.cpp $VKLIBS -o build/test_ui
    # The world clipped into the scene window, and picking in the same pixels.
    g++ $FLAGS $ARCH -Iinclude tests/test_viewport.cpp $VKLIBS -o build/test_viewport && \
        DAI_SHADER_DIR=shaders ./build/test_viewport
    # Windows and the solid texel every rectangle in the interface is drawn
    # with. Needs no renderer: it reads the atlas and the vertices.
    g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_ui_window.cpp src/dai_ui.cpp src/dai_font.cpp \
        src/dai_svg.cpp src/dai_icons.cpp -o build/test_ui_window
    # Text fields: selection, caret, Home/End, Escape - and the resize edges.
    # No renderer: input in, vertices out.
    g++ $FLAGS $ARCH -Iinclude -Isrc tests/test_ui_field.cpp src/dai_ui.cpp src/dai_font.cpp \
        src/dai_svg.cpp src/dai_icons.cpp -o build/test_ui_field && ./build/test_ui_field
    g++ $FLAGS $ARCH -Iinclude tests/test_editor_ui.cpp $VKLIBS -o build/test_editor_ui
    [ -n "${X11_LIB:-}" ] && g++ $FLAGS $ARCH -Iinclude tests/test_window.cpp $VKLIBS -o build/test_window
    if [ -n "$ASSETS_LIB" ]; then
        g++ $FLAGS $ARCH -Iinclude -I"$MNEMOSYNE/include" tests/test_assets.cpp \
            $ASSETS_LIB $LIBS $VKLIBS -o build/test_assets
    fi
fi

echo "-- diagnostics"
if [ "$VK_OK" = "1" ]; then
    g++ $FLAGS $ARCH -Iinclude tools/gizmo_shot.cpp $VKLIBS -o build/gizmo_shot
    # The fracture baker needs no renderer: it reads geometry, cuts it and
    # writes geometry. Linking the Vulkan half in would make a build tool
    # depend on a GPU driver being present.
    g++ $FLAGS $ARCH -Iinclude -Isrc tools/daifracture.cpp src/dai_fracture.cpp src/dai_gltf_geom.cpp \
        src/dai_gltf_write.cpp src/dai_json.cpp -o build/daifracture
    g++ $FLAGS $ARCH -Iinclude tools/editor_shot.cpp $VKLIBS -o build/editor_shot
fi

echo "-- examples"
g++ $FLAGS $ARCH -Iinclude examples/hello_daidalos.cpp $LIBS -o build/hello_daidalos
if [ "$VK_OK" = "1" ]; then
    for ex in sandbox_demo vehicle_demo model_viewer window_demo particles_demo editor_demo; do
        [ -f "examples/$ex.cpp" ] || continue
        # The editor drives the asset layer directly (mount, list, instantiate),
        # so it links it in when it exists; the other examples stay lean.
        EXTRA=""
        EXTRA_I=""
        if [ "$ex" = "editor_demo" ] && [ -n "$ASSETS_LIB" ]; then
            EXTRA="$ASSETS_LIB"
            EXTRA_I="-I$MNEMOSYNE/include"
        fi
        g++ $FLAGS $ARCH -Iinclude $EXTRA_I "examples/$ex.cpp" $EXTRA $VKLIBS -o "build/$ex"
    done
fi

echo "-- ok"
ls -la build/*.a build/test_daidalos build/hello_daidalos 2>/dev/null | sed 's/^/   /'
