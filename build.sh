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

# The whole build, on the terminal AND in build/last_run.log. RUN.md quotes
# lines out of a run - "ok: N checks, 0 failures" from the drone show suite,
# the scaling table, the screenshot tool's own log - and a quoted number that
# nobody can go back and re-read is a number that ages into a claim. The log is
# TRUNCATED here and appended to by tools/run_tests.sh, so the file holds the
# two commands in the order RUN.md documents them.
#
# Re-run through `tee` rather than redirected into a process substitution: the
# exit code has to survive (a broken build must stay broken), and a background
# tee can lose the last lines - which are the ones that say whether it worked.
if [ "${DAI_RUN_LOG:-}" != "1" ]; then
    mkdir -p build
    echo "=== ./build.sh $* $(date -u '+%Y-%m-%dT%H:%M:%SZ') ===" > build/last_run.log
    DAI_RUN_LOG=1 "$0" "$@" 2>&1 | tee -a build/last_run.log
    exit "${PIPESTATUS[0]}"
fi

# Why every "compile, then run" below is two statements and never
# `g++ ... && ./build/x`: under `set -e` a command on the LEFT of an AND-list is
# exempt from errexit, so that shape turns a compiler error into a silent skip -
# the build prints "-- ok" and the test that "passed" was yesterday's binary.
# That is exactly how this checkout spent a round with a drone show suite that
# had not compiled since the day before. Two statements: the compile fails, the
# build stops.

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

# ccache in front of the compiler. Measured 19.09.2026: the same translation
# unit is 2.38 s cold and 0.00 s on a cache hit, so a branch switch and back -
# exactly what an agent does between iterations - costs nothing instead of six
# minutes. It only ever helps: a miss adds ~0.2 s. Note it can only cache
# COMPILES; the ~50 calls below that compile and link in one go are helped by
# the parallelism, not by the cache.
CXX="${CXX:-g++}"
command -v ccache >/dev/null 2>&1 && CXX="ccache $CXX"

# J / Jwait: run the compiler calls of a section at the same time and wait
# wherever an artefact is consumed. See tools/parallel.sh for why this and not
# a generated ninja file.
. "$(dirname "$0")/tools/parallel.sh"

AUDIO_FLAGS="-I$AULOS/include"
AUDIO_LIB="$AULOS/build/libaulos.a"
if [ "${1:-}" = "noaudio" ] || [ ! -f "$AULOS/build/libaulos.a" ]; then
Jwait
    echo "-- building WITHOUT audio"
    AUDIO_FLAGS="-DDAI_NO_AUDIO"
    AUDIO_LIB=""
fi

mkdir -p build

Jwait
echo "-- physics backend availability"
# Deciding this BEFORE the engine core is compiled, because dai_engine.cpp is
# what refuses DAI_PHYSICS_TALOS when the backend was not linked in.
if [ -f "${TALOS:-/root/projects/talos}/TalC/talos.h" ] && [ -f "${TALOS:-/root/projects/talos}/build/libtalos.a" ]; then
    ENGINE_DEFS=""
else
    ENGINE_DEFS="-DDAI_NO_TALOS"
fi

Jwait
echo "-- engine core (no Jolt include path - this is the leak test)"
J $CXX $FLAGS $ARCH $ENGINE_DEFS -Iinclude -Isrc -c src/dai_engine.cpp -o build/dai_engine.o

Jwait
echo "-- physics backend: null"
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/physics_null.cpp -o build/physics_null.o

Jwait
echo "-- physics backend: jolt"
J $CXX $FLAGS $ARCH $JOLT_DEFS -Iinclude -Isrc -I"$JOLT_SRC" -c src/physics_jolt.cpp -o build/physics_jolt.o

# Second real backend: Talos through its C API. Optional the same way audio is
# - without it the engine still builds and DAI_PHYSICS_TALOS is refused rather
# than silently answered with Jolt.
TALOS_OBJ=""
TALOS_LIB=""
if [ -f "$TALOS/TalC/talos.h" ] && [ -f "$TALOS/build/libtalos.a" ]; then
Jwait
    echo "-- physics backend: talos ($TALOS)"
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$TALOS/TalC" -c src/physics_talos.cpp -o build/physics_talos.o
    TALOS_OBJ=build/physics_talos.o
    TALOS_LIB="$TALOS/build/libtalos.a"
else
Jwait
    echo "-- physics backend: talos SKIPPED (no $TALOS/build/libtalos.a - run talos/build.sh)"
fi

Jwait
echo "-- audio"
J $CXX $FLAGS $ARCH $AUDIO_FLAGS -Iinclude -Isrc -c src/dai_audio.cpp -o build/dai_audio.o

Jwait
echo "-- scene layer"
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_scene.cpp -o build/dai_scene.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_input.cpp -o build/dai_input.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_editor.cpp -o build/dai_editor.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_dock.cpp -o build/dai_dock.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_project.cpp -o build/dai_project.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_material.cpp -o build/dai_material.o

# The drone show pipeline. Arithmetic on points and time: no renderer, no
# window, no physics backend - which is why it sits in the plain archive next
# to the engine and why build/test_droneshow runs on a machine with no GPU.
# The panels that drive it are a different file and live with the editor UI.
Jwait
echo "-- drone show (sampling, assignment, layering, validation, export)"
for f in dai_show dai_show_sample dai_show_assign dai_show_plan dai_show_check dai_show_export; do
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c "src/$f.cpp" -o "build/$f.o"
done

Jwait
echo "-- scene document (editor truth: stable ids, generic undo)"
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc.cpp -o build/dai_doc.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc_text.cpp -o build/dai_doc_text.o
J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_doc_sync.cpp -o build/dai_doc_sync.o

# rm FIRST. `ar rcs` REPLACES the members it is given and leaves every other
# member of an existing archive exactly where it was - so an object that was
# once listed here and later moved to libdaidalos_vk.a stayed in this archive
# forever, never recompiled, and whatever linked libdaidalos.a first got that
# fossil. Measured today: eleven ghosts, among them dai_font.o, dai_ui.o and
# dai_script.o - a font fix landed in the .o, the archive kept the old one,
# and the difference was invisible because the editor happens to link the vk
# archive first. Same failure as the stale .spv two blocks down, same fix.
rm -f build/libdaidalos.a build/libdaidalos_vk.a build/libdaidalos_assets.a
Jwait
ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o ${TALOS_OBJ} \
       build/dai_material.o \
       build/dai_audio.o build/dai_scene.o build/dai_input.o build/dai_editor.o \
       build/dai_doc.o build/dai_doc_text.o build/dai_doc_sync.o build/dai_project.o \
       build/dai_show.o build/dai_show_sample.o build/dai_show_assign.o \
       build/dai_show_plan.o build/dai_show_check.o build/dai_show_export.o

Jwait
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
Jwait
    echo "-- renderer: vulkan 1.3"
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan.cpp       -o build/rhi_vulkan.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_frame.cpp -o build/rhi_vulkan_frame.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_texture.cpp -o build/rhi_vulkan_texture.o
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
Jwait
        echo "-- window backend: X11"
        J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_window.cpp -o build/rhi_vulkan_window.o
        WINDOW_OBJ=build/rhi_vulkan_window.o
        X11_LIB="-lX11"
        ;;
    wayland)
Jwait
        echo "-- window backend: Wayland"
        gcc $ARCH -O2 -Iinclude -Isrc -c src/generated/xdg-shell-protocol.c -o build/xdg-shell-protocol.o
        J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/rhi_vulkan_window_wayland.cpp -o build/rhi_vulkan_window.o
        WINDOW_OBJ="build/rhi_vulkan_window.o build/xdg-shell-protocol.o"
        X11_LIB="-lwayland-client"
        ;;
    win32)
        # Cross compiled with mingw-w64 to prove it BUILDS; it has not been run
        # on Windows from here. Produces an object file, not a linked binary.
Jwait
        echo "-- window backend: win32 (cross compile check only)"
        mkdir -p /tmp/vkinc && cp -r /usr/include/vulkan /tmp/vkinc/ 2>/dev/null || true
Jwait
        x86_64-w64-mingw32-g++ -std=c++17 -O2 -fno-rtti -fno-exceptions \
            -Iinclude -Isrc -I/tmp/vkinc -c src/rhi_vulkan_window_win32.cpp -o build/rhi_vulkan_window_win32.o
        # The updater's Windows half (WinHTTP, the rename trick) is only
        # compiled on this path, so check it here rather than discover it on
        # the first Windows build.
Jwait
        x86_64-w64-mingw32-g++ -std=c++17 -O2 -fno-rtti -fno-exceptions \
            -Iinclude -Isrc -c src/dai_update.cpp -o build/dai_update_win32.o
        echo "   ok: build/rhi_vulkan_window_win32.o, build/dai_update_win32.o"
        ;;
    *)
Jwait
        echo "-- window backend: none (headless only)"
        ;;
    esac
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_meshgen.cpp      -o build/dai_meshgen.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_image.cpp        -o build/dai_image.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_inflate.cpp      -o build/dai_inflate.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_jpeg.cpp         -o build/dai_jpeg.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_json.cpp         -o build/dai_json.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf.cpp         -o build/dai_gltf.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf_geom.cpp    -o build/dai_gltf_geom.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_fracture.cpp     -o build/dai_fracture.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_gltf_write.cpp   -o build/dai_gltf_write.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_particles.cpp    -o build/dai_particles.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_font.cpp          -o build/dai_font.o
    # Vector icons: the SVG rasteriser and the atlas it packs. Same reasoning
    # as the TrueType loader next to it - a few hundred lines instead of a
    # dependency, and icons that are sharp at whatever size the display wants.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_svg.cpp           -o build/dai_svg.o
    # Asset thumbnails: a mesh rasterised into a 64 px icon. No GPU on
    # purpose - see include/dai_thumb.h - so it sits with the other things
    # that turn data into pixels rather than with the renderer.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_thumb.cpp         -o build/dai_thumb.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_icons.cpp         -o build/dai_icons.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_ui.cpp            -o build/dai_ui.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_update.cpp       -o build/dai_update.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_editor_ui.cpp     -o build/dai_editor_ui.o
    # The drone show panels. With the editor UI rather than with the pipeline,
    # because this is the one file that knows about both dai_show and dai_ui -
    # the same split dai_editor / dai_editor_ui exists for.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_show_ui.cpp       -o build/dai_show_ui.o
    # Native (C++) behaviours: compiles a .cpp in the project to a shared
    # library and dlopen()s it. Lives with the editor because only the editor
    # has a compiler on hand and a reason to rebuild while running.
Jwait
    python3 tools/embed_native.py include/dai_native.h build/dai_native_header.cpp
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_native.cpp        -o build/dai_native.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_tr.cpp            -o build/dai_tr.o
    # The GAME's string tables (dai_strings) - not the editor's own (dai_tr).
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c src/dai_strings.cpp       -o build/dai_strings.o
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c build/dai_native_header.cpp -o build/dai_native_header.o
    # The shaders are also linked IN - the editor runs as one file anywhere,
    # and a shaders/ dir (or DAI_SHADER_DIR) still overrides when present.
Jwait
    python3 tools/embed_shaders.py shaders build/dai_shaders_embed.cpp
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -c build/dai_shaders_embed.cpp -o build/dai_shaders_embed.o
Jwait
    ar rcs build/libdaidalos_vk.a build/rhi_vulkan.o build/rhi_vulkan_frame.o build/rhi_vulkan_texture.o \
           build/dai_shaders_embed.o \
           $WINDOW_OBJ build/dai_dock.o build/dai_meshgen.o build/dai_image.o build/dai_inflate.o build/dai_jpeg.o build/dai_json.o \
           build/dai_gltf.o build/dai_gltf_geom.o build/dai_gltf_write.o build/dai_fracture.o build/dai_particles.o build/dai_font.o build/dai_svg.o build/dai_icons.o build/dai_thumb.o build/dai_ui.o build/dai_update.o \
           build/dai_editor_ui.o build/dai_show_ui.o build/dai_native.o build/dai_native_header.o build/dai_tr.o \
           build/dai_strings.o
    VK_OK=1
else
Jwait
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
Jwait
    echo "-- assets: Mnemosyne + glTF ($MNEMOSYNE)"
    MNE_FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread"
    for f in mne_path mne_vfs mne_pack mne_registry; do
        J $CXX $MNE_FLAGS -I"$MNEMOSYNE/include" -I"$MNEMOSYNE/src" -c "$MNEMOSYNE/src/$f.cpp" -o "build/$f.o"
    done
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$MNEMOSYNE/include" -c src/dai_assets.cpp -o build/dai_assets.o
Jwait
    ar rcs build/libdaidalos_assets.a build/dai_assets.o \
           build/mne_path.o build/mne_vfs.o build/mne_pack.o build/mne_registry.o
    ASSETS_LIB="build/libdaidalos_assets.a"
else
Jwait
    echo "-- assets: skipped (no Mnemosyne at $MNEMOSYNE)"
fi

LIBS="build/libdaidalos.a $AUDIO_LIB ${TALOS_LIB:-} -L$JOLT_LIB -lJolt -lpthread -lm"
VKLIBS="build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a $AUDIO_LIB ${TALOS_LIB:-} -L$JOLT_LIB -lJolt -lvulkan ${X11_LIB:-} -lpthread -lm -ldl"

# --- backend leak tests -------------------------------------------------
# Same idea as the Jolt one, for the renderer: the RHI must be swappable for
# D3D12/Metal/an emitter into someone else's engine by replacing rhi_*.cpp.
Jwait
echo "-- leak test: no talos.h outside src/physics_talos.cpp"
if grep -l "talos\.h\|tal_world\|tal_body_id" src/*.cpp src/*.hpp include/*.h 2>/dev/null | grep -v "^src/physics_talos.cpp"; then
    echo "   !! a Talos type escaped the backend (files listed above)"; exit 1
fi
Jwait
echo "-- leak test: no Vulkan outside src/rhi_vulkan*"
if grep -l "vulkan/vulkan.h\|VkDevice\|vkCmd" src/*.cpp src/*.hpp include/*.h 2>/dev/null | grep -v "^src/rhi_vulkan"; then
    echo "   !! a Vulkan type escaped the backend (files listed above)"; exit 1
fi
Jwait
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
J $CXX $FLAGS $ARCH -Iinclude build/_norender.cpp $LIBS -o build/_norender    # note: no -lvulkan
Jwait
./build/_norender || { echo "   !! scene layer cannot run without a renderer"; exit 1; }
rm -f build/_norender build/_norender.cpp

# Scripting is optional: it is the only part that needs a vendored library
QJS=extern/quickjs
if [ -f "$QJS/libquickjs.a" ]; then
Jwait
    echo "-- scripting: quickjs"
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$QJS" -c src/dai_script.cpp -o build/dai_script.o
    SCRIPT_LIB="build/dai_script.o $QJS/libquickjs.a"
else
Jwait
    echo "-- scripting: skipped (extern/quickjs not built)"
    SCRIPT_LIB=""
fi

Jwait
echo "-- tests"
J $CXX $FLAGS $ARCH -Iinclude tests/test_daidalos.cpp $LIBS -o build/test_daidalos
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_image.cpp src/dai_inflate.cpp src/dai_jpeg.cpp -o build/test_image
J $CXX $FLAGS $ARCH -Iinclude tests/test_merge.cpp $LIBS -o build/test_merge
J $CXX $FLAGS $ARCH -Iinclude tests/test_save.cpp $LIBS -o build/test_save
J $CXX $FLAGS $ARCH -Iinclude tests/test_input.cpp $LIBS -o build/test_input
J $CXX $FLAGS $ARCH -Iinclude tests/test_editor.cpp $LIBS -o build/test_editor
# Four bugs that were all the same question asked badly - "where is this object,
# really". Run here rather than merely built: three of the four are measurements
# (penetration depth, drag distance, roll distance) that a change to the physics
# settings or the sync layer can move without breaking anything that compiles.
J $CXX $FLAGS $ARCH -Iinclude tests/test_editor_live.cpp $LIBS -o build/test_editor_live
Jwait
./build/test_editor_live
J $CXX $FLAGS $ARCH -Iinclude tests/test_doc.cpp $LIBS -o build/test_doc
J $CXX $FLAGS $ARCH -Iinclude tests/test_play.cpp $LIBS -o build/test_play
J $CXX $FLAGS $ARCH -Iinclude tests/test_cam.cpp $LIBS -o build/test_cam
if [ -n "${TALOS_LIB:-}" ]; then
    # The second real backend, held to the same claims as the first.
    J $CXX $FLAGS $ARCH -Iinclude tests/test_talos.cpp $LIBS -o build/test_talos
fi
if [ -n "$SCRIPT_LIB" ] && [ "$VK_OK" = "1" ]; then
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -Iextern/quickjs tests/test_script.cpp $SCRIPT_LIB $VKLIBS -o build/test_script
    # The object model behaviours are written against - self.transform.position.x
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -Iextern/quickjs tests/test_objmodel.cpp $SCRIPT_LIB $VKLIBS -o build/test_objmodel
    # scene.spawn()/scene.destroy() over a REAL document, through the same
    # include/dai_spawn_host.inl the editor and the shipped game include.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc -Iextern/quickjs tests/test_spawn.cpp $SCRIPT_LIB $VKLIBS -o build/test_spawn
    # And the C++ example has to keep compiling: it is documentation that runs.
    J $CXX $FLAGS $ARCH -Iinclude -shared -fPIC examples/scripts/PlayerController.cpp -o build/_playercontroller_check.so
    rm -f build/_playercontroller_check.so
    # The component classes, compiled the way the editor compiles a behaviour:
    # against dai_native.h and nothing else. This is the C++ half of the object
    # model, and a header that only compiles inside the engine is a header that
    # does not work.
    J $CXX $FLAGS $ARCH -Iinclude -shared -fPIC examples/scripts/LampFlicker.cpp -o build/_lampflicker_check.so
    rm -f build/_lampflicker_check.so
fi
# The drone show, end to end. Five sources into one binary (see
# tests/droneshow_cases.hpp for why), and RUN here rather than merely built:
# the determinism check and the scaling table are the two claims this feature
# is sold on, and a claim that is only compiled is not a claim.
#
# The full run, WITHOUT "quick": the 10,000 drone row is the one that would
# expose an O(n^2) tick or an O(n^3) assignment, it asserts its own growth
# against the 1,000 drone row, and it costs about ten seconds. A scaling
# promise that the build skips is a scaling promise nobody is keeping.
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_droneshow.cpp \
    tests/droneshow_cases_sample.cpp tests/droneshow_cases_assign.cpp \
    tests/droneshow_cases_plan.cpp tests/droneshow_cases_io.cpp tests/droneshow_cases_edit.cpp \
    $LIBS -o build/test_droneshow
Jwait
./build/test_droneshow
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_font.cpp src/dai_font.cpp -o build/test_font
# The SVG rasteriser: no renderer, no font, no window - it turns text into
# coverage, so the test reads the coverage back.
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_svg.cpp src/dai_svg.cpp src/dai_icons.cpp \
    -o build/test_svg
Jwait
./build/test_svg
# Thumbnails are arithmetic on triangles: no renderer, no window, and the
# checks read the pixels back. Run here rather than merely built.
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_thumb.cpp src/dai_thumb.cpp \
    -o build/test_thumb
Jwait
./build/test_thumb
if [ "$VK_OK" = "1" ]; then
    J $CXX $FLAGS $ARCH -Iinclude tests/test_render_visual.cpp $VKLIBS -o build/test_render_visual
    # Cheap and load bearing: dai_key must stay bit identical to the X11
    # keysyms it is defined as, or the X11 backend silently stops matching.
    J $CXX $FLAGS $ARCH -Iinclude tests/test_keys.cpp -o build/test_keys
Jwait
    ./build/test_keys
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_strings.cpp src/dai_strings.cpp -o build/test_strings
# Degrees <-> quaternion, the conversion the inspector and every level script
# share since include/dai_euler.h exists. Header only, so it costs nothing.
J $CXX $FLAGS $ARCH -Iinclude tests/test_euler.cpp -o build/test_euler
Jwait
./build/test_euler
# The HUD, measured through the draw list: no GPU, no window, real coordinates.
J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_hud.cpp src/dai_ui.cpp src/dai_font.cpp \
    src/dai_svg.cpp src/dai_icons.cpp src/dai_tr.cpp src/dai_strings.cpp \
    src/dai_editor_ui.cpp src/dai_dock.cpp src/dai_show_ui.cpp \
    src/dai_inflate.cpp src/dai_jpeg.cpp $LIBS -o build/test_hud
    # Looks at the pixels: text that covers ~100%% of its own box is boxes, not
    # glyphs, which is how a broken font binding hid for so long.
    J $CXX $FLAGS $ARCH -Iinclude tests/test_ui_text.cpp $VKLIBS -o build/test_ui_text
    J $CXX $FLAGS $ARCH -Iinclude tests/test_gltf.cpp $VKLIBS -o build/test_gltf
    # No renderer: fracture is arithmetic on triangles, so the test runs
    # anywhere, including a machine with no GPU and no display.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_fracture.cpp src/dai_fracture.cpp \
        src/dai_gltf_geom.cpp src/dai_gltf_write.cpp src/dai_json.cpp -o build/test_fracture
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_update.cpp src/dai_update.cpp \
        src/dai_json.cpp -o build/test_update
    J $CXX $FLAGS $ARCH -Iinclude tests/test_particles.cpp $VKLIBS -o build/test_particles
    J $CXX $FLAGS $ARCH -Iinclude tests/test_skinning.cpp $VKLIBS -o build/test_skinning
    J $CXX $FLAGS $ARCH -Iinclude tests/test_ui.cpp $VKLIBS -o build/test_ui
    # The world clipped into the scene window, and picking in the same pixels.
    J $CXX $FLAGS $ARCH -Iinclude tests/test_viewport.cpp $VKLIBS -o build/test_viewport
    DAI_SHADER_DIR=shaders ./build/test_viewport
    # Windows and the solid texel every rectangle in the interface is drawn
    # with. Needs no renderer: it reads the atlas and the vertices.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_ui_window.cpp src/dai_ui.cpp src/dai_font.cpp \
        src/dai_svg.cpp src/dai_icons.cpp src/dai_tr.cpp -o build/test_ui_window
    # Text fields: selection, caret, Home/End, Escape - and the resize edges.
    # No renderer: input in, vertices out.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_ui_field.cpp src/dai_ui.cpp src/dai_font.cpp \
        src/dai_svg.cpp src/dai_icons.cpp src/dai_tr.cpp -o build/test_ui_field
Jwait
    ./build/test_ui_field
    # Docked panels tile and never overlap - the property the whole layout
    # rewrite exists for.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_dock.cpp src/dai_dock.cpp src/dai_ui.cpp \
        src/dai_font.cpp src/dai_svg.cpp src/dai_icons.cpp src/dai_tr.cpp -o build/test_dock
Jwait
    ./build/test_dock
    # A folder is a project: creation, validation, settings round trip.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tests/test_project.cpp src/dai_project.cpp \
        -o build/test_project
Jwait
    ./build/test_project
    J $CXX $FLAGS $ARCH -Iinclude tests/test_editor_ui.cpp $VKLIBS -o build/test_editor_ui
    # Which files the scene can place, and which it can paint with. No window,
    # no renderer - it links the editor UI for two functions and asks them.
    J $CXX $FLAGS $ARCH -Iinclude tests/test_assetkind.cpp $VKLIBS -o build/test_assetkind
Jwait
    ./build/test_assetkind
    [ -n "${X11_LIB:-}" ] && J $CXX $FLAGS $ARCH -Iinclude tests/test_window.cpp $VKLIBS -o build/test_window
    # Two windows on one renderer: the claim that a torn off panel can be a
    # real OS window without a second render pass. Needs a display, so it is
    # built here and run by tools/run_tests.sh under Xvfb.
    [ -n "${X11_LIB:-}" ] && J $CXX $FLAGS $ARCH -Iinclude tests/test_window_two.cpp $VKLIBS -o build/test_window_two
    if [ -n "$ASSETS_LIB" ]; then
        J $CXX $FLAGS $ARCH -Iinclude -I"$MNEMOSYNE/include" tests/test_assets.cpp \
            $ASSETS_LIB $LIBS $VKLIBS -o build/test_assets
    fi
fi

Jwait
echo "-- diagnostics"
if [ "$VK_OK" = "1" ]; then
    J $CXX $FLAGS $ARCH -Iinclude tools/gizmo_shot.cpp $VKLIBS -o build/gizmo_shot
    # The fracture baker needs no renderer: it reads geometry, cuts it and
    # writes geometry. Linking the Vulkan half in would make a build tool
    # depend on a GPU driver being present.
    J $CXX $FLAGS $ARCH -Iinclude -Isrc tools/daifracture.cpp src/dai_fracture.cpp src/dai_gltf_geom.cpp \
        src/dai_gltf_write.cpp src/dai_json.cpp -o build/daifracture
    J $CXX $FLAGS $ARCH -Iinclude tools/editor_shot.cpp $VKLIBS -o build/editor_shot
    # The game-mode editor, photographed by the same tool and under the name
    # the review knows it by. The working shots go to build/editor_shots, the
    # one picture the drone show set is compared against goes next to it in
    # .gauntlet-shots - so nothing in that folder is a leftover nobody can
    # regenerate.
    mkdir -p build/editor_shots .gauntlet-shots
    DAI_SHADER_DIR=shaders ./build/editor_shot build/editor_shots 1600 900 .gauntlet-shots
    # The show panels, photographed. Next to editor_shot because it is the same
    # kind of tool, and RUN here rather than by hand: a screenshot somebody has
    # to remember to regenerate is a screenshot that is wrong by the second
    # review. It opens a real droneshow project, so the pictures also carry the
    # claim that the project type works. No display needed - the renderer draws
    # into an image and writes the PNG.
    J $CXX $FLAGS $ARCH -Iinclude tools/droneshow_shot.cpp $VKLIBS -o build/droneshow_shot
    mkdir -p .gauntlet-shots
    # All three window sizes, from one command: the plain names are the
    # default 1600x900, `narrow-` is where a panel that guesses its layout
    # collides with itself, `wide-` is the projector in the control tent.
    # A set that only one person knows the incantation for is a set nobody
    # regenerates.
    DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1600 900
    DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1100 700  narrow-
    DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1920 1080 wide-
fi

Jwait
echo "-- examples"
J $CXX $FLAGS $ARCH -Iinclude examples/hello_daidalos.cpp $LIBS -o build/hello_daidalos
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
        # The editor runs the behaviours at play: link the script runtime in.
        if [ "$ex" = "editor_demo" ] && [ -n "$SCRIPT_LIB" ]; then
            EXTRA="$EXTRA $SCRIPT_LIB"
            EXTRA_I="$EXTRA_I -DDAI_WITH_SCRIPT"
        fi
        J $CXX $FLAGS $ARCH -Iinclude $EXTRA_I "examples/$ex.cpp" $EXTRA $VKLIBS -o "build/$ex"
    done
fi

Jwait
echo "-- ok"
Jwait
ls -la build/*.a build/test_daidalos build/hello_daidalos 2>/dev/null | sed 's/^/   /'

Jwait