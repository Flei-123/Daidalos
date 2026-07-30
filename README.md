# Daidalos

A deterministic game engine core in C++17. Physics, audio, fixed tick, rollback
netcode and a Vulkan renderer — with a hard boundary between the engine and the
libraries it uses.

```
Jolt Physics  ->  rigid bodies            (behind IPhysicsBackend)
Aulos         ->  event driven game audio (behind dai_audio.cpp)
Vulkan 1.3    ->  rendering               (behind dai_render.h)
Daidalos      ->  tick, world, snapshots, rollback, input, commands
your host     ->  Unity, SDL, your own renderer. The engine never calls back.
```

## The one rule

```
state(n+1) = step(state(n), input(n))
```

The simulation is a pure function of state and input. It never reads wall clock
time, never reads the audio system, never reads the renderer, never uses an
unseeded random number. That is what makes rollback possible, and it is the
thing that cannot be retrofitted later — Wube documented in FFF-215 that adding
multithreading to Factorio's deterministic sim afterwards would be a full
rewrite.

Everything that mutates the world is a **command stamped with a tick number**,
so a rollback can undo and replay it exactly.

## Build

```bash
./build.sh              # engine, both physics backends, tests, examples
./build.sh noaudio      # without Aulos
./build/test_daidalos   # 34 assertions
```

Dependencies: Jolt 5.6.0 (`JOLT_SRC`, `JOLT_LIB`), Aulos (`AULOS`), optional
`libvulkan-dev` + `glslang-tools` for the renderer. Nothing else.

## The physics boundary

`src/dai_physics.hpp` is the contract. It contains **no third party type** — no
`JPH::Vec3`, no `BodyID`, no Jolt header. Gameplay and engine code only ever see
`dai_vec3`, `dai_quat` and `uint32_t` slot indices.

| file | rule |
|---|---|
| `src/physics_jolt.cpp` | the only file in the engine allowed to include a Jolt header |
| `src/physics_null.cpp` | gravity + a floor, nothing else |
| `src/dai_engine.cpp` | compiled **without** the Jolt include path |

That last line is the leak test, and `build.sh` enforces it: if a Jolt header
ever sneaks into the engine core, the build fails. The null backend is the
runtime half of the same test — test `[9]` runs determinism, rollback and
raycast against it. Both halves have to keep passing.

Switch at runtime:

```c
dai_config cfg = {0};
cfg.backend = DAI_PHYSICS_NULL;   /* or DAI_PHYSICS_JOLT */
```

### What a backend swap would and would not carry over

Honest list, because the interface does not make this free:

* **Migrates:** body lifetime, forces, queries, your own compound/merge logic,
  the whole tick/snapshot/input structure.
* **Does not migrate:** tuning. Every solver has different restitution,
  penetration and sleeping behaviour. Masses, friction values, motor forces and
  spring rates all have to be redone.
* **Does not migrate:** saved solver state (warm starting impulses, sleep
  flags). `save_state` blobs are backend specific, and `restore_state` rejects
  a foreign one.
* **Does not migrate:** old replays and old multiplayer clients. A backend
  change is a hard version break.

The interface buys the *option* plus testability and a deterministic replay
harness. It does not buy a free migration.

## Usage

```c
dai_config cfg = {0};
cfg.tick_hz = 60;
cfg.seed = 1234;
cfg.audio_bank = "bank.json";
cfg.asset_root = "assets";

dai_world *w;
dai_create(&cfg, &w);
dai_set_tick_callback(w, on_tick, &game);   /* gameplay lives HERE */

/* per frame */
float alpha;
dai_advance(w, delta_seconds, &alpha);      /* runs 0..8 fixed ticks */
dai_get_transforms(w, transforms, 1024, alpha);
dai_present(w);                             /* hands queued sounds to Aulos */
```

Gameplay must live in the tick callback, not in your frame loop — the callback
is re-run for every re-simulated tick during a rollback. Anything outside it
happens once and is invisible to the replay.

## Rollback

```c
int resimulated = dai_apply_remote_input(w, player, tick, &input);
```

If the late input differs from what was predicted, the engine rewinds to that
tick, restores the physics state, replays commands and inputs, and returns how
many ticks it had to redo. Measured cost: **0.031 ms per re-simulated tick**
for 42 bodies, i.e. a 60 tick rollback costs 1.9 ms.

Sounds emitted during rolled back ticks are dropped before anything is heard —
that is why `dai_play` only queues an event and `dai_present` plays it.

## Verified, not claimed

Everything below is printed by `./build/test_daidalos`:

| # | claim | result |
|---|---|---|
| 1 | two worlds, same inputs, 300 ticks | identical checksum every tick |
| 2 | rollback without an input change | bit identical state |
| 3 | rollback with a corrected input == having had it from the start | `18dc26e12d91d9cf` both ways |
| 4 | bodies created and destroyed inside the rolled back window | body set restored exactly |
| 5 | audio cancelled by a rollback and re-emitted by the replay | counts match |
| 6 | static friction holds at 26°, slides at 30° (µ=0.5, critical 26.57°) | 0.000008 m vs 8.24 m |
| 6 | stiction 0.8/0.35 holds at 30°, slides at 40° (critical 38.66°) | 0.000009 m vs 22.7 m |
| 7 | 50 ms at 60 Hz | 3 ticks; a 10 s hitch caps at 8 |
| 8 | cost | 0.032 ms/tick, 42 bodies |
| 9 | the whole engine on the null backend | determinism + rollback + raycast pass |
| 10 | raycast and contacts on Jolt | normal (0,1,0), contacts reported |

Jolt itself was measured separately before any of this was written
(`/root/projects/jolt-friction`): identical checksums across 0, 1, 2 and 3
worker threads — two peers with different core counts do **not** desync.

## Renderer

Vulkan 1.3 with dynamic rendering: no `VkRenderPass`, no `VkFramebuffer`.
Renders offscreen and can read the frame back, so it is testable without a
display — here through Mesa lavapipe, on a real GPU through the identical code
path.

```bash
DAI_SHADER_DIR=shaders ./build/render_demo 4 /tmp
```

## Layout

```
include/daidalos.h     the C ABI. No C++, no Jolt, no Aulos.
include/dai_render.h   renderer interface
src/dai_physics.hpp    THE physics boundary
src/physics_jolt.cpp   Jolt backend  (only Jolt include in the project)
src/physics_null.cpp   null backend  (the leak test)
src/dai_engine.cpp     tick, commands, snapshots, rollback
src/dai_audio.cpp      Aulos backend (only Aulos include in the project)
src/rhi_vulkan.cpp     Vulkan 1.3 backend
```

## Known limits

* Snapshots hold a full physics blob per tick. 64 ticks × 2000 bodies is a few
  MB; for larger worlds use `StateRecorderFilter` to skip sleeping bodies.
* Contacts are reported per manifold, not per contact point, and `impulse` is
  not filled in yet.
* Cross platform determinism needs Jolt built with
  `CROSS_PLATFORM_DETERMINISTIC=ON` (~8% slower). Same binary, same machine is
  deterministic already.
* No constraints/joints in the interface yet — the next thing to add, and the
  prerequisite for bearings and pistons.

MIT.
