#!/bin/bash
# Cross compiles Daidalos for Windows from Linux with mingw-w64.
#
# Why cross compile rather than build on the Windows machine: the build is a
# bash script, the Windows box has no Vulkan SDK and no compiler configured for
# this, and installing 250 MB of SDK to prove a renderer works is the wrong
# trade. mingw-w64 is already here. The Windows machine then only has to *run*
# the result, which is the thing actually being tested.
#
# Two pieces make that possible without any SDK on either side:
#
#   Vulkan: Windows has no libvulkan.so to link against, it has vulkan-1.dll,
#   which the graphics driver puts in System32. An import library generated from
#   a name list (thirdparty/win/vulkan-1.def, see tools/make_vulkan_def.sh) is
#   enough to link against it. A user needs nothing installed.
#
#   Jolt: built once by tools/build_jolt_win.sh with the -posix mingw variants,
#   because the default win32 thread model has no std::mutex.
#
#   ./build_win.sh   ->   build-win/*.exe
set -e

CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
JOLT_SRC=${JOLT_SRC:-/root/projects/JoltPhysics}
JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build-win}
MNEMOSYNE=${MNEMOSYNE:-/root/projects/mnemosyne}
OUT=build-win

FLAGS="-std=c++17 -O2 -fno-rtti -fno-exceptions -Wall -Wno-unused-function -DUNICODE -D_UNICODE -DDAI_NO_AUDIO"
# EXACTLY the defines libJolt.a was built with, and the same -m flags. Jolt's
# headers change structure layout on JPH_PROFILE_ENABLED and JPH_DEBUG_RENDERER,
# so a caller compiled without them links fine and then crashes on the first
# call - which is precisely what happened here. Read them back out of the
# library's own flags.make if this ever needs checking:
#   grep -oE '\-DJPH_[A-Z0-9_]+' jolt-build-win/CMakeFiles/Jolt.dir/flags.make
JOLT_DEFS="-DJPH_DEBUG_RENDERER -DJPH_OBJECT_STREAM -DJPH_PROFILE_ENABLED \
 -DJPH_USE_AVX -DJPH_USE_AVX2 -DJPH_USE_CPU_COMPUTE -DJPH_USE_F16C -DJPH_USE_FMADD \
 -DJPH_USE_LZCNT -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 -DJPH_USE_TZCNT -DNDEBUG"
ARCH="-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse"

if [ ! -f "$JOLT_LIB/libJolt.a" ]; then
    echo "!! no Windows Jolt at $JOLT_LIB - run tools/build_jolt_win.sh first"
    exit 1
fi

mkdir -p "$OUT"

# The Vulkan import library, regenerated every time so it cannot drift from the
# calls the engine makes.
echo "-- vulkan import library"
x86_64-w64-mingw32-dlltool -d thirdparty/win/vulkan-1.def -l "$OUT/libvulkan-1.a" -D vulkan-1.dll
echo "   ok: $OUT/libvulkan-1.a"

# Vulkan headers: the Linux ones are the same headers. Only the platform define
# changes which surface extension gets declared.
VKINC=/tmp/dai_vkinc
mkdir -p "$VKINC" && cp -r /usr/include/vulkan "$VKINC/" 2>/dev/null || true
mkdir -p "$VKINC/vk_video" && cp -r /usr/include/vk_video/* "$VKINC/vk_video/" 2>/dev/null || true

# Audio is left out on purpose (-DDAI_NO_AUDIO): it needs Aulos, a separate
# project, and is not what this build is proving. physics_null is in, because
# dai_engine falls back to it and the linker wants the symbol either way.
echo "-- engine"
CORE="dai_engine dai_scene dai_input dai_doc dai_doc_text dai_doc_sync dai_editor \
      dai_editor_ui dai_meshgen dai_image dai_inflate dai_json dai_gltf dai_gltf_geom \
      dai_gltf_write dai_fracture dai_particles dai_font dai_ui dai_update \
      dai_audio physics_jolt physics_null"
OBJS=""
for f in $CORE; do
    $CXX $FLAGS $ARCH $JOLT_DEFS -Iinclude -Isrc -I"$JOLT_SRC" -I"$VKINC" -c "src/$f.cpp" -o "$OUT/$f.o"
    OBJS="$OBJS $OUT/$f.o"
done
x86_64-w64-mingw32-ar rcs "$OUT/libdaidalos.a" $OBJS
echo "   ok: $OUT/libdaidalos.a"

echo "-- renderer (vulkan, win32 surface)"
VKOBJS=""
# rhi_vulkan_window.cpp IS the X11 backend; win32 is its sibling, not an addition.
    for f in rhi_vulkan rhi_vulkan_frame rhi_vulkan_texture rhi_vulkan_window_win32; do
    $CXX $FLAGS $ARCH -DVK_USE_PLATFORM_WIN32_KHR -DDAI_WINDOW_WIN32 \
        -Iinclude -Isrc -I"$VKINC" -c "src/$f.cpp" -o "$OUT/$f.o"
    VKOBJS="$VKOBJS $OUT/$f.o"
done
x86_64-w64-mingw32-ar rcs "$OUT/libdaidalos_vk.a" $VKOBJS
echo "   ok: $OUT/libdaidalos_vk.a"

if [ -d "$MNEMOSYNE/src" ]; then
    echo "-- assets (mnemosyne)"
    MOBJS=""
    for f in "$MNEMOSYNE"/src/*.cpp; do
        n=$(basename "$f" .cpp)
        $CXX -std=c++17 -O2 -I"$MNEMOSYNE/include" -c "$f" -o "$OUT/mne_$n.o"
        MOBJS="$MOBJS $OUT/mne_$n.o"
    done
    $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$MNEMOSYNE/include" -c src/dai_assets.cpp -o "$OUT/dai_assets.o"
    x86_64-w64-mingw32-ar rcs "$OUT/libdaidalos_assets.a" "$OUT/dai_assets.o" $MOBJS
    ASSETS="$OUT/libdaidalos_assets.a"
    echo "   ok: $OUT/libdaidalos_assets.a"
else
    ASSETS=""
fi

# -static so the .exe runs on a machine with no mingw runtime beside it. The
# whole point is handing over one file.
LIBS="$OUT/libdaidalos_vk.a $OUT/libdaidalos.a $OUT/libdaidalos_vk.a $ASSETS \
      -L$JOLT_LIB -lJolt -L$OUT -lvulkan-1 -lwinhttp -lgdi32 -luser32 -lshell32 \
      -static -static-libgcc -static-libstdc++ -lpthread"

echo "-- programs"
for src in examples/win_smoke.cpp examples/win_keytest.cpp examples/editor_demo.cpp examples/window_demo.cpp; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .cpp)
    $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$VKINC" ${ASSETS:+-I$MNEMOSYNE/include} \
        "$src" $LIBS -o "$OUT/$name.exe"
    echo "   ok: $OUT/$name.exe"
done

ls -la "$OUT"/*.exe 2>/dev/null || echo "   (nothing linked)"
echo "-- ok"
