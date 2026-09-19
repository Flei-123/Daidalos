# Post processing

The screen space chain that runs after the world and the UI are drawn:
bloom, chromatic aberration, vignette, film grain, scanlines and a hit flash.

It is the one thing a neon scene cannot get from better materials. A glow is
light that has already left the surface and spread across the sensor; no
per-object shader can produce it, because the pixel that lights up is not on
the object.

```c
#include "dai_render.h"

dai_postfx fx = {0};
fx.enabled         = 1;
fx.bloom_threshold = 0.65f;
fx.bloom_knee      = 0.30f;
fx.bloom_intensity = 1.10f;
fx.vignette        = 0.40f;
fx.grain           = 0.035f;
fx.aberration      = 0.0045f;
fx.scanlines       = 0.06f;
fx.frame_index     = frame_counter;   /* NOT the clock - see Determinism */

dai_render_postfx(r, &fx);
dai_render_frame(r, instances, count);
```

`dai_render_postfx(r, NULL)` switches it off again. The struct is copied and
stays in effect until it is changed.

---

## Off is off

`enabled = 0` is the default, and it is a contract rather than a default value.

Every visual test in this repository compares pixels against arithmetic, and
`.gauntlet-shots/` is full of reference images. A renderer that quietly began
adding grain would turn all of them red for a reason that is not a bug. So with
the chain off **no command from `src/rhi_vulkan_post.cpp` enters the command
buffer at all** - the readback still copies straight out of `color_rt`, exactly
as it did before the feature existed.

`tests/test_postfx.cpp` [1a] and [1b] hold that claim by digesting the whole
frame and comparing it against a frame rendered before `dai_render_postfx` was
ever called. [1c] then checks that a frame with the chain ON *does* differ -
otherwise [1a] would be passing because the chain does nothing.

---

## What it does, in order

The order is the order light actually meets a camera, and it is one pass:

| step | why it is where it is |
|---|---|
| chromatic aberration | the lens bends the colours apart before anything else sees them |
| bloom | the glow adds to what the lens delivered |
| flash | light in the scene, so it is added with the bloom, not after the lens |
| vignette | the barrel darkens the corners of whatever arrived |
| grain | the sensor adds noise to the image it captured |
| scanlines | the display's raster is the last thing in the chain |

Four separate passes would each cost a full read and a full write of the frame.
At 1280x720 that is 3.5 MB each way, 28 MB per frame moved to do arithmetic
that fits in a handful of registers. They are one shader - `shaders/post_comp.frag`.

### Bloom

Three passes, all at quarter resolution:

1. **Bright pass** (`post_bright.frag`) - `color_rt` -> `bloom_a`, with a soft
   knee. The threshold is a *knee*, not a cut: a hard `if (luma > t)` makes the
   bloom pop in and out as a surface crosses the limit between frames, and a
   slow camera move across a gradient shows a visible contour where the glow
   starts. Four taps in a box, because one destination pixel covers four source
   pixels and a single bilinear tap would read two of them - a lone bright
   pixel could vanish depending on which half of the box it sat in.
2. **Horizontal blur** (`post_blur.frag`) - `bloom_a` -> `bloom_b`
3. **Vertical blur** - `bloom_b` -> `bloom_a`

Steps 2 and 3 run **twice**. The chain always ends in `bloom_a`, so the
composite always knows which image to read.

Luma is perceptual (`0.2126/0.7152/0.0722`), not `max(r,g,b)`: a saturated blue
neon at `(0,0,1)` has a max of 1.0 and would bloom as hard as white, which
reads as a blown out frame rather than a glowing sign.

---

## Why quarter resolution

A glow needs a blur radius of tens of pixels to read as light rather than as a
soft edge. There are three ways to get one:

- a very large kernel at full resolution - cost grows with the radius
- many passes at full resolution - each one is a full read and write
- **downsample once, then blur** - the kernel stays small and the radius in
  final-image pixels is multiplied by the downsample factor

The third is the standard trade and it costs nothing visible, because the thing
being blurred has no detail left to lose. Blurring something already blurry is
free quality.

Quarter resolution means the three bloom passes touch 1/16 of the pixels a
full-resolution chain would. Measured below: the whole bloom costs about
**10.5 ms** of the 11.0 ms chain at 1280x720 *on a software rasterizer* - at
full resolution those same three passes would be sixteen times the fragment
work.

The blur taps are spaced **2 texels** apart rather than 1. The same nine
weights then cover twice the radius for exactly the same nine samples, and the
bilinear filter fills in between them. Measured effect on the halo of a bright
sphere: 3.21x as many lit pixels as the same scene without bloom, against 2.01x
at 1-texel spacing.

The separable Gaussian is separable for free: a 9-tap kernel in two passes
costs 9+9 = 18 taps per pixel, the same kernel as a 9x9 square costs 81. A
Gaussian is the one kernel that factorises exactly, so this is not an
approximation.

---

## Determinism

The engine's one rule, from the README:

```
state(n+1) = step(state(n), input(n))
```

The grain is seeded from `fx.frame_index`, **never** from the wall clock. A
frame that reads the clock cannot be replayed, and a recording would grain
differently every time it was played back.

The same `frame_index` always produces the same picture. `tests/test_postfx.cpp`
[4a] renders frame 7, then frame 99, then frame 7 again, and asserts that the
first and third are bit identical while the second differs. [4c] then checks
that 74.6% of pixels actually changed between two indices - a hash that was
nearly constant would pass [4a] and [4b] and still not be film grain.

The counter reaches the shader as a float reduced modulo 4096: a float32 stops
being able to represent consecutive integers at 2^24, and a session left
running long enough would otherwise freeze the grain on a single pattern.

---

## Measured cost

1280x720, single sphere, `llvmpipe (LLVM 15.0.6, 256 bits)` - a **software**
rasterizer, on a machine under load from other jobs. Best-of-40 frames, because
the mean on a loaded machine measures the other jobs rather than this one. Two
independent runs, via `dai_render_last_ms`:

| configuration | run 1 | run 2 | run 3 | run 4 | cost over "off" |
|---|---|---|---|---|---|
| chain off | 4.73 ms | 5.41 ms | 4.83 ms | 4.86 ms | - |
| composite only (no bloom) | 10.79 | 11.25 | 10.94 | 11.20 | **+5.8 .. +6.3 ms** |
| bloom only | 15.19 | 15.34 | 16.22 | 15.87 | **+9.9 .. +11.4 ms** |
| full chain | 15.75 | 15.83 | 15.74 | 15.89 | **+10.4 .. +11.0 ms** |

Read these as *ratios, not as frame budgets*. Every number above is fragment
work done on a CPU. The same passes on any real GPU are a small fraction of
this - the bloom is 1/16 of the frame's pixels blurred nine taps at a time, and
the composite is one dependent texture read and about forty ALU operations per
pixel. What the table is good for is the shape:

- the composite alone is roughly half the chain's cost
- `bloom_intensity = 0` skips all three bloom passes, and the test measures
  that it really does - use it when a host wants grain and a vignette only
- the chain off costs **nothing at all**, which is the point of the early
  return in `vk_post_record`

`build/test_postfx` prints its own COST line on whatever machine it runs on.
Nothing in this document is a number that was not produced by a run.

---

## What the tests measure

`tests/test_postfx.cpp` - 26 checks. Every one counts something:

| check | measurement |
|---|---|
| [1a] [1b] | frame digest with the chain off equals the digest before it existed |
| [1c] | the chain on *does* change the frame, so [1a] is not vacuous |
| [2a] | lit pixels 3752 -> 12028, a factor of **3.21** |
| [2b] | the far corner stays under 0.004 - a halo, not a constant add |
| [2c] [2d] | the halo falls off: 0.0661 at 42 px out, 0.0185 at 60 px out |
| [2e] | intensity is a dial: 0.4 gives 0.0224 where 1.2 gives 0.0661 |
| [3a] | the corner keeps 0.203 of its light where the middle keeps 1.000 |
| [3b] | the centre is untouched - the vignette starts outside the middle third |
| [4a] [4b] [4c] | same index = same picture, different index = 74.6% of pixels differ |
| [5a] [5b] | channels separate at the edge, and not on the optical axis |
| [6a] [6b] [6c] | flash brightens and keeps its colour; scanlines alternate 0.607 / 0.305 |
| [7a]..[7e] | a resize rebuilds the chain instead of sampling a freed image |
| [8a]..[8c] | disarming does not retroactively change a frame already drawn |

The pictures are `.gauntlet-shots/postfx-off.png` and `postfx-on.png`, from
`tools/postfx_shot.cpp`. `tools/shot_guard.py` confirms them distinct:

```
shot_guard: ok - 2 pictures, all distinct, none blank, canary caught
```

The off shot has 343 colours, the on shot 7177 - which is the grain and the
bloom gradient, and is by itself a reason the two files can never be confused.

---

## Resize

The post images are the frame's size and the composite's descriptor set points
at `color_rt`'s image view. A resize destroys both.

`dai_render_resize` therefore calls `vk_post_free_targets` next to the other
targets and `vk_post_make_targets` inside the same rebuild that recreates
`color_rt`, which also **rewrites the descriptor sets**. Without that rewrite
the first post-fx frame after a resize samples freed memory - which usually
does not crash, it just renders garbage, and that is the worst kind of bug to
find later. [7c] and [7d] check the sphere is still there and the corner still
black after a resize; [7e] checks that "off is off" still holds at the new size.

---

## What a window shows

The processed frame lives in `post_rt`, not in `color_rt`. Both the readback
and the three window backends therefore ask for *the image the last frame
ended in* rather than naming `color_rt`:

- `dai_render_frame` copies from whatever `vk_post_record` returned
- `rhi_vulkan_window*.cpp` blit from `vk_present_image(r)`

Without that, a window would present the plain frame while
`dai_render_readback` returned the graded one - the same frame looking like two
different pictures depending on how you asked for it, which is the kind of bug
that survives a long time because both halves look correct on their own.

`vk_present_image` reads a flag set by the frame that ran, not the current
value of `fx.enabled`: a host may disarm the chain after a frame was drawn with
it on, and the picture waiting to be presented is still the processed one.
[8a] pins that down.

---

## What is NOT in here

Deliberate omissions, so nobody goes looking:

- **No HDR.** Everything is `R8G8B8A8_UNORM`, so the bright pass works on
  values already clamped to 1.0 and a very bright highlight cannot bloom harder
  than a merely bright one. A real HDR pipeline needs a float format for
  `color_rt` and a tonemap, which is a change to the whole renderer, not to
  this file.
- **No tonemapping / no colour grading.** No ACES curve, no LUT, no lift-gamma-gain.
- **No motion blur and no depth of field.** Both need buffers this chain does
  not have - a velocity buffer and the depth target respectively. The depth
  attachment exists but is `DONT_CARE` at store time, so it is not readable
  after the pass.
- **No temporal anything.** No TAA, no temporal bloom stabilisation, no history
  buffer. The renderer keeps no previous frame, and adding one would mean a
  frame's output depended on its predecessor - which is exactly the property
  the determinism rule exists to prevent.
- **No lens flare, no dirt mask, no anamorphic streaks.** All three need an
  authored texture; the chain is currently self-contained and needs no assets.
- **No per-object glow control.** Bloom is a threshold on the finished frame.
  An object glows by being bright. There is no "emissive-only" bloom mask,
  which would need a second colour attachment.
- **The bloom is not energy-conserving.** `bloom_intensity` is a multiplier, not
  a mix, so a high value adds light to the frame rather than redistributing it.
  That is on purpose - it is the dial that makes neon read as neon - but it
  means bloom can brighten the overall image.
- **No exposure interaction.** `dai_render_exposure` is applied in the mesh
  shader, before any of this. Changing exposure changes what crosses the bloom
  threshold, and the chain does not compensate.
