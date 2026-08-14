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

30 suites plus the two screenshot tools. The run this file was written from -
the tree this checkout is at:

```
TOTAL 2178 passed, 0 failed
all green
```

`test_window_two` opens two real windows on an Xvfb screen the script starts
itself, and the screenshot tools run at the end - all three drone show sets and
the game mode picture - because a picture nobody regenerates is a picture that
is wrong by the next review.

`test_editor_ui` is the thirtieth suite. It was on the "needs a display" list
without needing one, and while nobody ran it the hierarchy grew a search box
above its tree and nine of its checks had been clicking one row too high. It is
in the list now, at 72 checks, and it carries the proof that the translate gizmo
lands on the object it moves.

Two more suites live outside `run_tests.sh` and are honest about it:
`tools/build_runtime.sh` builds the packer and the standalone runtime and runs
`tests/test_vfs.cpp` on the way (`ok: 89 checks, 0 failures`, measured here),
and `build_web.sh` is the emscripten path. What is in `tests/` but in NO build
script - `test_anim.cpp`, `test_audio.cpp`, `test_drift.cpp`,
`test_shadow_contact.cpp`, `probe_spherebox.cpp`, `probe_settings.cpp` - are
older probes; five of them still compile, `probe_settings.cpp` does not. None of
them is counted in the total above, and this line is here so nobody counts them.

The drone show suite is worth running on its own, because it prints three things
no other suite can - the flight proof, the determinism proof and the scaling
table:

```bash
./build/test_droneshow          # 332 checks + determinism + the scaling table
./build/test_droneshow quick    # the same, without the 10,000 drone row (~8 s faster)
```

It ends in `ok: 332 checks, 0 failures` (tree at `3f786a8`), and on the way it
prints the audit of the show the screenshots are
taken of, read back out of the exported `.skyc` and
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

| when | commit | machine |
|---|---|---|
| 2026-08-14 | `3f786a8` | AMD EPYC 7571, 8 cores, 12 GB, Debian 12, g++ 12.2, `-O3`, and other builds running next to it |

| drones | sample | assign | layer | profile | validate | plan MB | check MB | ticks | wall |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 18.38 ms | 2.31 ms | 1.40 ms | 0.01 ms | 12.05 ms | 0.01 | 0.02 | 701 | 34 ms |
| 1 000 | 104.04 ms | 484.93 ms | 14.49 ms | 0.08 ms | 189.58 ms | 0.14 | 0.21 | 946 | 793 ms |
| 10 000 | 1 288.96 ms | 270.74 ms | 2 419.42 ms | 1.55 ms | 4 784.70 ms | 1.87 | 2.44 | 2 437 | 8 767 ms |

Read it with the tick column in hand: the fixture's figures grow with the fleet,
so the 10,000 drone show is also longer than the 1,000 drone one, and part of
the wall clock difference is show length rather than fleet size. The test
therefore compares the two rows PER TICK - (8767/2437) / (793/946) = **4.29 x**
for ten times the drones, on the tree at `3f786a8` - and fails the build above
15 x, or above 30 s of wall clock for the big row (`tests/test_droneshow.cpp`,
the two CHECKs after the table). A quadratic tick would put ten in that ratio on
its own; a cubic assignment a hundred. The same table measured on an idle
machine came out at 3.79 x (8243 ms for the big row instead of 8767);
the spread between those two numbers is three builds sharing eight cores, and it
is the reason the bound is three times the measurement rather than a whisker
above it.

`assign` falling from 485 ms to 271 ms between the two big rows is not a typo:
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
DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1100 700  narrow-
DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots 1920 1080 wide-
```

All three run inside `./build.sh`, so `.gauntlet-shots/` is a product of the
build and not of somebody's shell history. The fourth argument is the SET: with
no dash it is a prefix (`narrow-03-viewport-conflict.png`), with a leading dash
a suffix, and left off the pictures keep the plain names. The eight per set are
`01-start`, `02-transition`, `03-viewport-conflict`, `04-storyboard`,
`05-parameters`, `06-validation`, `07-preview-full`, `08-conflict-clicked`.

It is idempotent: run it twice and it opens the projects it made the first time.
The projects land in `build/shot_projects/`, the exports in that project's
`assets/`. It prints what it photographed, including the conflict it clicked:

```
solve 1 formation fault: drones 172 and 173 stand 0.40 m apart in 'Sphere (near miss)' - the floor is 2.00 m: assign 99.2 ms, layer 1119.4 ms, validate 77.9 ms, 1 conflicts
  conflict  0  t   80.60s  drones 172+216  kind 0  0.400 of 2.000 m
mesh assets/test/blender_scene.glb 2004 triangles -> selected in the storyboard
clicked validation row at 120,747 -> conflict 0, t=80.60, drone 172
```

The solve does not say "ok", and that is the fixture working as intended: the
fourth figure has two points parked 0.4 m apart, which no transition can undo,
so `dai_show_layer` answers `DAI_SHOW_LAYER_FORMATION_FAULT` instead of
`DAI_OK` and `dai_show_solve` passes it on with the pair, the gap and the figure
it stands in. Every transition in that show IS separated - `0 left over` in the
panel - and reporting the show as solved on the strength of that counter was
exactly the hole this answer closes. Drone 173 is the point that was moved;
172+216 is the pair the validator then finds standing together at 80.60 s.

The narrow run exists because 1100x700 is where labels collide if a panel lays
itself out by guessing; there the fields wrap instead, the storyboard's second
button says "From mesh" rather than being cut in half, and the conflict label in
the preview folds out to the edge of the view with a leader line back to the
ring - below 1200 px there is no room for it beside a marker in the middle of a
figure. `.gauntlet-shots/` holds the three sets: the plain names are 1600x900
and are the ones `tools/run_tests.sh` refreshes on every run.

The tool also imports `assets/test/blender_scene.glb` into the project, which is
why the Project panel lists a `figure.glb` next to the exports and why the
storyboard in the pictures offers "From selected mesh" instead of "no mesh
selected": the state that was photographed is one a director can act in.

Panels 04-07 are photographed at the size the DOCK gives them, with the tab head
above them - a column at column width, Validation as the bottom strip. A panel
stretched over the whole frame is a picture of a background with one button in
it, which says nothing about the panel anyone actually uses.

The ninth picture in the folder, `09-game-mode-editor.png`, is the same editor
in GAME mode - the point of the whole feature being one binary with two panel
sets. It comes from the editor's own screenshot tool, which takes the gauntlet
folder as a fifth argument and writes that one name into it; its working shots
(`editor_edit`, `editor_hover`, `editor_rotate`, `editor_playing`,
`editor_flythrough`, `editor_focus`) stay in `build/editor_shots`:

```bash
DAI_SHADER_DIR=shaders ./build/editor_shot build/editor_shots 1600 900 .gauntlet-shots
```

`./build.sh` and `./tools/run_tests.sh` both run it, so every file under
`.gauntlet-shots/` comes out of a documented command and none of them is a
leftover somebody made by hand. What that picture is worth looking at for: the
translate gizmo stands ON the selected body. The world is drawn into the Scene
panel's rect, with that rect's aspect, so a host that hands the editor the frame
size instead of the panel rect draws the handles next to the object - 92 px out
at 1600x900, which is what this picture showed before.
`tests/test_editor_ui.cpp` now projects both the gizmo's anchor and the ink it
actually draws against the object's centre, computed the way
`src/rhi_vulkan_frame.cpp` computes it, and fails above 5 px:

```
  gizmo anchor 0.0 px, drawn centre 2.3 px from the object centre
```

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
