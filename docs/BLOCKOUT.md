# Blockout - building a room without leaving the editor

A blockout node is not a mesh in the project folder. It is the RECIPE for one:
"a wall, 4 m by 3 m by 30 cm, standing on the floor, with a 0.9 m doorway cut
out of it". The triangles are derived from those numbers every time the scene
opens, and they are never written into the scene file - which is the whole
point. A mesh baked into a scene is a mesh nobody can edit six months later;
five numbers are still five numbers.

Everything below is DATA on the node: it serialises, it undoes, it redoes, it
multi-selects, and a behaviour, a C++ component or the Jarvis bridge can write
it by name. Nothing about a room is hard coded anywhere.

```
   dai_node_desc            the recipe: blockout, csg, door_socket + fields
        |  include/dai_blockout.h        pure arithmetic, no engine, no GPU
        v
   daiblock::Solid          convex polygons in the node's own space
        |  csg(a, b, op)                 union / subtract / intersect
        v
   daiblock::Mesh           welded, T junctions repaired, triangulated
        |  include/dai_blockout_host.inl the editor host, once per frame
        v
   dai_render_mesh_create   an ordinary mesh: drawn, picked, exported as glTF
```

## The components

### Blockout (Add Component > Blockout > Box / Cylinder / Stairs / Arch / Wedge)

| field | property name | meaning |
|---|---|---|
| Shape | `blockout.kind` | 1 Box, 2 Cylinder, 3 Stairs, 4 Arch, 5 Wedge |
| Size (m) | `blockout.size` | **full** size per axis, in metres. Not half extents - a door is 0.9 m wide, and an artist made to type 0.45 will type 0.9 anyway |
| Segments | `blockout.segments` | sides around a cylinder, steps along an arch |
| Steps | `blockout.steps` | how many steps a flight has |
| Thickness (m) | `blockout.thickness` | the arch ring; 0 means a fifth of the smaller of rise and span |
| Pivot | `blockout.pivot` | −1..1 per axis: where the node origin sits inside the shape's own box. 0 is the centre, −1 the min face. A wall is built from the floor up, so its pivot Y is −1 |

A zero is always "the default", never a zero sized wall: that is what makes a
scene written before these fields existed load unchanged.

What the five shapes are, exactly - each one a closed solid whose volume the
suite checks against arithmetic done on paper:

* **Box** - a rectangular block. `size` is what a tape measure would read.
* **Cylinder** - an n-sided prism inscribed in the ellipse `size.x` × `size.z`,
  `size.y` tall. Sixteen sides by default; the volume is the inscribed prism's,
  not the circle's, and the test says so.
* **Stairs** - a flight rising in +Y and running in +Z inside the `size` block.
  N steps hold `w·h·d·(N+1)/(2N)`: 5 m³ of a 1×2×4 block at four steps, 4.5 at
  eight. Built face by face, NOT as a union of boxes - a union would leave the
  internal faces where the boxes touch inside the solid.
* **Arch** - a half annulus of `thickness`, spanning `size.x`, rising `size.y`,
  extruded `size.z` deep. A doorway to subtract, or a gate to stand in.
* **Wedge** - a ramp: full height at −Z, nothing at +Z. Half a box.

### CSG (Add Component > Blockout > CSG (boolean))

`csg.op` is 1 union, 2 subtract, 3 intersect. The node's blockout CHILDREN are
combined in hierarchy order, first child first, and the result is drawn on the
node itself. The children stop drawing themselves the moment the parent has an
operation - the hole belongs to the wall now - and moving a child with the
ordinary gizmo moves the hole.

A child that is itself a CSG node is resolved the same way, so "a wall with a
doorway, and that doorway with a letterbox" is one expression and not a special
case. A CSG node with no shape of its own takes its first child as the base:
subtracting from nothing is nothing, and a node that draws nothing looks
exactly like a node that is broken.

The result is a real solid, not a picture of one:

* every edge is shared by **exactly two** triangles;
* every face is wound the same way round, and outwards;
* the volume is the volume the arithmetic says.

All three are checked in `tests/blockout_cases.hpp` for the box, the cylinder
window, the two-wall corner and the twice cut wall.

### DoorSocket (Add Component > Blockout > Door Socket)

Where the next room may be docked on. `door.offset` is the middle of the
opening's **sill** in the node's own space - a door stands on the floor, and a
socket measured from the middle of the air is a socket nobody can place by
hand. `door.normal` is which way you walk through it, `door.width` and
`door.height` are the opening.

It is drawn in the viewport as the opening's rectangle plus an arrow along the
normal, bright on the selected node and dim on the rest - the same rule the
collider wireframes follow. An anchor point nobody can see is an anchor point
nobody trusts.

### Modifiers (Add Component > Blockout > Modifier > Bevel / Subdivide / Solidify / Array / Mirror)

Detail on a blockout does not come from placing vertices. It comes from a short
list of RULES that are re-run whenever the rough shape moves: break the edges,
divide the faces, give the plane a thickness, repeat it, mirror it. Blender
calls that list the modifier stack, and this is it.

The stack is **data on the node** - `int modifier_count` plus
`dai_modifier modifiers[8]` in `dai_node_desc`. So it is in the scene file, it
rides the same undo step as every other field, it lands on every selected node
in a multi-edit, and the mesh it produces stays derived: the stack runs on the
solid the CSG produced, before anything becomes triangles, and nothing is ever
written back into the document.

Up to eight entries, run **in order, first entry first**. The order is part of
the result and not a detail:

* Bevel, then Array - nine steps, each with its own broken edge.
* Array, then Bevel - one block of nine treads with a break around its
  outline, because after the array the seams between the copies are no longer
  edges.

Both are useful and they are different meshes; `tests/modifier_stack_cases.hpp`
computes them and asserts they differ, and asserts the triangle count of the
first one against one bevelled cube times the number of copies.

| type | number | what it does | its fields |
|---|---|---|---|
| Bevel | 1 | breaks every edge whose two faces disagree by more than `angle` degrees; each face steps back `width` in its own plane | `width` (m), `segments` 1..4, `angle` (deg) |
| Subdivide | 2 | quarters every face, `level` times. `smooth` pulls the new points towards their neighbours (Catmull-Clark) and averages the normals; without it the shape does not move at all | `level` 1..3, `smooth` |
| Solidify | 3 | gives a surface a wall. An OPEN surface comes back CLOSED - that is what this one is for | `thickness` (m), `shift` -1 inward .. +1 outward |
| Array | 4 | `copies` copies, each one `offset` further on. `relative` makes the offset a multiple of the shape's own bounding size per axis instead of metres; `rotation` degrees are added per copy around `axis` | `copies`, `offset` [x,y,z], `relative`, `rotation` (deg), `axis` |
| Mirror | 5 | mirrors on the local X, Y or Z plane and joins the two halves; points closer to the plane than `weld` are moved onto it | `axis` 0 X / 1 Y / 2 Z, `weld` (m) |

A **zero is the default** here like everywhere else in the record, and what a
zero means is written down exactly once - `bevel_of()`, `subdiv_of()`,
`solidify_of()`, `array_of()` and `mirror_of()` in `include/dai_modifier.h`.
The inspector shows the number those functions read, so the panel and the
geometry cannot disagree.

**In the inspector.** The Modifiers list sits under the other blockout
sections. Every entry has a header row - its number and type, the tick box that
switches it off, and the arrows and the minus that reorder or remove it -
and under it the fields of THAT type and nothing else's. An entry that is
switched off is skipped entirely and keeps every number it had, which is what
makes "switch it off and look" useful; the suite asserts that an entry that is
off changes the mesh by not one bit.

**The property names.** Index 0..7 out of the name, one table for JavaScript,
C++ components and the bridge, exactly like the fields above:

```
modifier.count                    how many entries the stack runs (0..8)
modifier.N.type                   1 Bevel 2 Subdivide 3 Solidify 4 Array 5 Mirror
modifier.N.off                    the tick box, 1 = skipped
modifier.N.enabled                the same, the way round a reader expects
modifier.N.amount                 = .width  = .thickness  = .weld     (metres)
modifier.N.count                  = .segments  = .level  = .copies
modifier.N.angle                  = .rotation   (degrees)
modifier.N.axis                   0 X, 1 Y, 2 Z
modifier.N.offset                 = .step       [x, y, z], the array's step
modifier.N.param                  = .shift      solidify, -1..1
modifier.N.smooth                 subdivide: average the normals
modifier.N.relative               array: the offset is a multiple of the size
```

The aliases are the **same four bytes**, not a copy: `modifier.0.width` and
`modifier.0.amount` are one field and read each other back.
`examples/scripts/modifier_stair.js` builds a staircase out of one box, a
Bevel and an Array through these names alone, and `tools/bridge_check.py`
sets one over the socket and photographs the mesh before and after.

**In the scene file.** One line per non-empty slot inside the node block, plus
the count. The slot's index is the first number on the line, so the order
survives even when a middle slot happens to hold a default:

```
node 7
  name Stair
  blockout 1
  bosize 1.3 0.18 0.3
  bopivot 0 -1 0
  modcount 2
  mod 0 1 0 0.012 2 30 0 0 0 0 0 0 0
  mod 1 4 0 0 9 0 1 0 0 0 0.18 0.3 0
end
```

The fields after the index are, in order: `type off amount count angle axis
smooth relative offset.x offset.y offset.z param` - the fields of
`dai_modifier` in the order the struct lists them. The line is parsed as
strictly as every other key: an unknown type, a slot past the end of the stack,
a count longer than the stack or a line that stops half way is a rejected file,
not a silently dropped entry. A scene written before the stack existed has
neither line and loads exactly as it did.

## From a script or from the bridge

The property names above are the same in JavaScript, in a C++ component and
over the Jarvis bridge, because all three go through one table
(`include/dai_blockout_props.inl`). The script editor offers them.

```js
// a wall with a doorway, made entirely from data
var wall = scene.add("Wall");
wall.blockout.kind = 1;                 // Box
wall.blockout.size = [4, 3, 0.3];
wall.blockout.pivot = [0, -1, 0];       // standing on the floor
wall.csg.op = 2;                        // subtract the children
wall.door.enabled = true;
wall.door.width = 0.9;
wall.door.height = 2.05;

var hole = scene.add("Doorway");
hole.parent = wall;
hole.blockout.kind = 1;
hole.blockout.size = [0.9, 2.05, 1];    // thicker than the wall, on purpose
hole.blockout.pivot = [0, -1, 0];
```

## The rules the mesh builder keeps

**Deterministic.** The same fields in give the same triangles out, in the same
order, byte for byte - no clock, no random, no hash iteration order. Two builds
are compared with `memcmp` in the suite, and a changed field is checked to
really change the digest, so the proof cannot be a cache working.

**Closed.** Every position is snapped to a micrometre grid before anything is
welded, and T junctions - a vertex sitting in the middle of a neighbour's edge,
which is the seam you can see daylight through - are repaired by splitting that
edge. `daiblock::open_edges`, `nonmanifold_edges` and `inconsistent_edges` are
part of the API rather than comments, and the exported glTF is read back and
measured in `tests/blockout_gltf_cases.hpp`.

**Cached by content, not by frame.** The host hashes the fields the mesh came
out of, including every child that took part in the boolean and its transform,
and rebuilds only when that number moves. A wall rebuilt every frame is a mesh
upload per frame per wall.

**Derived, never written back.** The host attaches the mesh with
`dai_scene_set_render` on the entity behind the node. The document's own `mesh`
field stays `0xFFFFFFFF`, which is what makes the sync layer leave the built
mesh alone. Writing a mesh id back would put derived data in the undo stack and
in the scene file.

**Metres, and only once.** The mesh is built in metres, so a blockout node
carries `render_extent` 1, 1, 1 - "the model is already the size it says it
is". Add Component sets it; the host divides by the entity's render scale for
anything that does not, so a wall on a node with a half metre collider is still
4 m long.

## Where the code is

| file | what it holds |
|---|---|
| `include/dai_blockout.h` | the generators, the welding and the measurements the tests are written against. Header only: `build.sh` is frozen, so a new `src/*.cpp` would never reach an archive |
| `include/dai_blockout_csg.h` | the BSP boolean, `csg(a, b, op)`. Included at the end of `dai_blockout.h`, so one include gets both |
| `include/dai_blockout_host.inl` | the editor host: document -> solid -> stack -> mesh -> renderer, cached. `shape_of()` and `stack_of()` are the only conversions out of the document |
| `include/dai_modifier.h` | the stack's data model, the topology the operators read, `apply()` and the two ways a result becomes triangles |
| `include/dai_modifier_edge.h` | `bevel()` and `subdivide()` |
| `include/dai_modifier_dup.h` | `solidify()`, `array()` and `mirror()` |
| `include/dai_blockout_props.inl` | one property table for JS, C++ components and the bridge |
| `src/dai_editor_ui_blockout_inspector.inl` | the three inspector sections |
| `src/dai_editor_ui_blockout_addcomp.inl` | the Blockout category in Add Component |
| `src/dai_editor_ui_blockout_addcomp_apply.inl` | what a freshly added component arrives with |
| `src/dai_editor_ui_blockout.inl` | the DoorSocket gizmo |
| `tests/blockout_cases.hpp` | shapes, booleans, determinism, the document round trip (runs in `build/test_doc`) |
| `tests/blockout_csg_cases.hpp` | the boolean against the bar: volumes with a closed form, no degenerate triangle, sloped cutters (runs in `build/test_doc`) |
| `tests/blockout_gltf_cases.hpp` | the .glb round trip (runs in `build/test_fracture`) |
| `tests/modifier_bevel_cases.hpp`, `tests/modifier_dup_cases.hpp` | the geometry of the five operators, measured (volumes, face counts, closedness, determinism) |
| `tests/modifier_stack_cases.hpp` | the stack as document: the scene lines, undo, the property names, the order, and the list at 1100x700 (runs in `build/test_editor_ui`) |
| `examples/scripts/modifier_stair.js` | a staircase from one box, a Bevel and an Array, over the bridge |
| `tests/test_editor_ui.cpp`, section 12 "blockout" | the panel against the mesh: the Size row found by probing, one undo step per drag, the mesh digest back after undo and forward after redo, the same drag on two selected boxes, the CSG operation switched through its dropdown and undone, the socket gizmo drawn in the Scene view and not in the Game view (runs in `build/test_editor_ui`) |
| `tools/blockout_shot.cpp` | the room built through `dai_doc` in C++ and photographed by the real editor with the host attached - the two pictures below |
| `tools/build_blockout_shot.sh` | compiles and runs it with build.sh's own flags: `tools/build_blockout_shot.sh OUTDIR W H [PREFIX]` |

## In the editor

The hierarchy says what a node is: a CSG node carries the collider glyph (a
boolean is a shape made of shapes), a blockout node the cube, a node whose
whole job is the docking point the package glyph. `node_icon()` in
`src/dai_editor_ui.cpp` answers in that order - before the "empty" a node
without physics used to get.

The three inspector sections sit among the other components, above the Add
Component button: Blockout (Shape, Size, Segments / Steps / Thickness where
the shape has them, Pivot), CSG (Operation, and what the children do), Door
Socket (Position, Normal, Width, Height). They edit the record and nothing
else; the diff below the seam turns a drag into ONE undo step and pushes the
same change into every selected node - the blockout, CSG and socket fields
are in that diff's list like position and colour are.

The socket gizmo takes its frame from the node's WORLD transform: the normal
and the up vector are rotated by the node, so a yawed wall draws its opening
square to itself and the arrow still points the way the door faces.

## The pictures

`tools/run_tests.sh` runs `tools/build_blockout_shot.sh` twice, right after
the modelling shots, at 1920x1080 (`wide-`) and 1100x700 (`narrow-`), into
`.gauntlet-shots/`:

* `14-blockout-room.png` - a 6 x 5 m room: floor and four walls as box
  blockouts standing on the floor, `Wall.Front` a CSG node (subtract) with the
  child `Doorway` cut out of it and a door socket on its sill facing out, a
  flight of six stairs and an arch inside. The CSG wall is selected: translate
  gizmo on it, the socket frame and arrow bright on the door, the hierarchy
  open with `Doorway` under `Wall.Front`, the inspector on Transform, Mesh
  Renderer, Blockout, CSG and Door Socket.
* `15-blockout-doorsocket.png` - the same scene from close to the door, the
  socket's node selected.
* `16-modifier-bevel.png` - the same 1.1 m block twice, side by side: the left
  one raw, the right one with one Bevel entry (8 cm, three segments) on it and
  selected, so the inspector shows the stack that made the difference.
* `17-modifier-array-stair.png` - a staircase of eight treads out of ONE box
  and one Array entry, the node selected and its list open.
* `18-modifier-mirror.png` - half a bracket and a Mirror on X: one closed
  solid, welded down the middle.

A non-zero exit of the tool is a red line in the run; a missing `.cpp` is
listed under "not built".
