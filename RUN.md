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

Roughly 6 minutes from cold on 8 cores (`rm -rf build && ./build.sh`, measured
here), and it ends in `-- ok`. It stops at the first failure: every "compile,
then run" step is two statements rather than `g++ ... && ./binary`, because
under `set -e` the left side of an AND-list is exempt from errexit - which is
how this checkout once finished green while running a test binary from the day
before. It needs `g++` (C++17), the Vulkan
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

29 suites plus the screenshot tool. The run this file was written from:

```
TOTAL 2095 passed, 0 failed
all green
```

`test_window_two` opens two real windows on an Xvfb screen the script starts
itself, and the screenshot tool runs at the end - a picture nobody regenerates
is a picture that is wrong by the next review.

The drone show suite is worth running on its own, because it prints three things
no other suite can - the flight proof, the determinism proof and the scaling
table:

```bash
./build/test_droneshow          # 321 checks + determinism + the scaling table
./build/test_droneshow quick    # the same, without the 10,000 drone row (~8 s faster)
```

It ends in `ok: 321 checks, 0 failures`, and on the way it prints the audit of
the show the screenshots are taken of, read back out of the exported `.skyc` and
flown at 40 Hz between the exported frames:

```
export - the .skyc, read back and flown, is flyable
  flown from the file: 0.400 m closest, 8.000 m/s fastest, 2.501 m/s2 hardest, over 85.6 s at 10 Hz x 4
```

0.400 m is the fault the fixture PLANTS on purpose - two points parked 0.4 m
apart inside one formation, which no transition can undo (see `[3i]`). The
assertion is that this is the only pair in 420 that ever goes below the 2 m
minimum anywhere on the timeline, that the flight never makes it worse than the
formation already is, and that nothing exceeds 8 m/s or 4 m/s². A fixture
without that planted fault would prove that the audit runs, not that it
measures.

### The scaling table, as measured

Not a claim, a run. This is what `./build/test_droneshow` printed here on the
run this file documents, so a reader can tell a regression from a faster
machine:

| when | machine |
|---|---|
| 2026-08-14 | AMD EPYC 7571, 8 cores, 12 GB, Debian 12, g++ 12.2, `-O3`, and other builds running next to it |

| drones | sample | assign | layer | profile | validate | plan MB | check MB | ticks | wall |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 18.19 ms | 2.30 ms | 1.40 ms | 0.01 ms | 12.10 ms | 0.01 | 0.02 | 701 | 34 ms |
| 1 000 | 110.27 ms | 525.64 ms | 15.61 ms | 0.09 ms | 193.40 ms | 0.14 | 0.21 | 946 | 845 ms |
| 10 000 | 1 258.32 ms | 393.04 ms | 2 248.98 ms | 1.20 ms | 4 338.86 ms | 1.87 | 2.44 | 2 437 | 8 243 ms |

Read it with the tick column in hand: the fixture's figures grow with the fleet,
so the 10,000 drone show is also longer than the 1,000 drone one, and part of
the wall clock difference is show length rather than fleet size. The test
therefore compares the two rows PER TICK - (8243/2437) / (845/946) = **3.79 x**
for ten times the drones - and fails the build above 15 x, or above 30 s of wall
clock for the big row. A quadratic tick would put ten in that ratio on its own;
a cubic assignment a hundred.

`assign` falling from 525 ms to 393 ms between the two big rows is not a typo:
above 2,000 drones the solver switches from the exact Jonker-Volgenant to the
clustered auction, and the storyboard says so in the panel.

The memory columns are the point of the whole keyframe design: 10,000 drones
over 2,437 ticks are 1.87 MB of plan. Materialised ticks would be 3 433 MB,
which the suite prints next to it:

```
[3f] the plan is keyframes, and it stays keyframes
  plan for 10000 drones x 8 keys: 1.87 MB (materialised ticks: 3433 MB)
```

Both numbers move with the machine. The two things that must not move are the
determinism proof (`determinism - the same input, twice, byte for byte`, a
`memcmp`, not an epsilon) and the zero in the failure count.

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
# the same show at two other window sizes, into names of their own
DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1100 700  -narrow-1100x700
DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1920 1080 -large-1920x1080
```

It is idempotent: run it twice and it opens the projects it made the first time.
The projects land in `build/shot_projects/`, the exports in that project's
`assets/`. It prints what it photographed, including the conflict it clicked:

```
solve ok: assign 103.8 ms, layer 1140.9 ms, validate 77.6 ms, 1 conflicts
  conflict  0  t   80.60s  drones 172+216  kind 0  0.400 of 2.000 m
clicked validation row at 120,747 -> conflict 0, t=80.60, drone 172
```

The narrow run exists because 1100x700 is where labels collide if a panel lays
itself out by guessing; there the fields wrap instead. `.gauntlet-shots/` holds
the three sets: the plain names are 1600x900 and are the ones
`tools/run_tests.sh` refreshes on every run.

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
