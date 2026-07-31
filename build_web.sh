#!/usr/bin/env bash
# WebAssembly build of the simulation core.
#
# The renderer is not part of this: Vulkan has no web equivalent, so a browser
# build needs a WebGPU backend behind dai_render.h - which is exactly the swap
# the RHI boundary was built for. What DOES port unchanged is the part that is
# hard to port: the deterministic tick, snapshots, rollback, the input ring and
# the scene layer.
set -euo pipefail
cd "$(dirname "$0")"
source /root/emsdk/emsdk_env.sh >/dev/null 2>&1

mkdir -p build/web
FLAGS="-std=c++17 -O2 -fno-rtti -fno-exceptions -DDAI_NO_AUDIO -DDAI_NO_JOLT -Iinclude -Isrc"

em++ $FLAGS -c src/dai_engine.cpp   -o build/web/dai_engine.o
em++ $FLAGS -c src/physics_null.cpp -o build/web/physics_null.o
em++ $FLAGS -c src/dai_audio.cpp    -o build/web/dai_audio.o
em++ $FLAGS -c src/dai_scene.cpp    -o build/web/dai_scene.o
em++ $FLAGS -c src/dai_particles.cpp -o build/web/dai_particles.o
em++ $FLAGS tools/web_headless.cpp build/web/*.o \
     -s ENVIRONMENT=node,web -s ALLOW_MEMORY_GROWTH=1 \
     -o build/web/daidalos_web.js
echo "-- ok: build/web/daidalos_web.js ($(stat -c%s build/web/daidalos_web.wasm) bytes of wasm)"
