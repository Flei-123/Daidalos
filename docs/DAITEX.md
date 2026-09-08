# .daitex - the procedural texture baker

A `.daitex` is a small node graph, written as JSON, that bakes into the three
maps this engine already renders: **base colour**, **ORM** and **normal**. It
exists for one reason: Jarvis cannot open Blender, and a wall still needs a
surface.

It does not change what a material is. From `docs/MATERIALS.md`:

> **Node graphs are an authoring tool. The engine consumes maps.**

That rule is why the baker writes PNGs beside the graph and then gets out of
the way. Nothing in the renderer knows a `.daitex` exists; it draws
`beton_basecolor.png` exactly like a texture that came out of Substance.

```
projects/Untitled/assets/textures/
    beton.daitex             <- the graph, in git, 1.8 kB
    beton_basecolor.png      <- baked; sRGB
    beton_orm.png            <- baked; linear, R=AO G=roughness B=metallic
    beton_normal.png         <- baked; linear tangent space, +Y up
```

The four graphs that ship are `raufaser_wand` (woodchip wallpaper),
`pvc_boden` (sheet vinyl), `rostblech` (rusted sheet metal) and `beton`
(concrete) - the four surfaces the ground floor of INNEN is made of.

---

## The contract

**Deterministic.** Same graph plus same seed = bit identical PNG bytes, on
every machine and every run. There is no clock, no `rand()`, no thread and no
libm transcendental in the evaluation: the only randomness is an integer hash
of the lattice coordinate and the seed, integer powers are repeated
multiplication, and the one place that needs `x^e` (the levels gamma) uses
`daitex::pow_pos`, written out in floats. `tests/test_daitex.cpp` bakes each
graph twice and compares the FILES byte for byte.

**Tileable.** Every generator is periodic over the unit square - the noise
lattices wrap, the blur wraps, the Sobel wraps - because the maps are
projected in world space by the triplanar material and a seam would land in
the corner of a room. The test measures the difference across the wrap against
the difference between two neighbouring columns and requires the first to be
no larger than the second.

**Baked on load and on change, never per frame.** The editor's seam
(`include/dai_daitex_host.inl`) walks the project's Assets folder twice a
second, bakes a graph whose mtime moved or whose `_basecolor.png` is missing,
and bakes at most one per poll. The console line it prints carries the digest:

```
daitex: baked projects/Untitled/assets/textures/beton.daitex  256x256  digest 7d865afad2d81a24
```

Two machines are compared by reading that line, not by diffing a PNG.

---

## The file

```json
{
  "daitex": 1,
  "name": "Concrete",
  "resolution": 256,
  "seed": 1907,
  "tiling_m": 3.0,

  "nodes": [
    { "id": "body", "type": "noise", "kind": "perlin",
      "params": { "cells": 4, "octaves": 5, "gain": 0.5, "lacunarity": 2 } },
    { "id": "grey", "type": "colorramp", "in": "body", "stops": [
      { "t": 0.0, "rgb": [ 0.318, 0.316, 0.308 ] },
      { "t": 1.0, "rgb": [ 0.612, 0.608, 0.596 ] } ] },
    { "id": "nrm", "type": "normal", "in": "body", "params": { "strength": 2.0 } }
  ],

  "out": {
    "base_color": "grey",
    "roughness": 0.85,
    "metallic": 0.0,
    "normal": "nrm"
  }
}
```

* `resolution` is clamped to 16..2048, `seed` is what every node hashes
  against, `tiling_m` is how many metres one tile covers - the number the
  triplanar material's tiling size is set from.
* **A node may only read the nodes above it.** That is what makes the graph a
  DAG without a topological sort, and what makes the evaluation order the same
  on every machine. Inputs are `"in"`, `"in_b"` and `"fac"`.
* An `out` slot is either a node id or a plain number: metal that is metal
  everywhere does not need a node to say so. A slot nobody wrote gets the
  neutral value (white AO, 0.5 roughness, 0 metal, the flat normal).
* Every node evaluates to an RGB image. A height, a mask and a colour are the
  same kind of thing; a "height" is just an image whose red channel is read.

### The nodes

| type | kind | params | what it is |
|---|---|---|---|
| `noise` | `value` `perlin` `worley` | cells, octaves, gain, lacunarity, seed | fbm over a periodic lattice |
| `gradient` | `x` `y` `radial` `diag` | from, to | a ramp |
| `brick` | (free) | rows, cols, gap, shift, bevel, variation | courses with mortar, every other row offset |
| `checker` | | rows, cols | 0 and 1 |
| `mix` | `mix` `add` `sub` `mul` `min` `max` `screen` `overlay` | factor | `in` and `in_b`, blended by `factor` or by the `fac` input |
| `levels` | | in_min, in_max, out_min, out_max, gamma | the contrast tool; out_min > out_max INVERTS |
| `blur` | | radius, passes | separable box, wrapped; three passes is a gaussian |
| `colorramp` | | (stops) | grey in, colour out |
| `normal` | | strength | Sobel of the height, +Y up, exact (128,128,255) where flat |
| `ao` | | radius, strength | eight directions, four steps, all fixed |
| `const` | | value / rgb | a number or a colour |

`include/dai_daitex_ranges.inl` is the one table of slider ranges. It is a
block of statements rather than a header because both the baker's clamp and
the inspector need it and the inspector - itself a block of statements inside
`asset_inspector_body` - cannot include anything.

---

## In the editor

* **Project panel.** A `.daitex` row shows its baked base colour as the
  thumbnail, so a folder of graphs is a folder of surfaces rather than four
  identical file icons.
* **Inspector.** The preview, the names of the three maps it writes, and every
  parameter of every node as an editable row - dragged like a slider, clamped
  to the range above, typed into when a number is meant exactly. `Save and
  bake` writes the file; the poll sees the newer mtime and re-bakes it, which
  is the same path a file edited outside the editor takes.
* **The panel edits the file's own text.** Each row knows the offset and the
  length of the literal it came from and writes the new number back into
  exactly that place - so formatting, key order and anything this build does
  not understand survive being edited.

## Using one in a material

The maps are ordinary files, so a `.daimat` points at them the way it points
at any other texture, and the triplanar switch is what makes them work without
a UV unwrap. The four that ship are in `projects/Untitled/assets/materials`,
one per graph:

```
daidalos-material 1
color 0.7 0.7 0.69
roughness 0.92
metallic 0
base_color_map textures/beton_basecolor.png
orm_map textures/beton_orm.png
normal_map textures/beton_normal.png
triplanar 1
triplanar_scale 3
triplanar_blend 4
```

`triplanar_scale` is the graph's own `tiling_m`: the pattern then appears at
the size it was authored at instead of at whatever the wall happens to be. The
scalars MULTIPLY their map (`docs/MATERIALS.md` again), so the numbers above
tint what the ORM already carries rather than replacing it.

## Where it lives

| file | what it is |
|---|---|
| `include/dai_daitex.h` | the baker: parse, evaluate, quantise, write. Header-only, because `build.sh` is frozen and names every translation unit it compiles. |
| `include/dai_daitex_ranges.inl` | the slider ranges, once |
| `include/dai_daitex_host.inl` | the editor/tool seam: watch, bake, preview |
| `src/dai_editor_ui_daitex.inl` | the inspector |
| `tests/test_daitex.cpp` | 349 checks, run by `tools/run_tests.sh` |
