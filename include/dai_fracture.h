/* dai_fracture.h - breaking a mesh into pieces, deterministically.
 *
 * Voronoi cells clipped out of the source geometry, with the cut faces capped
 * so each piece is a closed solid a physics engine can accept. The seed is an
 * explicit parameter: the same call always produces the same rubble, on any
 * machine, which is what lets a fractured object exist in a rollback world.
 *
 * Intended use is baking - run tools/daifracture once, ship the pieces, and at
 * runtime just swap one body for N. Calling it live is supported but rarely
 * what you want.
 */
#ifndef DAI_FRACTURE_H
#define DAI_FRACTURE_H

#include "dai_gltf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_fracture_opts {
    uint32_t pieces;    /* how many cells to attempt; 2..256           */
    uint64_t seed;      /* same seed, same result - always             */
    float    inset;     /* keep seeds off the surface, 0..0.45 of AABB */
} dai_fracture_opts;

DAI_API dai_fracture_opts dai_fracture_opts_default(void);

/* Breaks `in` (one or more primitives, treated as a single soup) into pieces.
 *
 * Two pass like the rest of the engine: pass out=NULL to learn the count, then
 * again with an array. Returns the number of pieces that had any volume, which
 * can be fewer than opts->pieces when cells land outside the mesh. Free the
 * result with dai_gltf_free_geometry. */
DAI_API uint32_t dai_fracture(const dai_mesh_data *in, uint32_t in_count,
                              const dai_fracture_opts *opts,
                              dai_mesh_data *out, uint32_t max_out,
                              char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* DAI_FRACTURE_H */
