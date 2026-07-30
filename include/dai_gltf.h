/*
 * glTF 2.0 / GLB import.
 *
 * The whole point: Blender -> engine in one step, no shader graph, no baking,
 * no exporter plugin. Export a .glb from Blender, load it here, draw it. What
 * comes across is exactly what the format guarantees:
 *
 *   meshes      indexed triangles, position / normal / uv
 *   materials   metallic-roughness: base colour, ORM, normal, emissive
 *   textures    PNG, embedded in the GLB or next to the .gltf
 *   nodes       the transform hierarchy, flattened to world space TRS
 *
 * Anything a Principled BSDF can express through glTF arrives; anything it
 * cannot (procedural node graphs) has to be baked to a map in Blender, which
 * is a Blender problem and not an engine problem. See docs/MATERIALS.md.
 *
 * The importer is written from scratch: its own JSON parser, its own base64,
 * its own PNG decode. No tinygltf, no cgltf, no stb.
 */
#ifndef DAI_GLTF_H
#define DAI_GLTF_H

#include "dai_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_model dai_model;

/* One drawable piece: a mesh, its material and its world transform. */
typedef struct dai_model_node {
    dai_mesh     mesh;
    dai_material material;
    dai_vec3     position;
    dai_quat     rotation;
    dai_vec3     scale;
    char         name[64];
} dai_model_node;

typedef struct dai_model_info {
    uint32_t nodes;
    uint32_t meshes;
    uint32_t materials;
    uint32_t textures;
    uint32_t triangles;
    uint32_t vertices;
    dai_vec3 bounds_min;
    dai_vec3 bounds_max;
} dai_model_info;

/* Loads .glb or .gltf. Meshes, textures and materials are created inside the
 * renderer, so the returned model only holds handles. Returns NULL on error
 * and fills `err`. */
DAI_API dai_model *dai_gltf_load(dai_renderer *r, const char *path, char *err, size_t err_len);
DAI_API void       dai_model_free(dai_model *m);

DAI_API dai_model_info        dai_model_get_info(const dai_model *m);
DAI_API uint32_t              dai_model_node_count(const dai_model *m);
DAI_API const dai_model_node *dai_model_node_at(const dai_model *m, uint32_t index);
DAI_API const dai_model_node *dai_model_find(const dai_model *m, const char *name);

/* Fills render instances for the whole model, transformed by an offset,
 * a uniform scale and a rotation. Returns how many were written. */
DAI_API uint32_t dai_model_instances(const dai_model *m, dai_render_instance *out, uint32_t max,
                                     dai_vec3 offset, dai_quat rotation, float scale);

#ifdef __cplusplus
}
#endif

#endif /* DAI_GLTF_H */
