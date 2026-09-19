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
#   ./build_win.sh   ->   build-win/*.exe   and   build/last_win.log
#
# The log is written HERE, by this run, and not copied in afterwards by
# whoever remembered. RUN.md quotes the last lines of it as the proof that the
# Windows editor linked, and for two rounds that proof came out of a file that
# some earlier, hand piped run had left lying about - so the document could
# swear the exe was built from sources that had changed since. Same tee
# re-entry as tools/run_tests.sh: the script calls itself once with the log
# flag set and pipes THAT through tee, because a redirect would lose the exit
# code and a background tee can lose the last lines - which are exactly the
# lines RUN.md quotes.
set -e

if [ "${DAI_WIN_LOG:-}" != "1" ]; then
    cd "$(dirname "$0")"
    mkdir -p build
    DAI_WIN_LOG=1 "$0" "$@" 2>&1 | tee build/last_win.log
    exit "${PIPESTATUS[0]}"
fi

CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
JOLT_SRC=${JOLT_SRC:-/root/projects/JoltPhysics}
JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build-win}
MNEMOSYNE=${MNEMOSYNE:-/root/projects/mnemosyne}
TALOS=${TALOS:-/root/projects/talos}
TALOS_WIN_LIB=${TALOS_WIN_LIB:-$TALOS/build-win/libtalos.a}
OUT=build-win

AULOS=${AULOS:-/root/projects/aulos}
AUDIO_LIB_WIN=""
AUDIO_DEF="-DDAI_NO_AUDIO"
# Audio is IN when Aulos has a Windows archive: the shipped game is the one
# build where silence is not acceptable. Cross-build it once with
#   x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Iinclude -Iextern -c src/aulos.cpp
# and archive it together with miniaudio_impl.o into build-win/libaulos.a.
if [ -f "$AULOS/build-win/libaulos.a" ]; then
    AUDIO_LIB_WIN="$AULOS/build-win/libaulos.a"
    AUDIO_DEF="-I$AULOS/include"
fi
FLAGS="-std=c++17 -O2 -fno-rtti -fno-exceptions -Wall -Wno-unused-function -DUNICODE -D_UNICODE $AUDIO_DEF"
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

# Jolt is OPT-IN for Windows now (WITH_JOLT=1). Talos is the shipped backend;
# Jolt stays in the Linux build as the reference the tests compare against, but
# the editor Justin runs should neither carry it nor claim it on start-up.
WITH_JOLT=0
    JOLT_DEFS="-DDAI_NO_JOLT -DNDEBUG"
    JOLT_LINK=""


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
      dai_editor_ui dai_meshgen dai_image dai_inflate dai_jpeg dai_json dai_gltf dai_gltf_geom \
      dai_gltf_write dai_fracture dai_particles dai_font dai_svg dai_icons dai_thumb dai_ui dai_dock dai_project dai_update \
      dai_audio dai_native dai_tr dai_strings dai_material physics_null"
# The drone show pipeline and its panels. They are part of the editor, not an
# add-on: a project.daidalos whose marker says "kind droneshow" opens a panel
# set that is compiled in here or it does not open at all. dai_show_ui is the
# one file that knows about both dai_show and dai_ui, and both are in this
# same archive, so it goes in with the rest rather than beside it.
CORE="$CORE dai_show dai_show_sample dai_show_assign dai_show_plan dai_show_check \
      dai_show_export dai_show_ui"
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
    $CXX $FLAGS $ARCH $JOLT_DEFS $TALOS_DEFS -Iinclude -Isrc -I"$JOLT_SRC" -I"$VKINC" -c "src/$f.cpp" -o "$OUT/$f.o"
    OBJS="$OBJS $OUT/$f.o"
done
# See build.sh: ar rcs never REMOVES, so an archive that is not deleted first
# keeps objects nobody lists any more and hands them to the linker.
rm -f "$OUT/libdaidalos.a" "$OUT/libdaidalos_vk.a" "$OUT/libdaidalos_assets.a"
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
# The native behaviour header travels in the binary too, for the same reason:
# the editor writes it out next to whatever .cpp it is about to compile.
python3 tools/embed_native.py include/dai_native.h "$OUT/dai_native_header.cpp"
$CXX $FLAGS $ARCH -Iinclude -Isrc -c "$OUT/dai_native_header.cpp" -o "$OUT/dai_native_header.o"
VKOBJS="$VKOBJS $OUT/dai_native_header.o"
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
# libdaidalos.a is named a second time AFTER the asset library: the asset
# layer calls back into dai_gltf_*, and a static archive is only scanned
# once at the position it is written. One extra name, no extra bytes.
LIBS="$OUT/libdaidalos_vk.a $OUT/libdaidalos.a $OUT/libdaidalos_vk.a $ASSETS $OUT/libdaidalos.a \
      ${TALOS_LINK:-} $JOLT_LINK ${AUDIO_LIB_WIN:-} -L$OUT -lvulkan-1 -lwinhttp -lgdi32 -luser32 -lshell32 -lcomdlg32 \
      "$QJS_WIN" -lole32 -lwinmm -lavrt -lksuser -static -static-libgcc -static-libstdc++ -lpthread"

echo "-- programs"
# The editor carries the app icon (resource 1 = assets/daidalos.ico); the
# window class looks it up by that id. windres resolves the path relative to
# the .rc, hence the cd.
if [ -f assets/daidalos.ico ]; then
    mkdir -p "$OUT/rc"
    # Icon + version info + manifest. The metadata is not decoration: a
    # binary with no name, no publisher and no manifest is exactly the shape
    # Windows Defender's ML calls "Wacatac.B!ml" on sight, and every false
    # positive costs a user who never gets as far as the viewport.
    VER_Y=$(date -u +%Y); VER_M=$(date -u +%-m); VER_D=$(date -u +%-d)
    VER_STR=$(date -u +%Y.%m.%d)
    cat > "$OUT/rc/daidalos.manifest" <<EOF
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="$VER_STR.0" name="FleiTec.Daidalos.Editor" type="win32"/>
  <description>Daidalos Editor</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges>
      <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
    </requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{e2011457-1546-43c5-a5fe-008deee3d3f0}"/>
    <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
    <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
    <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
  </application></compatibility>
</assembly>
EOF
    cat > "$OUT/rc/daidalos.rc" <<EOF
1 ICON "../../assets/daidalos.ico"
1 24 "daidalos.manifest"
1 VERSIONINFO
FILEVERSION $VER_Y,$VER_M,$VER_D,0
PRODUCTVERSION $VER_Y,$VER_M,$VER_D,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "FleiTec"
            VALUE "FileDescription", "Daidalos Editor"
            VALUE "FileVersion", "$VER_STR"
            VALUE "InternalName", "DaidalosEditor"
            VALUE "LegalCopyright", "FleiTec"
            VALUE "OriginalFilename", "DaidalosEditor.exe"
            VALUE "ProductName", "Daidalos Engine"
            VALUE "ProductVersion", "$VER_STR"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOF
    ( cd "$OUT/rc" && x86_64-w64-mingw32-windres daidalos.rc -O coff -o daidalos.res )
    ICON_RES="$OUT/rc/daidalos.res"
fi
for src in examples/win_smoke.cpp examples/win_keytest.cpp examples/editor_demo.cpp examples/window_demo.cpp; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .cpp)
    RES=""
    GUI=""
    # -mwindows: the editor is a GUI program. The cmd window that used to sit
    # behind it was where every diagnostic went to die; stdout is piped into
    # the editor's own Console panel now.
    [ "$name" = "editor_demo" ] && { RES="${ICON_RES:-}"; GUI="-mwindows"; }
    $CXX $FLAGS $ARCH -DDAI_WITH_SCRIPT -Iinclude -Isrc -I"$VKINC" ${ASSETS:+-I$MNEMOSYNE/include} \
        "$src" $RES $LIBS $GUI -o "$OUT/$name.exe"
    echo "   ok: $OUT/$name.exe"
done

ls -la "$OUT"/*.exe 2>/dev/null || echo "   (nothing linked)"
echo "-- ok"
