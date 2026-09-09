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

---

# Round 4 - finishing the show that is already there

Nothing below restructures anything. The pipeline, the panels, the exporters
and the twenty-nine other suites stand and stay standing; this round closes the
two open failures in `build/test_droneshow` and then hardens the four points of
the original brief that measurement - not opinion - shows to be weak.

`build.sh` is not edited by anybody. No new file appears under `src/` or
`tests/`, because a new file would need a new line in the build script, and the
build script is the one entry point.

## Where the checkout really stands (measured 2026-08-14, this commit)

* `./build.sh` - green. `./tools/run_tests.sh` - **2018 passed, 2 failed**, and
  the only red suite is `test_droneshow` with exactly the two known failures.
  No other regression exists to chase.
* `./build/test_droneshow` - `FAILED: 244 checks, 2 failures`, both in
  `[3g] the sampled show`.
* `./build/droneshow_shot` - runs, writes eleven PNGs, and the show in them
  reports **18 conflicts** (`.gauntlet-shots/06-validation.png`): near misses
  down to 0.38 m and seven `accelerates too hard` entries at up to 16 m/s2
  against a 4 m/s2 limit. The show on the screenshots does not fly either, and
  that is a second, larger instance of the same bug - not a separate feature.

## The two failures, diagnosed - not guessed

Reproduced with a cut-down copy of the suite (only `[3g]` and `[3h]`) plus
`src/dai_show_plan.cpp` rebuilt with `-DDAI_SHOW_PLAN_DEBUG`. The numbers below
are that run's output, so the modules start from facts:

```
OPEN 72/220  src_gap=7.438 dst_gap=2.005
             legA[t=4.00..33.16 rise=0.00]  legB[t=4.00..33.16 rise=0.00]  dur=50.39
OPEN 172/216 src_gap=3.155 dst_gap=0.400
             legA[t=68.42..108.42 rise=0.00] legB[t=68.42..108.42 rise=0.34 y=125.0]
DBG conflict 72+220   t 30.800  value 1.999675  limit 2.000000
DBG conflict 172+216  t 108.300 value 0.399920  limit 2.000000
DBG transition 1 cross 12 height 6 delay 0 unresolved 1
DBG transition 2 cross 12 height 5 delay 1 unresolved 0
DBG transition 3 cross  1 height 1 delay 0 unresolved 1
```

**FAIL 1 - "0.000 m inside the limit" is not a comparison bug.** Pair 72/220 is
*already in the separator's own open list*. Both legs are straight, in one
window, with one profile, so `min_separation` takes its exact branch
(`dist_origin_segment`) and the answer it gets is genuinely below
`min_d - SEP_EPS`: the two drones converge from 7.438 m to 2.005 m and graze at
1.999675 m on the way, 0.325 mm inside the floor - twenty times `SEP_EPS`, so
no epsilon anywhere makes this legal, and none may be widened to make it go
away. The separator sees the pair, tries its routes, fails to place either
drone, and hands the pair back honestly as `unresolved`. The bug is the RESCUE
giving up on a 0.3 mm deficit, not the measurement finding it.

**FAIL 2 - one of the two unresolved pairs is not a transition fault at all.**
172/216 has `dst_gap = 0.400`: its two destination points are the deliberately
planted 0.4 m fault of the fixture. No route, no delay and no stretch can
separate a pair that the *formation* puts 0.4 m apart - the transition is
being blamed for a fault the validator already reports at 108.30 s, in the
formation where it was planted. The other unresolved pair, 72/220, is FAIL 1
and has to be genuinely solved.

So the correct answer is two different things, and both must be true at the
end:

1. an endpoint that is itself in conflict is a FORMATION fault. The separator
   must name it as such (its own counter), must still guarantee the pair never
   comes closer *in transit* than its own endpoints already are, and must not
   count it as a transition it failed to separate;
2. every pair whose endpoints are legal - 72/220 above - must actually be
   separated, by lifting, delaying, or stretching that leg, until the exported
   trajectory clears `min_distance` over the whole timeline.

Forbidden, and a reviewer will check `git log -p` for exactly this: raising
`SEP_EPS`, lowering `min_distance`, spacing the sampler further apart to buy
slack, deleting or softening an assertion, dropping a conflict into a "known"
list, or shortening the fixture show.

## Modules - no two touch the same file

| module | files it owns, and only these |
|---|---|
| **separator** | `src/dai_show_plan.cpp`, `src/dai_show.cpp`, `src/dai_show_internal.hpp`, `include/dai_show.h` (additive only), `tests/droneshow_cases_plan.cpp`, `_dev_*.cpp` at the root |
| **validate** | `src/dai_show_check.cpp`, `src/dai_show_export.cpp`, `tests/droneshow_cases_io.cpp` |
| **sampling** | `src/dai_show_sample.cpp`, `tests/droneshow_cases_sample.cpp` |
| **assign** | `src/dai_show_assign.cpp`, `tests/droneshow_cases_assign.cpp` |
| **ui** | `include/dai_show_ui.h`, `src/dai_show_ui.cpp`, `include/dai_editor_ui.h`, `src/dai_editor_ui.cpp`, `examples/editor_demo.cpp`, `tools/droneshow_shot.cpp`, `.gauntlet-shots/` |
| **docs** | `RUN.md`, `README.md`, `PLAN.md` |

`build.sh`, `tools/run_tests.sh`, `tests/test_droneshow.cpp` and
`tests/droneshow_cases.hpp` are read-only for every module: they hold the
determinism proof, the scaling table and the harness, and a proof that its own
author may edit is not a proof.

## The interfaces, exactly

**separator -> everyone.** `include/dai_show.h` gains exactly one field,
appended at the end of `dai_show_layer_stats`:

```c
uint32_t endpoint_pairs;   /* pairs whose own endpoints are closer than
                              min_distance: a formation fault, reported by the
                              validator at that formation, not a transition the
                              separator failed to solve. */
```

Nothing else in that header moves, is renamed or is reordered - `ui` and
`validate` compile against it unchanged. `unresolved` keeps its meaning for
every pair that is not an endpoint pair, and `dai_show_layer` still returns
`DAI_ERR_STATE` when one of those is left over. The single tolerance
`daishow::SEP_EPS` stays in `src/dai_show_internal.hpp` and stays 1e-4; no
module defines a second one.

**separator <-> validate.** One contract, in one sentence: *the separator
proves its claim about the polyline the plan actually stores, and the validator
measures that same polyline.* The separator therefore judges a route with
`daishow::leg_key_count` keys and the chord reserve, never with a curve the
export does not carry; the validator keeps its swept sub-tick refinement and
stays the ground truth. If one side needs the other to change, it says so in
its report - it does not edit the other's file.

**sampling -> everyone.** The contract does not move: `dai_show_sample`
guarantees a pairwise minimum of `min_distance_m` for a FIXED `count`, and the
separator must cope with a formation packed to exactly that. Spacing the points
further apart to make `[3g]` pass is the diluted-fixture trick and is out of
bounds. What sampling owes this round is the silhouette weighting, colour from
vertex colour or texture at the sample point, and the bitwise determinism proof
for a fixed seed.

**ui -> the user.** Public `dai_show.h` API only. The screenshot tool's show is
the same pipeline the panels drive, so when the separator lands, `ui` re-runs
`droneshow_shot` and the eleven PNGs must show one conflict - the planted one -
instead of eighteen.

**docs -> the reader.** Every number in `RUN.md` is a line pasted out of a run
made after all other modules landed. Nothing in `docs` is written before its
measurement exists.

## Sequencing

`separator` starts alone and is the critical path. `validate`, `sampling` and
`assign` start at once beside it - none of them needs the new field to compile.
`ui` works on layout, framing and the conflict list immediately and re-shoots
the screenshots after `separator` lands. `docs` runs last, on a tree that is
already green.

## The bar for this round

1. `./build.sh` green.
2. `./build/test_droneshow` - 0 failures, with the assertions of `[3g]` intact,
   the fixture untouched and `SEP_EPS` unchanged.
3. `./tools/run_tests.sh` - 2018 + N passed, 0 failed.
4. The determinism proof green: two runs, `memcmp` equal.
5. The scaling table runs to 10,000 drones, tick ratio < 15x, wall < 30 s.
6. The exported `.skyc` of the fixture is flyable over the whole timeline, not
   only at keyframes - proven in `tests/droneshow_cases_io.cpp`.
7. `.gauntlet-shots/` shows a usable panel set and a show with one conflict.
8. `RUN.md` matches this checkout, number for number.

---

# Round 5 - Blockout / CSG / DoorSocket, finished

One module of the modelling round, brought from "wip" to measured. Nothing is
rebuilt beside what is there: the seams of `include/dai_ext.h` stay, the file
list in docs/BLOCKOUT.md stays, `build.sh` / `build_win.sh` are not touched.
Three builders, no two on one file.

## Where the checkout really stands (measured 2026-09-08, commit e6902bf + this skeleton)

* `./build.sh` green, `./tools/run_tests.sh`: **TOTAL 3000 passed, 0 failed,
  all green** (build/last_run.log).
* All seams are wired: host (`include/dai_blockout_host.inl`), props table,
  inspector, Add Component, socket gizmo, `test_doc` runs `test_blockout()`,
  `test_fracture` runs `test_blockout_gltf()`. The five generators pass
  volume, edge and winding checks. The document round trip, undo/redo of the
  fields and the old-scene compatibility are proved.
* A probe against the bar (`tests/blockout_csg_cases.hpp`, written now and
  wired into `tests/test_doc.cpp`) finds the real gaps - these are facts, not
  guesses:

```
stairs 2x3x1.5, 5 steps      degenerate triangles = 8
wall 4x3x0.2 - door 1x2x0.5  vol 2.000000 open 0 nonmanifold 0  degenerate = 4
union of two boxes           vol 4.050000 open 0 nonmanifold 0  degenerate = 2
wall - arch ring             SEGFAULT: Bsp::build recurses ~21000 deep (stack overflow)
```

  Diagnosed, with the stack trace in hand:

  1. **degenerate triangles** come from `finalise()`: `repair_t_junctions`
     inserts a vertex into an edge, and the fan triangulation from vertex 0
     then emits a zero area triangle whenever vertex 0 and two inserted
     points lie on one line. Today those zero area triangles are also what
     keeps the position based edge count at "exactly two" - so dropping them
     is not enough, the polygon has to be triangulated so that every boundary
     edge lands in exactly one real triangle (an ear clip that never clips a
     collinear ear, deterministic order, is the smallest correct answer).
  2. **the recursion** is `EPS = 1e-9` against `SNAP = 1e-6`: `split_poly`
     gives a split piece its parent's normal but SNAPPED points, so the piece
     sits up to ~5e-7 off its own plane, is classified FRONT of it, and the
     child node picks the same plane again, for ever. The tolerance of the
     plane test must be coarser than the snap grid (and the polygon a node's
     plane was taken from must land in that node), or every rotated or
     sloped cutter is a crash.

* The screenshots: `.gauntlet-shots/10..13-modeling-*.png` come out of
  `tools/modeling_shot.cpp` over the BRIDGE and show the fallback room (a
  scaled cube called Wall.Front with Mesh Renderer / Box Collider / Rigidbody
  in the inspector - no Blockout section, no CSG node). 12 and 13 are byte
  identical. The bridge is not part of this round; the blockout pictures
  therefore come from a tool of their own that builds the room through
  `dai_doc` in C++, exactly as `tools/editor_shot.cpp` does.
* `node_icon()` in src/dai_editor_ui.cpp shows a blockout node that has no
  physics as the "empty" icon: the hierarchy does not yet say what it is.

## What the skeleton did (this commit)

| change | why |
|---|---|
| `include/dai_blockout_csg.h` - the boolean, cut out of `dai_blockout.h` verbatim and included at ITS end | so the shapes/welding and the boolean have one owner each. Every caller still writes `#include "dai_blockout.h"`. |
| `daiblock::degenerate_triangles()`, `normal_mismatches()`, `inward_faces()` in `dai_blockout.h` | the three measurements the bar names, in the API so both test files use one definition |
| `tests/blockout_csg_cases.hpp`, wired into `tests/test_doc.cpp` after `test_blockout()` | the bar as code: 2.4 - 0.4 = 2.0, union 3.5, intersect 0.5, arch / cylinder / 45-degree / wedge cutters, no degenerate triangle, determinism. RED today (see above): that is the work order, not a failure of the tests. |

## Ownership - no two modules touch the same file

| module | files it owns, and only these |
|---|---|
| **geometry** (Builder 1) | `include/dai_blockout.h`, `tests/blockout_cases.hpp` |
| **csg** (Builder 2) | `include/dai_blockout_csg.h`, `tests/blockout_csg_cases.hpp`, `tests/blockout_gltf_cases.hpp` |
| **editor** (Builder 3) | `src/dai_editor_ui_blockout.inl`, `src/dai_editor_ui_blockout_addcomp.inl`, `src/dai_editor_ui_blockout_addcomp_apply.inl`, `src/dai_editor_ui_blockout_inspector.inl`, `src/dai_editor_ui.cpp` (only `node_icon()`), `include/dai_blockout_host.inl`, `include/dai_blockout_props.inl`, `tests/test_editor_ui.cpp`, `tools/blockout_shot.cpp` (new), `tools/build_blockout_shot.sh` (new), `tools/run_tests.sh`, `docs/BLOCKOUT.md`, `RUN.md`, `.gauntlet-shots/` |

Read-only for everyone: `build.sh`, `build_win.sh`, `tests/test_doc.cpp`,
`tests/test_fracture.cpp`, `include/dai_doc.h`, `src/dai_doc*.cpp`,
`tools/modeling_shot.cpp`, `tools/build_modeling_shot.sh`, everything
`dai_daitex*` / `dai_bridge*` / `dai_material*`.

## The interfaces, exactly

**geometry -> csg, editor.** `include/dai_blockout.h` keeps every existing
name and signature: `Shape`, `Solid`, `Poly`, `Mesh`, `build()`,
`transform()`, `finalise()`, `volume()`, `area()`, `edge_report()`,
`open_edges()`, `nonmanifold_edges()`, `inconsistent_edges()`, `bounds()`,
`digest()`, plus the three new measurements. `EPS`, `SNAP`, `snap()`,
`make_poly()`, `flip()`, `detail::Key`, `detail::key_of()`, `detail::clean_poly()`
are what the boolean uses and keep their meaning. Nothing is renamed; a new
helper is added below the existing ones. The contract `finalise()` must
newly keep: **zero degenerate triangles on every closed input, edge count
unchanged** - the boolean's own tests (`csg`) depend on exactly this, and it
is the one cross dependency of the round: `[wall minus door] ... degenerate`
in `tests/blockout_csg_cases.hpp` turns green only when geometry's
triangulation lands.

**csg -> editor.** `daiblock::csg(const Solid&, const Solid&, int op)` keeps
its signature and its semantics (op = `daiblock::Op` = `dai_csg_op`, first
argument the base, empty inputs as today). If `csg` needs a coarser plane
tolerance it defines it IN `dai_blockout_csg.h` (e.g. `PLANE_EPS`, derived
from `SNAP`, documented as the reason `EPS` alone was not enough) and does
not move `EPS` or `SNAP` in geometry's file.

**editor -> the user.** Property names of `include/dai_blockout_props.inl`
do not change; `dai_node_desc` does not change (`dai_doc.h` is read only, and
the file format is the file format). The inspector seam keeps its rule: it
edits `r` and never calls `dai_doc_set`. The shot tool is
`tools/blockout_shot.cpp`, built by `tools/build_blockout_shot.sh` (same
flags as `tools/build_modeling_shot.sh`, copied not guessed), called as

```
tools/build_blockout_shot.sh OUTDIR W H [PREFIX]
```

and writes `PREFIX14-blockout-room.png` (the judged picture: four box
walls, one of them a CSG node with a door hole, stairs, arch, DoorSocket
gizmo on the door, hierarchy open, inspector on the CSG node) and
`PREFIX15-blockout-doorsocket.png` (the socket node selected, gizmo bright).
`tools/run_tests.sh` runs it twice, `1920 1080 wide-` and `1100 700 narrow-`,
right after the `modeling_shot` block and in the same shape (non-zero exit =
RED, MISSING when the .cpp is absent).

## Sequencing

All three start at once. `csg`'s degenerate checks go green when `geometry`
lands - `csg` does not work around that in its own file. `editor` writes
RUN.md and docs/BLOCKOUT.md LAST, from a `./build.sh && ./tools/run_tests.sh`
run on the merged tree; a number typed before that run exists is fiction.

## The bar for this round

1. `./build.sh` and `./build_win.sh` green.
2. `./tools/run_tests.sh` ends on `all green` with more than 3000 passed.
3. Determinism: two builds of the same fields bit identical (memcmp), for
   every generator and for the boolean.
4. `tests/blockout_csg_cases.hpp` green as written: 2.0 m3 to 1e-4, every
   edge on exactly two faces, zero degenerate triangles, zero normal
   mismatches, union 3.5, intersect 0.5, arch / cylinder / 45-degree / wedge
   cutters; `inward_faces() == 0` on every convex body in blockout_cases.hpp.
5. `.gauntlet-shots/wide-14-*` and `narrow-14-*` show room, hole, stairs,
   arch, socket gizmo, hierarchy with the nodes, inspector with the CSG
   node's fields - nothing clipped, no half drawn panel, at both sizes.
6. A field change on a box undone through the editor gives back a mesh with
   the old digest (`tests/test_editor_ui.cpp`).
7. A CSG result exported with `dai_gltf_write` reads back with the same
   triangle count (`tests/blockout_gltf_cases.hpp`, union as well as subtract).
8. No test weakened, no assertion removed, no fixture thinned.
9. Every number in RUN.md is a line from `build/last_run.log`.

---

# Round: the MODIFIER STACK

Detail on a blockout does not come from placing vertices. It comes from a
short list of RULES that are re-run whenever the rough shape moves - break
the edges, divide the faces, give the plane a thickness, repeat it, mirror
it. Blender calls that list the modifier stack. Daidalos has the rough shapes
(`include/dai_blockout.h`), the boolean (`dai_blockout_csg.h`), the host that
turns both into a mesh (`dai_blockout_host.inl`) and the inspector seams that
show them. What it has not got is the list. This round builds it.

Nothing below replaces what exists. `daiblock::Solid`, `csg()`, `finalise()`,
`volume()`, `open_edges()`, `digest()` and the snapping rules stay exactly as
they are and every modifier is written against them. `build.sh` and
`build_win.sh` are NOT touched: new geometry is header-only for the reason
`dai_blockout.h` gives at the top of itself, and new tests are `.hpp` case
files included by a translation unit `build.sh` already names.

## What is already standing (written by the lead, in the tree, compiling)

* `include/dai_modifier.h` - the whole data model and the plumbing:
  `daimod::Type`, `daimod::Mod` (one stack entry, the shape the document
  holds it in), `daimod::Stack`, the five parameter structs (`Bevel`,
  `Subdiv`, `Solidify`, `Array`, `Mirror`), the five `*_of(Mod)` readers that
  are the ONLY definition of what a zero field means, `topology_of()` (edges
  by welded position, sorted, with their two faces), `dihedral()`,
  `bounds(Solid)`, `settle()`, `apply(Stack, Solid) -> Result`,
  `finalise(Result)` / `finalise_smooth()` (positions identical, only the
  normals averaged) and `digest(Stack)` for the host's rebuild cache.
* `include/dai_modifier_dup.h` - `solidify()`, `array()`, `mirror()`, working
  today: a 2x2 plane solidified 0.1 m is 0.4 m3 with zero open edges, three
  boxes at 1.5 m spacing are 3.0 m3 and 36 triangles, half a box mirrored on
  X is a closed 1.0 m3 box.
* `include/dai_modifier_edge.h` - `subdivide()`, working today as the FLAT
  division (level 2 on a cube: 96 faces, 192 triangles, volume unchanged,
  zero open edges), and `bevel()`, which today returns its input unchanged
  and is the one deliberate hole in the skeleton. It is B1's first job and it
  is marked as such in the file.

## Modules, and the files each one owns

Two modules never edit the same file. That is the rule that lets all three
run at once.

### B1 - bevel and subdivide

Owns `include/dai_modifier_edge.h`, `tests/modifier_bevel_cases.hpp` (new)
and the include + call lines it adds to `tests/test_doc.cpp`.

* `bevel()`: break only edges whose faces disagree by more than `p.angle`
  degrees; each face shrinks back `p.width` in its own plane; each broken
  edge becomes `p.segments` (1..4) quads; each corner closes with a face. On
  a cube: 12 new edge faces with one segment, and a volume smaller by an
  amount the test derives on paper (tolerance 1e-4) - the message must print
  the formula's terms, not just "close enough". Must survive a CSG result:
  wall minus door, bevelled, has zero degenerate triangles (area > 1e-9),
  zero duplicate vertices inside the weld tolerance and zero open edges.
* `subdivide()`: keep the flat path exactly as it is (it is what "flat"
  means) and add the Catmull-Clark point rules behind `Subdiv::smooth`, so
  level 2 on a cube lands between the cube's volume and the volume of the
  sphere through its corners, and the face count is the one the test states.
* Levels clamp 1..3 and segments 1..4 in `*_of()` - do not clamp a second
  time somewhere else.

### B2 - solidify, array and mirror

Owns `include/dai_modifier_dup.h`, `tests/modifier_dup_cases.hpp` (new) and
the include + call lines it adds to `tests/test_fracture.cpp` (the TU
`blockout_gltf_cases.hpp` already rides).

* Harden what stands: solidify on an OPEN surface is closed (every edge on
  exactly two faces) and on a CLOSED one is a shell with the right volume;
  `shift` -1 / 0 / +1 puts the material where it says.
* `array()`: `n` copies is exactly `n` times the triangle count and a
  bounding box grown by `(n-1) * step`; relative offset is a multiple of the
  shape's own bounding size per axis; rotation per copy about X, Y or Z.
  State in the header what happens when copies touch exactly, and test the
  case the stair example uses.
* `mirror()`: every point has a partner across the plane to 1e-5, faces that
  lie entirely on the plane are dropped, the joined solid stays closed, and
  the weld distance does what it says.
* Determinism per operator: same input twice, `memcmp` of the vertex and
  index arrays.

### B3 - the stack on the node: document, inspector, bridge, pictures

Owns `include/dai_doc.h`, `src/dai_doc_text.cpp`, `src/dai_editor_ui.cpp`
(the multi-edit `DAI_MF` list and the AC_NODE completion table),
`include/dai_blockout_props.inl`, `include/dai_blockout_host.inl`,
`src/dai_editor_ui_blockout_inspector.inl`,
`src/dai_editor_ui_blockout_addcomp.inl`,
`src/dai_editor_ui_blockout_addcomp_apply.inl`, `tools/blockout_shot.cpp`,
`tools/run_tests.sh`, `tools/bridge_check.py`, `examples/scripts/`,
`docs/BLOCKOUT.md`, `RUN.md`, `tests/modifier_stack_cases.hpp` (new) and the
include + call lines it adds to `tests/test_editor_ui.cpp`.

The document contract, fixed here so the other two can be written against it:

```c
#define DAI_MODIFIER_MAX 8
typedef enum dai_modifier_type {
    DAI_MOD_NONE = 0, DAI_MOD_BEVEL, DAI_MOD_SUBDIVIDE,
    DAI_MOD_SOLIDIFY, DAI_MOD_ARRAY, DAI_MOD_MIRROR, DAI_MOD_TYPE_COUNT
} dai_modifier_type;
#define DAI_MODF_SMOOTH   0x1u
#define DAI_MODF_RELATIVE 0x2u
typedef struct dai_modifier {
    int      type;      /* dai_modifier_type, 0 = the slot is empty          */
    int      off;       /* 1 = switched off: the stack skips it entirely     */
    float    amount;    /* bevel width | solidify thickness | mirror weld, m */
    int      count;     /* bevel segments 1..4 | subdiv level 1..3 | copies  */
    float    angle;     /* bevel threshold deg | array degrees per copy      */
    int      axis;      /* mirror axis | array rotation axis, 0 X 1 Y 2 Z    */
    uint32_t flags;     /* DAI_MODF_*                                        */
    dai_vec3 offset;    /* array step                                        */
    float    param;     /* solidify shift -1..1                              */
} dai_modifier;         /* all fields 4 bytes: no padding, memcmp is honest  */
```

and in `dai_node_desc`, right after the CSG field:
`int modifier_count;` plus `dai_modifier modifiers[DAI_MODIFIER_MAX];`.
That is what makes undo, multi-edit and the whole-record `memcmp` in
`src/dai_doc.cpp` work with no new machinery at all.

`daimod::Mod` in `include/dai_modifier.h` is the double-precision mirror of
that struct; B3 writes the one conversion (`mod_of(const dai_modifier &)` /
`stack_of(const dai_node_desc &)`) and puts it in `dai_blockout_host.inl`
next to `shape_of()`, so there is exactly one.

Scene file: one `mod` line per entry inside the node block, strictly parsed
like every other key, plus the count; a scene without them loads unchanged
and a round trip is byte identical.

Property names for scripts and the bridge (`dai_blockout_props.inl`, index
0..7 parsed out of the name):

```
modifier.count                     how many entries
modifier.N.type                    1 bevel, 2 subdivide, 3 solidify, 4 array, 5 mirror
modifier.N.off                     the tick box, inverted flag
modifier.N.amount   (= .width, = .thickness, = .weld)
modifier.N.count    (= .segments,  = .level, = .copies)
modifier.N.angle
modifier.N.axis
modifier.N.offset                  vec3
modifier.N.param    (= .shift)
modifier.N.smooth                  flag bit 0
modifier.N.relative                flag bit 1
```

`modifier.0.width` and `modifier.0.segments` must work - a script that says
those two words gets a bevel. The aliases are the same storage, not a copy.

Inspector: one Modifiers list under the existing blockout fields. Add
Component > Modifier > Bevel / Subdivide / Solidify / Array / Mirror (kinds
30..34, the 20..26 range is taken). Per entry a header row - type name, on/off
tick, up, down, delete - then that type's fields and nothing else's. At
1100x700 nothing clipped and nothing overlapping, asserted the way
`dai_editor_ui_inspector_last_field` already asserts it.

Host: hash the modifier array into the rebuild key in `hash_node()`, run
`daimod::apply` on the solid the CSG produced, and finalise through
`daimod::finalise(Result)` so a smooth subdivide is shaded smooth. Nothing
else changes: the mesh is still derived, still never written back.

Pictures 16..18 in `tools/blockout_shot.cpp`, both sizes, hooked into
`tools/run_tests.sh` next to 14..15: the same shape without and with a bevel
side by side, a stair built by an array of one step, a mirrored part - each
with the inspector open on the node and the stack visible in it.

## Sequencing

All three start at once and never wait: B1 and B2 write pure geometry against
headers that already compile, B3 writes the document, the panel and the
bridge against the struct above and tests its own path with the modifiers
that already work (array, mirror, subdivide). The order test
(array-then-bevel differs from bevel-then-array) and the pictures that show a
bevel are the two things that need B1 landed; they are the last things B3
writes, not the first.

## The bar for this round

1. `./build.sh` and `./build_win.sh` green.
2. `./tools/run_tests.sh` ends on `all green` with more than 5144 passed.
3. The same stack computed twice is bit identical - vertices and indices.
4. Bevel: 12 new edge faces on a cube at one segment and the volume the
   formula predicts to 1e-4. Array: n times the triangles, box grown by
   (n-1)*step. Mirror: every point has its partner to 1e-5. Solidify on an
   open surface: closed. Subdivide level 2 on a cube: the stated face count
   and a volume between the cube and its sphere.
5. Bevel on a CSG result: zero degenerate triangles, no duplicate vertices
   inside the weld tolerance, still closed.
6. Order counts and is nailed down both ways; an entry that is off changes
   nothing at all; undo of a parameter change gives back the old digest.
7. `.gauntlet-shots/` shows it at 1920x1080 AND 1100x700: bevel on and off
   visibly different, a stair from an array, a mirrored part, the inspector
   with the stack - type, tick box, order.
8. A script sets a modifier through the property names and the mesh measurably
   changes.
9. No existing test weakened, no assertion removed, no fixture thinned.
10. Every number in RUN.md comes out of `build/last_run.log`.
