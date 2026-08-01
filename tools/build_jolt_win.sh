#!/bin/bash
# Cross compiles Jolt for Windows with mingw-w64.
#
# Two things bite here and both cost an hour if you meet them by surprise:
#
#   The default mingw on Debian uses the win32 thread model, which has no
#   std::mutex and no std::thread - Jolt does not compile at all. The -posix
#   variants of the same compiler have them.
#
#   Jolt turns its DX12 compute backend on for Windows targets, and that needs
#   dxcapi.h from the DirectX Shader Compiler - which mingw does not ship and
#   the engine does not use. JPH_USE_DX12=OFF, and the last 1% of the build
#   stops failing.
#
#   The build settings must match the Linux build (Release, no extra flags), or
#   the physics diverges between the two platforms and a deterministic engine
#   stops being one.
#
#   tools/build_jolt_win.sh   ->   /root/projects/jolt-build-win/libJolt.a
set -e

JOLT_SRC=${JOLT_SRC:-/root/projects/JoltPhysics}
OUT=${OUT:-/root/projects/jolt-build-win}

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"

cat > toolchain.cmake <<'TC'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# -posix, not the default: the win32 thread model has no std::mutex.
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TC

cmake "$JOLT_SRC/Build" \
    -DCMAKE_TOOLCHAIN_FILE="$OUT/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTARGET_UNIT_TESTS=OFF -DTARGET_HELLO_WORLD=OFF \
    -DTARGET_PERFORMANCE_TEST=OFF -DTARGET_SAMPLES=OFF -DTARGET_VIEWER=OFF \
    -DJPH_USE_DX12=OFF \
    > configure.log 2>&1

make -j"$(nproc)" > make.log 2>&1 || {
    echo "build failed:"
    grep -E "error:" make.log | head -5
    exit 1
}

find . -name libJolt.a -exec ls -la {} \;
