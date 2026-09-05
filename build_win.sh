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
#   because the default win32 thread model has no std::mutex.
#
#   ./build_win.sh   ->   build-win/*.exe
set -e

CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
MNEMOSYNE=${MNEMOSYNE:-$PWD/../mnemosyne}
TALOS=${TALOS:-$PWD/../talos}
TALOS_WIN_LIB=${TALOS_WIN_LIB:-$TALOS/build-win/libtalos.a}
OUT=build-win

FLAGS="-std=c++17 -O2 -fno-rtti -fno-exceptions -Wall -Wno-unused-function -DUNICODE -D_UNICODE -DDAI_NO_AUDIO"
ARCH="-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse"


mkdir -p "$OUT"

# QuickJS for the editor's behaviours: the same vendored sources as the Linux
# lib, cross-compiled ONCE - rebuilding someone else's library every run is
# how builds get slow.
QJS_WIN=${QJS_WIN:-extern/quickjs/libquickjs-win.a}
if [ ! -f "$QJS_WIN" ]; then
    echo "-- quickjs (windows, one-time)"
    for c in dtoa libregexp libunicode quickjs; do
        x86_64-w64-mingw32-gcc -O2 -std=c11 -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN \
            -Iextern/quickjs -c "extern/quickjs/$c.c" -o "$OUT/qjs_$c.o"
    done
    x86_64-w64-mingw32-ar rcs "$QJS_WIN" "$OUT/qjs_dtoa.o" "$OUT/qjs_libregexp.o" \
        "$OUT/qjs_libunicode.o" "$OUT/qjs_quickjs.o"
    echo "   ok: $QJS_WIN"
fi

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
      dai_gltf_write dai_fracture dai_particles dai_font dai_svg dai_icons dai_ui dai_dock dai_project dai_update \
      dai_audio physics_null"
# dai_script needs the vendored QuickJS headers; the define lets the editor
# compile its runner only when scripting is actually linked.
CORE="$CORE dai_script"
FLAGS="$FLAGS -Iextern/quickjs"
# The Talos backend needs its own include path and its own mingw built library
# (tools/build_talos_win.sh). Without one, the engine is compiled with
# -DDAI_NO_TALOS and refuses DAI_PHYSICS_TALOS instead of quietly giving out
# Jolt.
TALOS_DEFS="-DDAI_NO_TALOS"
TALOS_LINK=""
if [ -f "$TALOS_WIN_LIB" ] && [ -f "$TALOS/TalC/talos.h" ]; then
    echo "-- physics backend: talos ($TALOS_WIN_LIB)"
    $CXX $FLAGS $ARCH -Iinclude -Isrc -I"$TALOS/TalC" -c src/physics_talos.cpp -o "$OUT/physics_talos.o"
    TALOS_DEFS=""
    TALOS_LINK="$TALOS_WIN_LIB"
    EXTRA_OBJS="$OUT/physics_talos.o"
else
    echo "-- physics backend: talos SKIPPED (no $TALOS_WIN_LIB)"
    EXTRA_OBJS=""
fi
OBJS=""
for f in $CORE; do
    $CXX $FLAGS $ARCH $TALOS_DEFS -Iinclude -Isrc -I"$VKINC" -c "src/$f.cpp" -o "$OUT/$f.o"
    OBJS="$OBJS $OUT/$f.o"
done
x86_64-w64-mingw32-ar rcs "$OUT/libdaidalos.a" $OBJS $EXTRA_OBJS
echo "   ok: $OUT/libdaidalos.a"

echo "-- renderer (vulkan, win32 surface)"
VKOBJS=""
# rhi_vulkan_window.cpp IS the X11 backend; win32 is its sibling, not an addition.
    for f in rhi_vulkan rhi_vulkan_frame rhi_vulkan_texture rhi_vulkan_window_win32; do
    $CXX $FLAGS $ARCH -DVK_USE_PLATFORM_WIN32_KHR -DDAI_WINDOW_WIN32 \
        -Iinclude -Isrc -I"$VKINC" -c "src/$f.cpp" -o "$OUT/$f.o"
    VKOBJS="$VKOBJS $OUT/$f.o"
done
# The shaders are embedded too: the shipped .exe needs NOTHING beside it.
python3 tools/embed_shaders.py shaders "$OUT/dai_shaders_embed.cpp"
$CXX $FLAGS $ARCH -Iinclude -Isrc -c "$OUT/dai_shaders_embed.cpp" -o "$OUT/dai_shaders_embed.o"
VKOBJS="$VKOBJS $OUT/dai_shaders_embed.o"
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
      ${TALOS_LINK:-} -L$OUT -lvulkan-1 -lwinhttp -lgdi32 -luser32 -lshell32 \
      "$QJS_WIN" -static -static-libgcc -static-libstdc++ -lpthread"

echo "-- programs"
for src in examples/win_smoke.cpp examples/win_keytest.cpp examples/editor_demo.cpp examples/window_demo.cpp; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .cpp)
    $CXX $FLAGS $ARCH -DDAI_WITH_SCRIPT -Iinclude -Isrc -I"$VKINC" ${ASSETS:+-I$MNEMOSYNE/include} \
        "$src" $LIBS -o "$OUT/$name.exe"
    echo "   ok: $OUT/$name.exe"
done

ls -la "$OUT"/*.exe 2>/dev/null || echo "   (nothing linked)"
echo "-- ok"
