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
DAI_SHADER_DIR=shaders ./build/test_render_visual /tmp   # 33 visual assertions
./build/test_image /tmp/pngfix                           #  9 PNG/DEFLATE assertions
DAI_SHADER_DIR=shaders ./build/test_gltf assets/test /tmp # 15 import assertions

# a real window, on a machine without a display:
Xvfb :77 -screen 0 1280x720x24 &
DISPLAY=:77 DAI_SHADER_DIR=shaders ./build/test_window
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
- **window**: X11 + `VK_KHR_swapchain`. Presenting is a blit of the finished
  offscreen frame, so the headless tests and the on screen build run identical
  code and a Win32/Wayland port is one file.
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
- **grey soup**: Reinhard tonemapping plus a heavy ambient term produced frames
  with no black, no white and almost no colour. Now ACES, lower ambient, and
  test 13 to keep it that way.

---

## What is still missing

- No particle system yet.
- No skinning, no animation system.
- Window backend is X11 only (Win32/Wayland would be the same file again).
- Shadow cascades are fitted per frame with no caching, so a very large scene
  re-renders all three every frame.
- Contact impulses are not filled in yet; snapshots store the full blob per tick.
