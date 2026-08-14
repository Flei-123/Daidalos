# Running Daidalos

Everything below was run on this checkout, in this order. Commands are relative
to the project root.

## 1. Build

```bash
./build.sh
```

One script, no cmake step, no generator. It compiles the engine core, the three
physics backends, the renderer, the drone show pipeline, ~40 test binaries, the
tools and the examples, and it RUNS the parts that are claims rather than code:
the two leak tests (no Talos header outside `src/physics_talos.cpp`, no Vulkan
symbol outside `src/rhi_vulkan*`), the drone show suite with its determinism
proof and its scaling table, and the screenshot tool.

Roughly 12 minutes from cold on 8 cores. It needs `g++` (C++17), the Vulkan
headers and `glslangValidator`; without them the renderer section is skipped and
everything else still builds. Optional, auto-detected next door: Talos
(`/root/projects/talos`), Jolt, Aulos, Mnemosyne, QuickJS.

Useful variants:

```bash
./build.sh noaudio          # without Aulos
DAI_WINDOW=wayland ./build.sh
DAI_WINDOW=win32 ./build.sh # cross compile check with mingw-w64
```

## 2. Tests

```bash
./tools/run_tests.sh        # every suite that needs no GPU, one total at the end
./tools/run_tests.sh -v     # and the output of each
```

29 suites. `test_window_two` opens two real windows on an Xvfb screen the script
starts itself, and the screenshot tool runs at the end - a picture nobody
regenerates is a picture that is wrong by the next review.

The drone show suite is worth running on its own, because it prints two things
no other suite can:

```bash
./build/test_droneshow          # 235 assertions + determinism + the scaling table
./build/test_droneshow quick    # the same, without the 10,000 drone row (~10 s faster)
```

### The scaling table, as measured

Not a claim, a run. This is what the last row of `./build/test_droneshow`
printed here, so a reader can tell a regression from a faster machine:

| when | machine |
|---|---|
| 2026-08-14 | AMD EPYC 7571, 8 cores, 12 GB, Debian 12, g++ 12.2, `-O3`, and three other builds running next to it |

| drones | sample | assign | layer | profile | validate | plan MB | check MB | ticks | wall |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 18 ms | 2 ms | 1 ms | 0.01 ms | 16 ms | 0.01 | 0.01 | 701 | 37 ms |
| 1 000 | 104 ms | 523 ms | 19 ms | 0.07 ms | 243 ms | 0.14 | 0.09 | 946 | 0.9 s |
| 10 000 | 1 142 ms | 381 ms | 1 953 ms | 1.35 ms | 22 423 ms | 1.87 | 1.08 | 8 815 | 25.9 s |

Read it with the tick column in hand: the fixture's figures grow with the
fleet, so the 10,000 drone show is also nine times LONGER than the 1,000 drone
one, and most of the wall clock difference is show length rather than fleet
size. The test therefore compares the two rows per tick - 3.1 x for ten times
the drones - and fails the build above 15 x, or above 30 s of wall clock. A
quadratic tick would put ten in that ratio on its own; a cubic assignment a
hundred.

`assign` falling from 523 ms to 381 ms between the two big rows is not a typo:
above 2,000 drones the solver switches from the exact Jonker-Volgenant to the
clustered auction, and the storyboard says so in the panel.

The memory columns are the point of the whole keyframe design: 10,000 drones
over a six minute show (8,815 ticks at 25 fps) are 1.87 MB of plan.
Materialised ticks would be 3.4 GB, which the suite prints next to it.

## 3. The editor

```bash
DAI_SHADER_DIR=shaders ./build/editor_demo
```

X11 only for now (it reads keysyms directly). On a machine with no display,
`Xvfb :77 -screen 0 1600x900x24 &` and `DISPLAY=:77` in front of it.

The Project panel picks or creates a project, and creating one asks for the
**kind**: `game` or `droneshow`. That single field is what swaps the panel set -
the same binary, the same dock, the same renderer. A project file written before
the field existed has no `kind` line and opens as a game, unchanged.

## 4. A drone show without the editor

The screenshot tool is also the shortest end-to-end run of the show pipeline:
it creates a game project and a droneshow project side by side, re-opens both
from disk, samples three figures, solves, validates, exports `.skyc`, `.csv` and
`.json`, and writes eight PNGs of the panels.

```bash
mkdir -p .gauntlet-shots
DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1600 900
```

It is idempotent: run it twice and it opens the projects it made the first time.
The projects land in `build/shot_projects/`, the exports in that project's
`assets/`.

## 5. The other examples

```bash
DAI_SHADER_DIR=shaders ./build/sandbox_demo 6 /tmp     # general scene
DAI_SHADER_DIR=shaders ./build/vehicle_demo  6 /tmp    # machine built from joints
DAI_SHADER_DIR=shaders ./build/particles_demo 6 /tmp
DAI_SHADER_DIR=shaders ./build/model_viewer assets/test
./build/hello_daidalos                                 # no renderer at all
```

## If something fails

* **`glslangValidator: not found`** - the renderer, the editor, the show panels
  and the screenshot tool are skipped; the engine, the pipeline and the headless
  suites still build and run.
* **`error: XDG_RUNTIME_DIR is invalid or not set`** - printed by the Vulkan
  loader, harmless: the renderer draws offscreen and never asks for a
  compositor.
* **`cannot find -lJolt`** - only the reference backend. `WITH_JOLT=` unset or
  `JOLT_LIB=` pointing elsewhere builds without it.
* **the build ends in `ld terminated with signal 15`** - the build was killed
  from outside (a timeout, usually). It is not a link error; run it again with
  more time.
