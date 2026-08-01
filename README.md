# Daidalos

A deterministic C++ game engine. General purpose: it does not assume a genre,
a camera style or a gameplay loop. What it gives you is the part that is
painful to retrofit later.

```
  simulation   deterministic fixed tick, snapshots, rollback, input queue
  physics      swappable backend (Jolt today, null backend for proof)
  scene        entities: a body plus how it looks, compounds, camera helpers
  rendering    Vulkan 1.3, meshes, materials, sun, sky, shadows, MSAA
  audio        event driven, decoupled from the sim (Aulos)
  host         your game, your window, your editor. The engine never calls you.
```

The one rule the whole design follows:

```
state(n+1) = step(state(n), input(n))
```

The simulation is a pure function of state and input. It never reads the clock,
the audio system, the renderer, or an unseeded random number. That is what makes
rollback netcode possible - and it is the thing you cannot add afterwards.

MIT licensed.

---

## Build and run

```bash
./build.sh                     # engine + backends + renderer + tests + examples
./build.sh noaudio             # without Aulos

./build/test_daidalos                                    # 48 simulation assertions
./build/test_merge                                       # 45 merge/split assertions
./build/test_font                                        # 16 font assertions
DAI_SHADER_DIR=shaders ./build/test_ui /tmp              # 19 UI assertions
DAI_SHADER_DIR=shaders ./build/test_particles /tmp       # 16 particle assertions
DAI_SHADER_DIR=shaders ./build/test_skinning assets/test # 14 skinning assertions
DAI_SHADER_DIR=shaders ./build/test_render_visual /tmp   # 33 visual assertions
./build/test_image /tmp/pngfix                           #  9 PNG/DEFLATE assertions
DAI_SHADER_DIR=shaders ./build/test_gltf assets/test /tmp # 15 import assertions

# a real window, on a machine without a display:
Xvfb :77 -screen 0 1280x720x24 &
DISPLAY=:77 DAI_SHADER_DIR=shaders ./build/test_window

# the same test against a Wayland compositor, also headless:
mkdir -p /tmp/wl && chmod 700 /tmp/wl
XDG_RUNTIME_DIR=/tmp/wl weston --backend=headless-backend.so --socket=wl-daidalos &
DAI_WINDOW=wayland ./build.sh
XDG_RUNTIME_DIR=/tmp/wl WAYLAND_DISPLAY=wl-daidalos DAI_SHADER_DIR=shaders ./build/test_window

DAI_WINDOW=win32 ./build.sh     # cross compile check with mingw-w64
DISPLAY=:77 DAI_SHADER_DIR=shaders ./build/window_demo
DAI_SHADER_DIR=shaders ./build/sandbox_demo 6 /tmp       # general sandbox scene
DAI_SHADER_DIR=shaders ./build/vehicle_demo  6 /tmp      # machine built from joints
```

`build.sh` compiles `dai_engine.cpp` **without the Jolt include path**. If a
Jolt header ever leaks into the engine core, the build breaks. That is the
entire point of `src/dai_physics.hpp`.

The same trick guards the renderer: the build fails if any Vulkan symbol
appears outside `src/rhi_vulkan*`, and it links and runs a program that uses
the engine and the scene layer **without `-lvulkan`**. Swapping in D3D12, Metal
or a bridge into someone else's engine means writing one `rhi_*.cpp`.

### Dependencies, in full

Jolt (physics) and Aulos (audio) are vendored. Vulkan is an API, not a library
that does work for us. Everything else - matrix maths, mesh generation, OBJ,
PNG **encode and decode**, DEFLATE, JSON, glTF, base64, the whole renderer - is
written here. No stb, no zlib, no libpng, no GLM, no tinygltf, no VMA.

---

## WebAssembly

The simulation core builds for the web and produces **the same numbers**:

```bash
./build_web.sh && node build/web/daidalos_web.js
```

```
backend   : null
ticks     : 600
CHECKSUM  : c45b292fa025384e      <- identical to the native build
rollback  : ok, checksum reproduced
```

600 ticks, 65 bodies, a rollback, byte for byte the same checksum as the native
binary of the same sources. That is the prerequisite for a browser client
sharing a rollback session with a native one, and it is only true because the
tick never reads the clock, the renderer or an unseeded random number.

`./build_web.sh` produces three things: the headless self check above, a
**library build** (`daidalos.js` + `.wasm`, MODULARIZE'd, flat C exports) and
**generated TypeScript declarations** (`daidalos.d.ts`) so a TS client gets
completions and type errors instead of `any`. Transforms are read straight out
of `HEAPF32` - 9 floats per body, no copy per frame:

```ts
const n = M._dai_web_transforms(world, 1.0);
const view = new Float32Array(M.HEAPF32.buffer, M._dai_web_transform_ptr(), n * 9);
// [handle, userData, x, y, z, qx, qy, qz, qw] per body
```

`tests/test_web.mjs` runs that path under node. One thing it makes explicit:
determinism is a property of the SIMULATION, not of building a scene in another
language - JS computes `-4 + (i % 8) * 1.1` in double and rounds on the way
into a float parameter, C++ does it in float throughout, so the two builds
start microns apart. Feed both sides bit identical inputs and they agree.

40 KB of wasm for engine + scene + particles. `DAI_NO_JOLT` drops the physics
backend from the link, which is also a stricter version of the leak test: the
engine has to be complete without it.

What does NOT port yet is the renderer - Vulkan has no browser equivalent. A
web build needs a WebGPU backend behind `dai_render.h`, which is precisely the
swap the RHI boundary exists for: one new `rhi_*.cpp`, nothing else changes.
Jolt itself compiles to wasm (upstream supports Emscripten), so a full browser
build is a build system exercise rather than a redesign.

## Layers

### Simulation core - `include/daidalos.h`

Fixed tick, tick stamped command log for every world mutation, snapshot ring,
rollback with replay, per player input ring, deterministic RNG, audio events.
Bodies: box, sphere, capsule, compound. Joints: fixed, hinge (bearing), slider
(piston), point, distance - with limits and motors, all rollback safe.

### Physics backend - `src/dai_physics.hpp`

One interface, no foreign types: `dai_vec3`, `dai_quat`, slot indices. Two
implementations: `physics_jolt.cpp` (the only file in the project that includes
Jolt) and `physics_null.cpp` (gravity and a floor - it exists so the abstraction
can be proven, not assumed).

### Scene document - `include/dai_doc.h`

What the editor edits. Plain records with stable ids that are never reused, a
parent/child hierarchy, and an undo stack built from before/after snapshots of
whole records - one `Change` covers create, modify and delete, so undo works for
everything without a command class per property.

This layer exists because of one problem: an editor that mutates the live world
can only undo what it can put back, and a destroyed physics body cannot come
back under its old handle. So "undo delete" ends up broken or missing. The fix
is the same one Unity and Godot use - edit the document, then reconcile the
runtime against it. `dai_doc_sync_apply` does that incrementally, keyed on a per
node revision, so untouched nodes are left alone and physics keeps running while
you edit. `dai_doc_sync_pull` goes the other way: stop play mode, keep the
result.

Scenes save as a line based text format, one property per line, only non default
fields written, floats at the shortest precision that reads back bit identical.
That diffs and merges per property instead of per brace, which matters when a
scene file lives in version control.

### Editor - `include/dai_editor.h`, `include/dai_editor_ui.h`

Two layers on purpose. `dai_editor` is the core: picking, selection, gizmo
maths, drags, play mode, timeline scrubbing - no UI dependency at all, so a web
viewer or a Qt shell can drive it. `dai_editor_ui` is the one file that knows
about both the editor and `dai_ui`, and draws the hierarchy, inspector, toolbar
and timeline.

The viewport camera uses Unity's bindings, not Blender's: right mouse to look
around with WASD/QE flying while it is held, wheel for speed while flying and
dolly otherwise, middle to pan, alt+left to orbit, F to frame the selection.
The bindings live in `dai_editor_cam_update` so a frontend fills one input
struct instead of scattering them.

Play mode never touches the document, so Stop is exact: the sync layer is told
that everything it believes about the world is stale and rewrites it. Keeping a
simulated result is an explicit button, not the default. Scrubbing rides the
engine's snapshot ring - `dai_seek_to` restores a snapshot going back and
replays recorded commands going forward, which lands on bit identical state
either way.

The inspector is laid out in components, the way Unity's is: Transform,
Rigidbody and Renderer under collapsible headers, each header's checkbox being
that component's on/off switch - Rigidbody off is `no_body`, which is what a
group node is; Renderer off is `hidden`. The blocks are not invented for the
sake of the metaphor, each one is the part of `dai_node_desc` the engine
already treats as one thing.

**The document is the truth, and the scene follows it.** Typing a number into
the inspector used to change the document and nothing else, so the gizmo -
which reads the document - jumped to the new position while the object stayed
exactly where it was. Only the gizmo drag path had ever called the
document-to-scene resync. `dai_editor_advance` now resyncs whenever the editor
is not playing, and `dai_editor_resync` exposes the same thing for a frontend
that never calls advance. Both are incremental and idempotent, so doing it
after an edit and again next frame costs nothing.

The inspector takes typed numbers, not just drags: click a field, type,
Enter or click away commits - and the axis letter in front of a vec3 is a
drag handle, the way Unity's X/Y/Z labels work. Rotation shows degrees,
cached so the quaternion's round trip never fights a half typed number.
Undo batches by edit: a typed value is one step, like a drag, not one step
per keystroke. The blocks mirror what the engine treats as one thing, and
after the component split that is Transform, **Rigidbody** (only the mass
side: motion, friction, bounce), **Collider** (shape, Is Trigger, size,
center) and Renderer (mesh by name, colour, rough, emissive) - separate
because they are: a static level is a collider without a rigidbody, and a
trigger volume is a collider that reports overlaps instead of blocking.
During play the inspector and the gizmo read the live body - the document
holds the pre-play pose, which is exactly what makes Stop exact.

Right click opens a menu: Rename / Duplicate / Delete on a node, New Box /
New Sphere on empty space, in the hierarchy and in the viewport. The click
that dismisses a menu cannot also press whatever lies under it, and F2
renames inline.

A project is a folder of scenes and assets, no more mystical than Unity's:
the host hands over listing, create and open, and the Project window's
Projects tab is where "nothing mounted" becomes a project.

Panels dock. A window carries an edge and a slot - the whole edge, its upper
half or its lower half - so the default layout is hierarchy over project on the
left and inspector down the right, dragging a title bar pulls a window out,
dropping it near an edge puts it back with a preview rectangle, and the resize
grip drags the split because a docked window keeps its own width.

### Scene layer - `include/dai_scene.h`

The glue between "a body exists" and "here is what it looks like". Spawn an
entity with a body description plus mesh, colour, roughness, emissive, flags.
Compound bodies expand into one render instance per part. Camera helpers:
orbit, exponential follow, frame-a-bounding-sphere.

Without this layer every demo grows a `switch (user_data)` that guesses sizes -
which is exactly how "why does that crate look like a plank" happens.

### Asset import - `include/dai_gltf.h`

glTF 2.0 / GLB, written from scratch: own JSON parser, own base64, own PNG and
DEFLATE decoder. Meshes, metallic-roughness materials, embedded or external PNG
textures, the node hierarchy flattened to world space TRS.

```bash
DAI_SHADER_DIR=shaders ./build/model_viewer scene.glb 4 /tmp
```

The test loads a GLB that **Blender 4.5 exported on a real machine** (five
objects, five materials, a packed colour grid texture, a 12x scaled ground
plane) and checks geometry, materials, the Z-up to Y-up conversion and the
matrix decomposition. See `docs/MATERIALS.md` for why the material model is
four maps and no node graph.

### Assets - `include/dai_assets.h`

The glue between "the document says `models/crate.glb`" and "there is a mesh on
screen". Two libraries that never learn about each other:

- **Mnemosyne** decides where bytes come from - mounted folders and pack files
  searched by priority, so a mod folder overrides the shipped pack - loads each
  file once, caches it, and notices when it changed on disk.
- **dai_gltf** turns those bytes into meshes, materials and textures.

```c
dai_assets *a = dai_assets_create(renderer, 1);   // 1 = watch for edits
dai_assets_mount_pack(a, "game.mnp", 0);
dai_assets_mount_dir (a, "mods/hd", 10);          // wins
dai_assets_bind(a, sync);

// per frame
if (dai_assets_poll(a)) dai_assets_bind(a, sync);  // something (re)loaded
dai_doc_sync_apply(sync);
```

Loading is asynchronous and the split is not cosmetic: reading and parsing run
on worker threads, and the GPU upload happens inside `dai_assets_poll` on the
thread that owns the Vulkan context, because a loader that touches the GPU from
a worker is a crash waiting for a busy frame. A node whose asset has not
arrived yet keeps drawing its collision shape - visibly wrong, never invisible -
and picks up the real mesh a frame or two later.

A file with several objects can go into a scene two ways, and the difference is
physical:

- **One node.** `asset models/crate.glb` draws every piece, sharing one rigid
  body. Right for anything rigid - a chair, a rock, a lamp post.
- **A tree.** `dai_assets_instantiate` makes one document node per piece,
  parented the way it was parented in Blender, each with its own selector and
  its own body sized from its own bounding box. That is what a crate whose lid
  opens needs: a lid that rotates against its crate is two bodies and a joint,
  which one body can never be.

The hierarchy survives the import either way. Transforms are flattened to model
space so a piece can be drawn without walking anything, but each piece also
keeps `parent` and its local transform - the structure the artist built is data,
not something the importer throws away. `models/scene.glb#Crate` narrows to a
single object, which is how the same export doubles as a library of props.

The resolver is asked twice - once with a null buffer to get the count, once to
fill it - so an asset with forty pieces does not need a guessed maximum.

This is also where the sync layer had a real hole: the resolver only ran when a
node was first built, so an asset that finished loading afterwards never
reached the screen. It now re-resolves on every pass and rebuilds the entity
when the answer moved - which is exactly what asynchronous loading needs.

Unloading gives the geometry back. `dai_model_release` destroys the meshes,
textures and materials the import created; mesh slots keep their slice of the
geometry buffer and hand it to the next mesh that fits, textures and material
slots are recycled outright. Reloading the same model four times leaves the
mesh table exactly where the first load left it - measured, in test_assets [9],
because "we free it now" is the kind of claim that quietly stops being true.

### Asset browser - `dai_editor_ui_assets`

What is on disk, and one click to put it in the scene. The panel does not know
where the list comes from or how to load anything - the host fills it, the host
places it. Same rule the resolver follows, and the reason the editor UI still
builds without the asset layer.

```c
char paths[64][96];
uint32_t n = dai_assets_list(assets, paths[0], 64, 96);
const char *ptrs[64];
for (uint32_t i = 0; i < n && i < 64; ++i) ptrs[i] = paths[i];
dai_editor_ui_asset_list(panel, ptrs, n);

const char *pick; int as_tree;
if (dai_editor_ui_assets(panel, x, y, w, h, &pick, &as_tree)) {
    if (as_tree) dai_assets_instantiate(assets, doc, pick, 0);
    else         add_a_node_whose_asset_is(pick);
}
```

Two buttons because the difference is physical: **Place** is one node and one
rigid body, **As tree** is one node per piece with a body each. `dai_assets_list`
walks the mounted folders and packs, keeps only what the loader can actually
open, and sorts and de-duplicates - the same file in a folder and in a pack is
one entry, because the mount priority already decided which copy wins.

It is deliberately not in the default layout: it needs a list only the host can
produce, and a panel that always says "nothing mounted" is worse than no panel.

### Skinning and animation

glTF skins and animations import with the rest of the file: joint hierarchies,
inverse bind matrices, and translation/rotation/scale channels with LINEAR,
STEP and CUBICSPLINE interpolation (rotations slerp along the shortest arc).

```c
float joints[64 * 16];
uint32_t n = dai_model_pose(model, 0, time_seconds, joints, 64);
dai_render_joints(renderer, joints, n);            // one upload for every character
uint32_t count = dai_model_instances(model, inst, 256, offset, rot, scale);
```

Skinning happens in the vertex stage: four influences per vertex, joint
matrices in a storage buffer, and the same pipeline draws rigid and skinned
meshes (a vertex with no weights keeps an identity matrix). Rigid nodes of an
animated file follow their animated parents, so a swinging door is just a node.

### Fonts - `include/dai_font.h`

TrueType parsing and rasterisation, from scratch like everything else: cmap
(formats 4 and 12), simple and composite glyphs, quadratic beziers flattened
adaptively, scanline fill with the non zero winding rule and 4x vertical
supersampling, packed into one atlas.

Not FreeType, because FreeType is 200k lines and a build dependency on every
platform, while the part a game needs - outline, metrics, atlas - is a few
hundred.

```c
dai_font *f = dai_font_load("DejaVuSans.ttf", 32.0f, NULL, 0, err, sizeof(err));
const uint8_t *rgba = dai_font_atlas_rgba(f, &w, &h);   // upload as a texture
const dai_glyph *g = dai_font_glyph(f, codepoint);      // uv rect + offsets + advance
```

`tests/test_font.cpp` checks the things that are true of any correct renderer
rather than a reference image: space has advance but no ink, 'M' is wider than
'i', a composite glyph (a-umlaut) is not empty, UTF-8 decodes surrogate free,
and - the one that catches a wrong winding rule - **the middle of an 'o' is
empty while its edge is solid**.

### Icons - `include/dai_svg.h`, `include/dai_icons.h`

The same argument as the font, one layer up: an icon needs a path, a stroke
width and a viewBox out of SVG, and that is a few hundred lines. A baked PNG
sheet has exactly one correct size and turns to porridge the moment a display
is scaled; librsvg means shipping cairo, pango and glib to draw a play
triangle. So the icons are vectors, and they are rasterised at startup at the
size the interface actually draws them at.

`dai_svg` parses an XML subset good enough for icon files - `<path>` with
every command including arcs, rect/circle/ellipse/line/polyline/polygon, `<g>`
with inherited presentation attributes, `transform` with translate, scale,
rotate, matrix and the two skews, `fill-rule`, and stroke with width, caps and
joins. Colour, gradients, text and CSS are deliberately absent: an icon is
rasterised to **coverage** and tinted when drawn, so its own colours would be
thrown away anyway.

```c
dai_icons *icons = dai_icons_create(16.0f);              // the built-in set
const uint8_t *rgba = dai_icons_atlas_rgba(icons, &w, &h);
dai_ui_set_icons(ui, icons, dai_render_texture_create(r, rgba, w, h, 0));

if (dai_ui_icon_button(ui, DAI_ICON_PLAY, "Play", 0)) play();   // tooltip included
dai_icons_add(icons, "my-icon", "<svg viewBox='0 0 24 24'>...</svg>");
```

Two things the rasteriser has to get exactly right, and neither is obvious:

**Subpaths keep the direction they were written in.** A donut is an outer ring
one way round and an inner ring the other, and the non zero rule reads those
directions to tell a hole from a solid. Normalising them "for consistency"
fills the hole in. Only the stroker forces an orientation, and it must,
because there overlapping pieces have to add rather than cancel - a round join
landing on its own segment with the opposite sign punches a hole exactly where
the ink should be thickest.

**Coverage accumulates in float.** Four subsamples worth 255/4 = 63.75 each,
truncated to 63, add up to 252 - so nothing in the image is ever quite solid.
That is invisible in a screenshot. It is very visible in the test that asks
whether a 48 px icon was drawn at 48 px or stretched from 16, because the
answer to that is the count of fully covered pixels, and it was zero.

`tests/test_svg.cpp` reads pixels, not structures - a parser that returns the
right number of shapes and draws nothing is precisely the failure the font
had. Is there ink, is it where the path said, is the hole a hole, does an arc
bulge past its chord, does `fill="none"` mean none in both the attribute and
the `style` spelling, and is **every one of the built-in icons a drawing
rather than a blank or a solid block**.

### UI - `include/dai_ui.h`

Immediate mode: no widget tree, no retained state, no callbacks. A button is an
`if`. The reason is the same one the whole engine is built on - the simulation
is a pure function of state and input, and a retained UI tree would be a second
source of truth that can disagree with it (menus showing stale values after a
rollback is exactly that bug). Here the interface is rebuilt from the state
every frame, so it cannot drift.

```c
dai_ui_begin(ui, w, h, &input);
dai_ui_panel_begin(ui, 24, 24, 320, 180, "Daidalos");
dai_ui_label_fmt(ui, "%u bodies, %.1f ms", stats.bodies, stats.avg_step_ms);
if (dai_ui_button(ui, "Restart")) restart();
dai_ui_slider(ui, "Volume", &volume, 0.0f, 1.0f);
dai_ui_image(ui, icons, 32, 32, 0.0f, 0.0f, 0.25f, 0.25f, 0xFFFFFFFF);   // sprite
dai_ui_panel_end(ui);
dai_ui_end(ui);
```

The UI emits **vertices, not draw calls**: `dai_ui_draws()` returns batches of
triangles with a texture each, and `dai_render_ui()` draws them in one screen
space pass. Text and sprites are the same geometry - glyphs are white with
coverage in alpha, sprites bring their own colour, the vertex colour tints
both. A texture change starts a new batch, so an icon atlas costs one batch no
matter how many icons come out of it.

`tests/test_ui.cpp` covers the interaction rules that are easy to get subtly
wrong: a click fires **on release, over the widget, exactly once**; pressing
inside and releasing outside does **not** fire; a slider clamps when dragged
past its ends; a sprite in the middle of a panel splits the batches; and the
finished frame really does contain lit glyph pixels.

### Particles - `include/dai_particles.h`

Presentation, not simulation - deliberately. Sparks and smoke would otherwise
have to be snapshotted, rolled back and re-simulated, multiplying the cost of
every rollback by the number of pretty effects on screen, and giving an effect
the ability to desync a multiplayer session. They are fed by the same events
the audio layer consumes, so a rolled back explosion cancels its sound and its
sparks together.

Emitters are data (rate, lifetime, cone, gravity, drag, size and colour curves,
blend mode, texture atlas), each with its own seeded RNG so a replay looks
identical. `dai_render_particle_atlas` points the pass at a cols x rows sprite
sheet; emitters either pick a random cell per particle or walk the cells as a
flipbook over the particle's life. With no atlas the shader draws its own soft
dot, so nothing needs a texture to look right. Drawn as
instanced camera facing billboards after the opaque pass: depth tested, no
depth write, premultiplied alpha so alpha and additive share one pipeline.

```bash
DAI_SHADER_DIR=shaders ./build/particles_demo 6 /tmp
```

### Renderer - `include/dai_render.h`

Vulkan 1.3 with dynamic rendering: no `VkRenderPass`, no `VkFramebuffer`.
Renders offscreen and reads the frame back, so it runs headless (here on Mesa
lavapipe, on a GPU through the identical code path).

- **meshes**: box, sphere, capsule, cylinder, cone, plane, plus
  `dai_render_mesh_create` for your own vertices and `dai_render_mesh_load_obj`
  for Wavefront OBJ. Instances are sorted by mesh, one draw call per mesh.
- **capsules**: one mesh serves every proportion - the caps are pushed apart in
  the vertex stage by the instance's `param`.
- **materials**: glTF 2.0 metallic-roughness - base colour, ORM (occlusion /
  roughness / metallic packed), tangent space normal map, emissive, alpha
  cutoff. Cook-Torrance GGX. Tangent frame from screen space derivatives, so
  assets never need baked tangents.
- **uv transform**: per axis tiling and an offset, on the material *and* per
  instance. `uv_offset` animated per frame is a scrolling conveyor belt, a
  river or lava; the instance offset is added to the material's, so a hundred
  belts share one material and still run out of phase. The instance's
  `uv_scale` overrides the material's when it is non zero, and a zero
  initialised instance therefore changes nothing.
- **textures**: PNG loaded by the engine's own DEFLATE decoder, full mip chains
  built on the GPU, sRGB vs linear decided by the slot rather than the artist.
- **lighting**: directional sun, hemisphere ambient, Cook-Torrance GGX,
  **3 cascaded shadow maps** (2048² each, 3×3 PCF, texel snapped so edges do
  not crawl), squared distance fog, procedural sky with a sun disc, ACES
  tonemapping, 4× MSAA.
- **window**: three backends behind the same four functions - **X11**,
  **Wayland** (xdg-shell) and **Win32** - selected with `DAI_WINDOW=`.
  Presenting is a blit of the finished offscreen frame, so the headless tests
  and the on screen build run identical code.
- **output**: `dai_render_write_png` (no zlib dependency) and `write_ppm`.

Conventions, pinned down by the visual tests: right handed, +Y up, +X right on
screen, front faces are the outside of a mesh, `scale` is a half extent for
boxes and a radius for spheres.

---

## Looking at the output

A renderer that runs is not a renderer that is correct. Two tools exist for
exactly that difference.

### `tests/test_render_visual.cpp` - 29 assertions on pixels

Every test renders a scene whose correct result can be computed on paper, reads
the pixels back and checks them:

| # | what it pins down |
|---|---|
| 1 | cube silhouette matches the projection maths to 6% |
| 2 | +Y is up on screen, +X is right |
| 3 | depth test: the near box occludes the far wall |
| 4 | camera inside a closed box sees nothing (back face culling) |
| 5 | a slab lit from above is brighter than its underside |
| 6 | a sphere fills 78.5% of its bounding box, not 100% |
| 7 | a cube rotated 45° is √2 wider |
| 8 | the same scene twice is bit identical |
| 9 | the side facing the sun is the bright one |
| 10 | a caster darkens the floor (measured by difference) |
| 11 | capsule proportions follow `param` |
| 12 | the sky has a gradient and a visible sun |
| 13 | image quality with shipping defaults: contrast, exposure, clipping, saturation, detail |

Test 13 is the "the pictures look odd" regression: it fails if the frame turns
into flat grey soup again.

### `tools/look.py` - the renderer's mirror

```bash
python3 tools/look.py frame.png --cols=110          # ASCII luminance + colour map
python3 tools/look.py frame.png --stretch           # normalise contrast first
python3 tools/look.py frame.png --crop=0.3,0.2,0.7,0.8
```

Prints an ASCII view of the frame plus objective numbers (exposure, p2..p98
contrast, clipping, detail, saturation). It exists so the rendered image can be
inspected without a display - by a human over ssh, or by an agent that has to
check its own output instead of claiming it looks fine.

`tools/diag_projection.cpp` and `tools/diag_shading.cpp` isolate camera maths
and shading when a test fails and you need to know which half is lying.

---

## Bugs these tools have already caught

- **the flat cube**: box faces were built without the face centre offset, so
  every face collapsed onto the origin plane. The cube rendered as a flat cross
  - it looked like planks sticking out of objects. Test 1 measured it as a 17%
  size error before anyone looked at a picture.
- **inside out world**: `frontFace` was set to clockwise. Back face culling then
  kept the far side of everything, so every object was lit from the wrong side
  and solid crates looked like open shells. Tests 4 and 9 pin it now - and test
  4 had to be rewritten, because comparing against the corner pixel let it pass
  while the camera was staring at a wall.
- **the camera gesture that never ended**: changing mouse button mid-drag (middle
  to alt+left, say) only re-anchored on "pressed while idle", so the old mode
  kept running and the first frame jumped by the distance between the two
  presses. Any mode change now re-anchors.
- **X11 wheel buttons in the button mask**: X reports the wheel as buttons 4 and
  5, which meant every scroll looked like a middle-button drag. They now feed a
  wheel accumulator instead - polling a bit could never work anyway, since a
  wheel click presses and releases inside one frame.
- **the leaking layout row**: `dai_ui_row` had no end, and `panel_end` did not
  clear it. A toolbar that finished with a row left every later panel laying its
  widgets out sideways, so the inspector looked empty - its fields were stacked
  off the right edge. Found by a panel test that could not click a field that
  was plainly visible in the screenshot.
- **scrubbing to tick zero**: a snapshot holds the state at the *start* of its
  tick, before that tick's commands. The bodies created by the first sync belong
  to that tick, so seeking to it deleted the entire scene. The timeline now
  starts one tick later.
- **the sun pointing the wrong way**: `dai_render_sun` takes the direction
  *towards* the sun; the first gizmo screenshot passed the direction the light
  travels. Every visible face then fell back to ambient only and the scene came
  out flat and lifeless. Nothing asserted on it - only looking at the picture
  found it.
- **grey soup**: Reinhard tonemapping plus a heavy ambient term produced frames
  with no black, no white and almost no colour. Now ACES, lower ambient, and
  test 13 to keep it that way.

---

## Breaking things

`tools/daifracture` cuts a mesh into pieces once, at build time:

```sh
./build/daifracture assets/models/stone.glb assets/models/stone_pieces.glb 12 42
#                   in                       out                          N  seed
```

Voronoi cells clipped out of the source geometry - scatter N points in the
mesh's box, keep for each point everything closer to it than to any other,
which is an intersection of half spaces. Each cut face is capped so a piece is
a closed solid the physics can accept, not a hollow shell.

Baked rather than solved live, for two reasons. Cutting geometry is expensive
and the cost lands exactly when the frame is already busy - something just got
hit. And a runtime cut would be a second source of geometry that has to match
across a rollback; a baked file is the same on every machine because it was
computed once. The seed is an explicit parameter, so re-running the command
reproduces the same rubble.

The test does not look at a screenshot. It sums the signed volume of every
piece and requires the original volume back: an unclosed cut face leaks volume,
overlapping cells give too much, a gap gives too little. A 2x2x2 box broken into
2, 5, 8, 20 and 50 pieces returns 8.000000 every time, as does an off-centre
8x0.5x2 slab. The same seed twice is bit identical; a different seed is not.

At runtime nothing needs to be cut. The pieces are an ordinary model, so the
glue is short - swap one body for N when the hit is hard enough:

```c
for (uint32_t i = 0; i < n_contacts; ++i) {
    if (contacts[i].impulse < break_threshold) continue;
    dai_node broken = contacts[i].node_a;              /* the stone */
    dai_vec3 at = contacts[i].position;
    dai_doc_remove(doc, broken);
    dai_node pieces = dai_assets_instantiate(assets, doc, "models/stone_pieces.glb", parent);
    /* one body per piece; push each away from where it was hit */
    for (dai_node p = first_child(doc, pieces); p; p = next_sibling(doc, p))
        dai_body_add_impulse(world, body_of(p), scaled_away_from(at, centre_of(p)), at);
}
```

Limits worth knowing before using it on a character model: the cap assumes the
cut cross section is convex, which holds for a convex mesh and for any Voronoi
cell, and is only approximately true for a concave one - so a hollow or L shaped
model can get wrong inner faces. UVs are not carried onto the new faces; a
broken stone wants rock on the outside and a flat colour inside, and inventing
coordinates would look worse than leaving them at zero. Materials are not
written - the pieces come out as plain geometry.

Writing glTF is new: `dai_gltf_write` in `src/dai_gltf_write.cpp` produces a
self contained `.glb` with positions, normals and indices, one node per mesh.
The parser it reads back with is the same one the engine uses, shared through
`src/dai_gltf_common.hpp`, which is a header precisely so the tool half
(`dai_gltf_geom.cpp`, geometry as plain arrays) can be linked without Vulkan.
`daifracture` and `test_fracture` build and run on a machine with no GPU.

## Staying current

The editor is handed to people who will not rebuild it, so it updates itself.
`include/dai_update.h` is the whole of it: fetch a manifest, compare versions,
hash the local files, download what actually differs, verify, install.

Publishing is generated, never typed - a manifest promising a hash the file does
not have is a manifest the updater will correctly refuse:

```sh
tools/dai_publish.py 0.2.0 https://jarvis.fleitec.com/dai/ \
    build/daidalos_editor.exe shaders.pack > dai_version.json
```

The client side is four calls:

```c
dai_update_sweep(dir);                        /* delete last update's leftovers */
dai_update_info u;
if (dai_update_check(MANIFEST_URL, DAI_VERSION, dir, &u, err, sizeof err) == DAI_OK
    && u.needed_count) {
    if (dai_update_download(&u, dir, err, sizeof err))       /* staged, verified */
        dai_update_commit(&u, dir, "daidalos_editor.exe", err, sizeof err);
}
```

Three properties it is built around, each one a way this feature usually goes
wrong:

*Verified before installed.* A download is hashed and compared to the manifest
before it is allowed near the install directory. Replacing a working editor with
a truncated one is worse than being out of date.

*Staged, then committed.* Files land as `<name>.new` and nothing existing moves
until every one of them is present and correct. Losing the connection halfway
leaves the old build running, not half of each.

*The rename trick.* Windows will not overwrite a running `.exe` but will rename
it, so the running file steps aside to `<name>.old`, the new one takes its
place, and the leftover is swept at the next start.

The version number only says a release happened; the *hashes* decide what gets
downloaded, so a release that changed one file transfers one file.

Testing an updater means testing the failures, so the transport is injectable
(`dai_update_set_fetch`) and `test_update` drives the whole flow against bytes
in memory: corrupt download refused with the install untouched, unreachable
server reported rather than fatal, interrupted download refused at commit, and a
manifest naming `../escaped.txt`, `/etc/passwd` or `sub/dir.txt` dropped before
any of them becomes a path. SHA-256 is checked against the published vectors and
at lengths 55/56/57/63/64/65/119/120, where message padding either works or
quietly does not. 53 checks.

It has also been run for real: `dai_publish.py` writing a manifest, a local HTTP
server, the built in transport fetching it, and the installed binary coming out
byte identical to the one that was served, with the previous one preserved as
`.old`. The Windows half (WinHTTP, no extra DLL) is compiled on every build by
the mingw cross check, so it cannot rot unnoticed.

## Windows

Daidalos now runs on Windows, on real hardware, and the evidence is a frame that
came back off an RTX 3060:

```
daidalos win_smoke
  device: NVIDIA GeForce RTX 3060 (Vulkan 1.4.341)
  window: 1280x720
  frames: 120 drawn, 0 failed, 1.26 ms average
  coverage: 100.0% of the frame is lit
  wrote: shot.ppm (1280x720)
```

`./build_win.sh` cross compiles from Linux with mingw-w64; the Windows machine
only has to run the result. That is deliberate - the build is a bash script, and
installing a 250 MB SDK on the target to prove a renderer works is the wrong
trade. Two pieces make it work with nothing installed on either side:

*Vulkan without the SDK.* Windows has no `libvulkan.so` to link against, it has
`vulkan-1.dll`, which the graphics driver already put in System32. An import
library generated from a name list is enough to link against it
(`thirdparty/win/vulkan-1.def`, regenerated by `tools/make_vulkan_def.sh` from
the calls the engine actually makes). The shipped `.exe` needs no runtime, no
redistributable and no SDK. It is statically linked, so it is one file.

*Jolt for Windows.* `tools/build_jolt_win.sh`, once. Two traps in there, both
already sprung and documented in the script: Debian's default mingw uses the
win32 thread model, which has no `std::mutex`, so Jolt does not compile at all -
the `-posix` variants of the same compiler do. And Jolt turns on a DX12 compute
backend for Windows targets that wants `dxcapi.h` from the DirectX shader
compiler, which mingw does not ship and the engine does not use.

The bug worth remembering is the third one. The first run opened the window,
printed the device name, and then died inside `dai_create`. The cause was that
`libJolt.a` had been built with `JPH_PROFILE_ENABLED` and `JPH_DEBUG_RENDERER`
and the calling code had not - Jolt's headers change structure layout on those,
so it links cleanly and crashes on the first call. `build_win.sh` now uses
exactly the defines and `-m` flags the library was built with, and says where to
read them back out of if it ever needs checking. A renderer that starts and a
physics engine that crashes look identical from the outside until something
prints between them.

`examples/win_smoke.cpp` is what produced the output above: it opens a window,
drops 24 bodies onto a floor, presents 120 frames, reads the last one back and
writes it out, and fails if the frame comes back black - a window that presented
nothing and a frame that rendered nothing would otherwise both look like
success.

### The editor runs on Windows too

It used to ask for keys as X11 keysyms - fine on X11, meaningless on Windows,
where the window procedure is handed virtual key codes. The fix was not an
`#ifdef` around every key: **key codes are now the engine's own vocabulary**
(`dai_key` in `dai_render.h`), and each backend maps its own numbers onto them.

The values *are* the X11 keysyms, which for letters and digits is just ASCII, so
the X11 backend needs no translation at all and Win32 maps onto it. That is a
load bearing claim and an invisible one - if a value drifted nothing would fail
to compile, X11 would just quietly stop matching - so `test_keys` asserts all 32
against the real X11 headers on every build.

Proving the Windows half needed the keys actually pressed, so `win_keytest.exe`
posts real `WM_KEYDOWN` messages to its own window and reads them back through
`dai_window_key_down`. Run on the RTX 3060 machine: **18 checks, 0 failures** -
every editor binding, both sides of shift/ctrl/alt reported from the side-less
virtual key, and an unmapped key (F12) confirmed to touch nothing, because the
key slot is a hash and a collision would silently press W.

## Three bugs the first Windows session found

Running the editor on someone else's machine surfaced three things a headless
test suite could not. Two of them were in the engine and had been wrong on Linux
all along.

**Editing a node painted it black.** A node with no colour of its own gets one
from the palette when it spawns; the document still holds zero, meaning "none
chosen". The sync pushed that zero back on every update - so moving a crate
turned it black, then the next one, then the next. Nothing crashed and nothing
failed to build, the scene just went dark one object at a time. The sync now
leaves the colour alone unless the document actually specifies one, and
`test_doc` moves an uncoloured node and demands the palette colour survive.
Reverting the fix makes that test fail with `painted it black (0.00 0.00 0.00)`,
which is the point of writing it.

**The gizmo was offset from the mouse.** The finished frame is blitted onto the
window and stretched to fit, so window pixels and renderer pixels only agree
when the window is exactly the render resolution. `dai_window_mouse` handed back
window pixels, the editor hit tested against what it had drawn, and the miss
grew with the window - maximise it and the gizmo moves out from under the
cursor. The mouse is now reported **in the space the frame was drawn in**, in
all three backends. Handing back raw window pixels and expecting every caller to
divide is how that bug gets written once per program.

**The interface had no text.** The editor loaded
`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` - a hard coded Linux path, so
on Windows it drew an interface of blank boxes. `dai_font_load_ui` now tries
`$DAI_FONT`, then the Segoe/Tahoma/Arial faces Windows ships, then the DejaVu
and Liberation ones Linux ships, and says what it tried when none of them exist.

The mouse fix is verified the same way the keyboard was - by doing it, not by
asserting it. `win_keytest` resizes its own window to half, equal and double the
render resolution, posts a pointer position, and checks what comes back:

```
  window  320x200   pointer (100, 50) -> (100, 50), expected (100, 50)  ok
  window  640x400   pointer (200,100) -> (100, 50), expected (100, 50)  ok
  window  160x100   pointer ( 50, 25) -> (100, 50), expected (100, 50)  ok
ok: 21 checks, 0 failures
```

## What an ultrawide monitor exposed

Three more, all from one screenshot of the editor filling a 3440x1440 screen,
and all three the same root cause wearing different clothes.

The renderer drew at a fixed 1440x810 and the finished frame was blitted onto
the window, stretched to fit. On a 21:9 monitor that meant:

- **Everything was distorted.** A 16:9 frame smeared across a 21:9 screen -
  boxes came out wider than they are.
- **The text was a mess.** 17 pixel glyphs scaled up 2.4x are a smear of white,
  which is what "brutal verbuggt" looked like. The font was innocent - dumping
  Segoe UI and DejaVu Sans as ASCII art shows both rasterising cleanly. Worth
  checking before rewriting a rasteriser.
- **The interface laid itself out for the wrong size.** The UI uses window
  pixels, the renderer used its own - so the panels were sized for one canvas
  and drawn into another.

`dai_render_resize` fixes all three by removing the mismatch: the renderer
follows the window, so window, framebuffer and interface are one resolution and
nothing is scaled at all. Dynamic rendering makes it cheap - no render pass and
no framebuffers to rebuild, just three images and the readback buffer. Failing
to allocate at the new size restores the old one rather than leaving a renderer
with no targets.

The editor calls it when `dai_window_size` changes, along with
`dai_editor_camera_viewport` so picking and the gizmo know the new size. That is
deliberately *not* `dai_editor_camera` with the same eye and target: it would
recompute the orbit angles and the camera would jerk every time the window was
resized.

**Dragging is now live.** The gizmo wrote to the document on every mouse move
but the scene was only synchronised in `dai_editor_drag_end`, so an object sat
still while being dragged and teleported on release - it felt like dragging a
number rather than a thing. `dai_editor_drag_update` pushes to the scene every
frame now. Move, rotate and scale each have their own early returns, so the push
lives in a wrapper around them rather than at the end of whichever branch was
remembered. `dai_doc_sync_apply` only touches nodes whose revision changed, so a
drag costs the handful that are actually moving.

`tools/editor_shot` takes a resolution now, so the ultrawide case can be
rendered here rather than only on the desk where it broke.

## The text was boxes, on every platform, for a long time

Worth writing down because of how it survived.

The UI batches draws by **texture**. The renderer, asked to bind one, scanned
the material table for a material that happened to use that texture and fell
back to material 0 when none did. A font atlas is uploaded directly and belongs
to no material - so it *always* hit the fallback, and every glyph was sampled
from the default white texture. Text rendered as rows of solid white boxes.

Nothing detected it. The UI tests count vertices, and the vertex count was
correct. `editor_shot` wrote PNGs that were never opened. The renderer reported
no error, because binding the wrong texture is not an error. It was found by a
user, in a photograph of his monitor.

Two things came out of that:

Textures now get **their own descriptor set** (`vk_texture_set`), built on first
use and cached on the entry, so a draw that names a texture binds that texture.
No table scan, no fallback.

And `test_ui_text` looks at the pixels. It draws a row of H's and measures what
fraction of the text's bounding box is lit, plus how often the busiest scanline
crosses between light and dark. Glyphs are mostly holes; boxes are not:

```
  broken:  text box 204x14 - 94.1% lit,  25 transitions   FAIL
  fixed:   text box 512x13 - 53.9% lit,  64 transitions   ok
```

Reverting the fix makes it fail with *"text covers 94.1% of its own box - these
are boxes, not glyphs"*. A test that passes whether or not the feature works is
not a test, and every UI test in this repo had that shape until now.

The lesson generalises past this bug: **a renderer test that never looks at the
picture is measuring the wrong thing.** Assert on the pixels, or find out from a
photograph.

## What is still missing

Kept honest: things listed here are genuinely absent, and things that got built
have been struck from the list rather than left in to look modest.

Assets are referenced by path (`asset models/crate.glb` in the scene file),
resolved through Mnemosyne and the glTF importer, and a scene with imported
models saves and reopens. A file with several objects draws as several pieces
on one scene node, or instantiates as a tree of nodes with one body per piece;
reloading releases the old meshes and textures and reuses the slots, so an
editing session does not grow. What is *not* here yet: no asset browser, and a
piece's collision box comes from its bounding box with no offset, so a Blender
object whose origin sits outside its mesh gets a box in the wrong place.

Editor:
- No box select; multi-selection is click by click.
- Joints and compound bodies are deliberately not serialised (see dai_doc.h).
- Prefab overrides do not exist: an instance's children come from the file, and
  editing one would be silently discarded on the next reload. Moving and
  renaming the instance root works, because that is a normal node.
- No copy/paste between scenes.
- The asset browser lists and places; it has no thumbnails, no folder tree and
  no search.
- The text field has no caret movement and no selection - typing and backspace.

Engine:
- `dai_contact::impulse` is the impact, not the resting load - a crate sitting
  on the floor reports ~0 rather than its weight, and rotational inertia is
  left out so an off centre hit reads slightly high.
- No morph targets and no animation state machine (pose blending exists).
- Snapshots store the full physics blob per tick, so a large scene pays for the
  rollback ring in memory whether it needs it or not.
- No network transport. Rollback and tick stamped input exist; the wire does not.
- Particle atlases are one texture for the whole pass, not one per emitter.
- Fracture caps assume a convex cut cross section, so a concave model can get
  wrong inner faces; and `dai_gltf_write` emits geometry only, no materials.
- The editor demo is X11 only: it reads keysyms directly, so the Windows build
  currently ships win_smoke and not the editor.

Absent entirely: terrain, navmesh/AI, audio authoring in the editor, an asset
browser, and any way to export a finished scene as a standalone game.
