/*
 * Asset thumbnails: a picture of a mesh, rasterised on the CPU.
 *
 * The Project window used to show a coloured icon for a .glb, which tells you
 * the file is a model and nothing else. Twelve models in a folder are twelve
 * identical amber squares, and the only way to find the one you want is to
 * place it and undo.
 *
 * Why the CPU. The obvious answer is "render it with the renderer into an
 * offscreen target", and that is the wrong shape for this problem: it needs a
 * second render pass, a framebuffer per size, a readback or a descriptor per
 * asset, and it makes the Project window depend on a GPU being present - the
 * editor's own tests run headless. A thumbnail is 64x64 pixels of a mesh that
 * never moves. That is a few thousand triangles through a z-buffer, once, and
 * then it is a texture like any other.
 *
 * So: same reasoning as the SVG rasteriser and the TrueType loader next to it.
 * A few hundred lines instead of a dependency, and it works everywhere.
 *
 *     uint8_t rgba[64 * 64 * 4];
 *     dai_thumb_mesh m{ positions, vcount, indices, icount };
 *     if (dai_thumb_render(&m, rgba, 64, 0xC8C8C8))
 *         tex = dai_render_texture_create(r, rgba, 64, 64, 1);
 *
 * The projection is ORTHOGRAPHIC and the mesh is fitted to the frame, so the
 * picture does not depend on how big the model is in metres: a 1 m crate and a
 * 100 m crate give the same thumbnail. That is the property that matters -
 * these are icons, not previews of a scene.
 */
#ifndef DAI_THUMB_H
#define DAI_THUMB_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_thumb_mesh {
    const float    *positions;    /* 3 floats per vertex, x y z            */
    uint32_t        vertex_count;
    /* 3 per triangle. NULL means the positions ARE the triangle list, in
     * order - which is what most of this engine's geometry already is. */
    const uint32_t *indices;
    uint32_t        index_count;
} dai_thumb_mesh;

/* Writes `size` x `size` RGBA8 (straight alpha, row major, top row first) into
 * `rgba`, which must hold size*size*4 bytes. The background is left fully
 * transparent, so the thumbnail sits on whatever the row behind it is.
 *
 * `tint_rgb` is 0xRRGGBB, the colour of the surface; pass 0 for a neutral
 * grey. Lighting is one key light over the viewer's shoulder plus ambient,
 * two sided - a model whose triangles wind the wrong way still gets shaded
 * rather than turning black.
 *
 * Returns 1 when something was drawn, 0 when the mesh is empty, degenerate
 * (every vertex in one place) or the arguments do not make sense. On 0 the
 * buffer is cleared to transparent rather than left as it was found.
 *
 * No allocation beyond one depth buffer, no globals, thread safe. */
DAI_API int dai_thumb_render(const dai_thumb_mesh *mesh, uint8_t *rgba,
                             uint32_t size, uint32_t tint_rgb);

/* The same picture for something the engine can draw but has no mesh file
 * for - a box, a sphere, a capsule, a cylinder - so a prefab made of shapes
 * gets a thumbnail too. `shape` is a dai_shape value. */
DAI_API int dai_thumb_render_shape(int shape, uint8_t *rgba, uint32_t size,
                                   uint32_t tint_rgb);

#ifdef __cplusplus
}
#endif

#endif /* DAI_THUMB_H */
