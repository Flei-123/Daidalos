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
