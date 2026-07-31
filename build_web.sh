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
em++ $FLAGS -c src/dai_input.cpp -o build/web/dai_input.o

# 1. the headless self check: same scene, same checksum as the native build
em++ $FLAGS tools/web_headless.cpp build/web/*.o \
     -s ENVIRONMENT=node,web -s ALLOW_MEMORY_GROWTH=1 \
     -o build/web/daidalos_web.js
echo "-- ok: build/web/daidalos_web.js ($(stat -c%s build/web/daidalos_web.wasm) bytes of wasm)"

# 2. the actual library for a web client: a flat C API, no main()
EXPORTS='["_dai_web_create","_dai_web_destroy","_dai_web_body","_dai_web_impulse","_dai_web_step","_dai_web_tick","_dai_web_checksum","_dai_web_transforms","_dai_web_transform_ptr","_dai_web_input","_dai_web_rollback","_dai_web_body_count","_dai_web_version","_malloc","_free"]'
em++ $FLAGS tools/web_api.cpp build/web/dai_engine.o build/web/physics_null.o \
     build/web/dai_audio.o build/web/dai_scene.o build/web/dai_particles.o build/web/dai_input.o \
     -s ENVIRONMENT=node,web -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_NAME=Daidalos \
     -s EXPORTED_FUNCTIONS="$EXPORTS" \
     -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32","UTF8ToString"]' \
     -o build/web/daidalos.js
echo "-- ok: build/web/daidalos.js ($(stat -c%s build/web/daidalos.wasm) bytes of wasm)"

# 3. TypeScript declarations, generated from the same list
python3 tools/gen_dts.py build/web/daidalos.d.ts
echo "-- ok: build/web/daidalos.d.ts"
