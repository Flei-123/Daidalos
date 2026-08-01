# Materials and the Blender round trip

The goal of this document is one sentence: **getting art from Blender into
Daidalos should be an export, not a project.**

The usual pain (shader graphs that do not exist in the engine, baking sessions,
tangent conventions that disagree, colour spaces that silently shift) all comes
from the same root cause: the engine and the DCC tool try to share an
*authoring* model. They should share a *delivery* model instead.

---

## The rule

**Node graphs are an authoring tool. The engine consumes maps.**

Blender's Principled BSDF is not a node graph problem - it maps 1:1 onto the
glTF metallic-roughness model. So:

- if a material is Principled BSDF + textures + values -> **export, done**
- if a material uses procedural nodes (noise, gradients, mixes) -> **bake once**
  in Blender to a texture, then export. The bake is a Blender operation, not an
  engine feature, and it can be scripted.

That is the entire pipeline. There is no importer setting to get wrong.

---

## What a material is

Exactly four maps and a handful of scalars - the glTF 2.0 metallic-roughness
model, which is also what Blender, Substance, Godot, three.js and Unreal's
import path all speak:

| slot | colour space | channels | comes from |
|---|---|---|---|
| **base colour** | sRGB | RGB + A | Base Color, Alpha |
| **ORM** | linear | R = ambient occlusion, G = roughness, B = metallic | Roughness, Metallic, baked AO |
| **normal** | linear | tangent space XYZ | Normal input |
| **emissive** | sRGB | RGB | Emission |

Scalars (`base_color`, `metallic`, `roughness`, `normal_strength`,
`occlusion`, `emissive`, `alpha_cutoff`) **multiply** their map. A
material can therefore be:

- pure numbers (no textures at all - red rough plastic is three floats),
- pure maps,
- or maps tinted by numbers.

`dai_material_desc_default()` gives the neutral values, so a partially filled
description is never a broken one.

### The UV transform

Two `dai_vec2` on the material, `uv_scale` and `uv_offset`:

```c
md.uv_scale  = { 4, 1 };      /* four repeats along U, one along V */
md.uv_offset = { t * 0.5f, 0 };
dai_render_material_update(r, mat, &md);   /* per frame, no new material */
```

`uv_scale` is per axis on purpose. A conveyor belt is long and narrow and needs
`4x1`; a single float cannot express that, which is what the first version got
wrong. The sampler wraps, so an offset of 1.0 is exactly one full repeat and the
value can grow forever without a modulo - though wrapping it keeps float
precision sane over a long session.

The same two values exist on `dai_render_instance`:

| | material | instance | result |
|---|---|---|---|
| `uv_scale` | 4,1 | 0,0 | 4,1 - the instance says nothing, the material wins |
| `uv_scale` | 4,1 | 2,2 | 2,2 - the instance overrides |
| `uv_offset` | 0.25,0 | 0.25,0 | 0.5,0 - offsets **add** |

Adding rather than overriding is the whole point of the offset: one belt
material, a hundred belts, each at its own phase, still one draw call because
nothing about the material changed. Tiling overrides instead, because two
objects wanting different tiling from the same material is a different object,
not a different phase.

It costs 16 bytes per instance in the vertex stream and one `vec4` in the push
constant block (80 of the guaranteed 128 bytes used), so there is no extra
buffer traffic and no extra descriptor set.

### Why ORM is one texture

Occlusion, roughness and metallic are single channel data. Shipping them as
three greyscale files wastes three samplers, three cache lines and three
uploads. glTF already defines the packing (G = roughness, B = metallic, and the
occlusion map's R), Blender's exporter already produces it, and the engine
reads it as one map. Nothing to configure.

### Why there are no tangents in the asset

Tangent vectors have to match the normal map's convention exactly, and every
tool disagrees about handedness. Daidalos builds the tangent frame in the
fragment shader from screen space derivatives instead. Costs a few ALU ops,
removes an entire class of "the normal map looks inverted on one axis" bugs,
and means a mesh with only position/normal/uv is always enough.

### Colour space is not the artist's problem

`dai_render_texture_create(..., srgb)` picks `VK_FORMAT_R8G8B8A8_SRGB` or
`..._UNORM`. Decoding happens in the sampler, in hardware, before filtering -
which is also the only correct place for it. The importer knows which slot is
which, so base colour and emissive get sRGB, ORM and normal get linear, and
nobody has to remember.

Visual test 14 checks exactly this: the same bytes uploaded as sRGB must render
darker than uploaded as linear.

---

## The round trip, concretely

```bash
# in Blender: File > Export > glTF 2.0 (.glb), defaults are fine.
# headless, which is how the test fixture is produced:
blender --background --python tools/make_testscene.py

# in the engine:
DAI_SHADER_DIR=shaders ./build/model_viewer scene.glb 4 /tmp
```

```c
dai_model *m = dai_gltf_load(renderer, "scene.glb", err, sizeof(err));
dai_render_instance inst[512];
uint32_t n = dai_model_instances(m, inst, 512, offset, rotation, scale);
dai_render_frame(renderer, inst, n);
```

What crosses the boundary: meshes (indexed triangles, position/normal/uv),
materials, PNG textures (embedded in the GLB or beside the .gltf), and the node
hierarchy flattened to world space TRS.

`tests/test_gltf.cpp` runs against a GLB **Blender actually exported** - five
objects, five materials, a packed colour grid texture, a non uniformly scaled
ground plane - and checks node count, material count, triangle count, that Y is
up after Blender's Z-up conversion, that the 12x scaled plane kept its scale,
that the separate .gltf + .bin + external PNG variant agrees with the GLB, and
that the frame actually changes when the model is drawn.

---

## Deliberate limits

- **JPEG and KTX2 textures are not decoded.** glTF allows them; the engine
  ships its own PNG/DEFLATE decoder and no more. A missing decode yields a
  white texture and a working material, never a failed load.
- **No skinning, morph targets or animation yet.** They parse past cleanly.
- **Shear does not survive.** A node's world matrix is decomposed to
  translation/rotation/scale; Blender does not export shear, and an instance
  transform has nowhere to put it.
- **One UV set.** `TEXCOORD_0` only.

---

## When you really do need a custom shader

The escape hatch is a per material fragment shader, not a node graph in the
engine. The material stays data; only the code that consumes it changes. That
keeps the common path (99% of assets) boring and fast, and puts the complexity
where it belongs: in the one material that actually needs it.
