# Drone show mode - the build plan

A second project type inside the existing editor. No fork, no second binary,
no change to the engine, the renderer or the dock system. The whole feature is
a new pipeline library (`dai_show`), a new panel set (`dai_show_ui`), one new
field in the project marker, and nine new settings keys.

The rule the whole thing follows is Daidalos' own, one domain over:

```
plan = solve(storyboard, settings, seed)
```

Nothing in the solve path reads the clock or an unseeded random number. If the
tool says "collision free", it says so reproducibly - on the director's
workstation, on the operator's laptop, and in the accident report.

## What already exists (written before the modules start)

| file | what it is |
|---|---|
| `include/dai_show.h` | **the contract.** Every type, every function, every guarantee. Nobody edits it. |
| `tests/droneshow_cases.hpp` | the test harness: `CHECK`, the shared fixtures (`show_test_mesh`, `show_grid_formation`), the four suite entry points. Nobody edits it. |
| `tests/test_droneshow.cpp` | `main`, the fixtures, the **determinism proof** and the **scaling table**. Nobody edits it. |
| `build.sh` | already compiles all six `src/dai_show*.cpp`, `src/dai_show_ui.cpp`, and builds **and runs** `build/test_droneshow`. Nobody edits it. |
| `tools/run_tests.sh` | `test_droneshow` is in the headless suite list. |
| the six `src/dai_show*.cpp` and four `tests/droneshow_cases_*.cpp` | exist with their file-level comment and the checklist of what belongs in them. Empty of code on purpose - no stubs to forget to remove. |

Until every module has landed, `build.sh` fails at the link step of
`test_droneshow`. That is intended: an honest missing symbol beats a stub that
returns success.

## Ownership - no two modules touch the same file

| module | files it owns, and only these |
|---|---|
| **project** | `include/dai_project.h`, `src/dai_project.cpp`, `tests/test_project.cpp` |
| **sampling** | `src/dai_show_sample.cpp`, `tests/droneshow_cases_sample.cpp` |
| **assign** | `src/dai_show_assign.cpp`, `tests/droneshow_cases_assign.cpp` |
| **plan** | `src/dai_show.cpp`, `src/dai_show_plan.cpp`, `tests/droneshow_cases_plan.cpp` |
| **io** | `src/dai_show_check.cpp`, `src/dai_show_export.cpp`, `tests/droneshow_cases_io.cpp` |
| **ui** | `include/dai_show_ui.h`, `src/dai_show_ui.cpp`, `include/dai_editor_ui.h`, `src/dai_editor_ui.cpp`, `examples/editor_demo.cpp`, `README.md` |

Shared headers are read-only for everyone. If a module believes `dai_show.h` is
wrong, it says so in its report rather than editing it - a contract that one
side changes is not a contract.

## The interfaces, in one paragraph each

**project -> everyone.** `project.daidalos` gains `kind game|droneshow`; a
missing line means `game`, which is how every existing project keeps opening.
`dai_project_create` takes the kind as a parameter. `dai_project_settings`
gains the nine droneshow fields, written only when they differ from the
default, so a game project's `settings/project.txt` does not grow a line.
`dai_project_kind(p)` is how the editor decides which panel set to register.

**sampling -> plan, ui.** `dai_show_sample_feasible` and `dai_show_sample`.
Input is a triangle soup as plain float arrays - exactly what
`dai_gltf_read_geometry` hands back - so nothing about the renderer or the
asset layer reaches this file. Output is `dai_show_point[count]` with the
pairwise minimum distance held.

**assign -> plan, ui.** `dai_show_assign` fills a permutation and a stats
struct that names the method that ran and what it cost against the optimum.
`dai_show_assign_brute_force` exists so the test can prove the solver rather
than agree with it.

**plan -> everyone.** Owns `dai_show` (the document) and `dai_show_plan` (the
keyframe container). Every other module reads a plan only through
`dai_show_plan_sample`, `dai_show_plan_key_at` and the counts - there is no
array of ticks, and there will not be one. `dai_show_solve` calls sampling,
assign and layering and is the only place a clock is read.

**io -> ui.** `dai_show_validate` turns a plan plus settings into a sorted
conflict list; the exporters turn a plan into `.skyc`, CSV and JSON, and the
JSON and `.skyc` importers make the round trip provable.

**ui -> the user.** Registers `Storyboard`, `Show Parameters` and `Validation`
in the existing dock when the open project's kind is `droneshow`, draws the
drones into the Scene panel through the existing renderer as coloured
instances, and marks conflicts red with a line between the pair.

## Sequencing

The four pipeline modules and the project module are independent and start at
once. `ui` is written against the header like the rest and integrates last; it
is the only module that may be blocked, and only on `plan`'s document API.

## The bar, restated

* `./build.sh` green, the architecture rules intact (no backend header in the
  engine core, no Vulkan symbol outside `src/rhi_vulkan*`), old tests still green.
* `build/test_droneshow` runs, with the assertions listed at the top of each
  `tests/droneshow_cases_*.cpp` - not a subset of them.
* Determinism proved by memcmp, including on the parallel paths.
* Sampling, assignment, layering and validation timed at 100 / 1,000 / 10,000
  drones and printed. No naive `O(n^2)` per tick, no `O(n^3)` assignment in
  the 10,000 path - the scaling test asserts the broadphase was really used.
* A droneshow project can be created and opened; a game project with no `kind`
  line opens exactly as before.
* The panels are on the screenshots, populated, nothing overlapping or clipped.
* A reviewer cannot tell a second author was here.
