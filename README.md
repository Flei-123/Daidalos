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
  cutoff, uv scale. Cook-Torrance GGX. Tangent frame from screen space
  derivatives, so assets never need baked tangents.
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

## What is still missing

Kept honest: things listed here are genuinely absent, and things that got built
have been struck from the list rather than left in to look modest.

**The big one - assets are not in the scene file.** A node stores a mesh *index*,
not a reference to anything on disk, so a scene with imported glTF models cannot
be saved and reopened: the index points somewhere else next run. What is needed
is a path based asset reference (mesh, texture, material) and a library that
resolves it. Until then the editor can only author primitives.

Editor:
- No box select; multi-selection is click by click.
- Joints and compound bodies are deliberately not serialised (see dai_doc.h).
- No prefab or instance concept - a hundred identical crates are a hundred
  independent copies.
- No copy/paste between scenes.
- The text field has no caret movement and no selection - typing and backspace.

Engine:
- `dai_contact::impulse` is always 0. The contact point and normal are real, the
  strength is not, so "how hard did that hit" cannot be answered yet.
- No morph targets and no animation state machine (pose blending exists).
- Snapshots store the full physics blob per tick, so a large scene pays for the
  rollback ring in memory whether it needs it or not.
- No network transport. Rollback and tick stamped input exist; the wire does not.
- Particle atlases are one texture for the whole pass, not one per emitter.
- The Win32 backend is cross compiled with mingw-w64 but has never been run on
  Windows from here: no Windows machine in this setup has a compiler and a
  Vulkan loader. X11 and Wayland are both tested headless.

Absent entirely: terrain, navmesh/AI, audio authoring in the editor, an asset
browser, and any way to export a finished scene as a standalone game.
