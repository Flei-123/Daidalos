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

/* One drawable piece: a mesh, its material, and where it sits.
 *
 * Two transforms on purpose. `position/rotation/scale` are in the MODEL's
 * space - flattened, ready to draw, and what most callers want. `parent` plus
 * `local_*` keep the structure the artist built: in Blender a crate with a lid
 * parented to it arrives as two pieces, the lid pointing at the crate, and its
 * local transform is the one you animate when the lid opens.
 *
 * `parent` indexes this same node array, or -1 for a root. A group node with
 * no mesh of its own produces no piece, so the parent link skips it and points
 * at the nearest ancestor that does draw something. */
typedef struct dai_model_node {
    dai_mesh     mesh;
    dai_material material;
    dai_vec3     position;        /* model space */
    dai_quat     rotation;
    dai_vec3     scale;
    int32_t      parent;          /* index into the node array, -1 = root */
    dai_vec3     local_position;  /* relative to `parent`, or model space */
    dai_quat     local_rotation;
    dai_vec3     local_scale;
    /* The mesh's own bounding box, in MESH space - before this node's scale,
     * so a collision shape is (bounds * scale). Not the model's bounds: those
     * cover everything and would give every piece the same box. */
    dai_vec3     bounds_min;
    dai_vec3     bounds_max;
    char         name[64];
} dai_model_node;

typedef struct dai_animation_info {
    char  name[64];
    float duration;      /* seconds */
    uint32_t channels;
} dai_animation_info;

typedef struct dai_model_info {
    uint32_t nodes;
    uint32_t meshes;
    uint32_t materials;
    uint32_t textures;
    uint32_t triangles;
    uint32_t vertices;
    uint32_t animations;
    uint32_t skins;
    uint32_t joints;      /* total joint matrices the model needs */
    dai_vec3 bounds_min;
    dai_vec3 bounds_max;
} dai_model_info;

/* Loads .glb or .gltf. Meshes, textures and materials are created inside the
 * renderer, so the returned model only holds handles. Returns NULL on error
 * and fills `err`. */
DAI_API dai_model *dai_gltf_load(dai_renderer *r, const char *path, char *err, size_t err_len);

/* Reads a file the glTF references by URI - a .bin buffer or an external PNG.
 * Return 1 and hand out bytes that stay valid until the call that asked for
 * them returns, or 0 if it does not exist. */
typedef int (*dai_gltf_read_fn)(const char *uri, const void **out_bytes,
                                size_t *out_size, void *user);

/* The same import, from bytes the caller already has. This is what an asset
 * cache uses: it has read the file through its own mounts (a folder, a pack),
 * and there is no path left to hand over. `sidecar` resolves external buffers
 * and images; pass NULL for a self contained .glb. */
DAI_API dai_model *dai_gltf_load_memory(dai_renderer *r, const void *bytes, size_t size,
                                        dai_gltf_read_fn sidecar, void *user,
                                        char *err, size_t err_len);

/* ---- geometry without a renderer ---------------------------------------- */

/* One primitive's triangles, on the CPU. What a TOOL needs: a fracture baker
 * has to cut the mesh, and cutting something that only exists as a handle
 * inside the renderer is not possible. */
typedef struct dai_mesh_data {
    dai_vertex *vertices;
    uint32_t    vertex_count;
    uint32_t   *indices;
    uint32_t    index_count;
    char        name[64];        /* the mesh's name, not the node's */
} dai_mesh_data;

/* Reads every triangle primitive out of a .glb or .gltf and hands back plain
 * arrays. No renderer, no textures, no materials, no node hierarchy - the
 * transform each primitive ends up under is the caller's problem, which is
 * exactly right for a tool that works in the mesh's own space.
 *
 * Returns how many primitives the file has; fills up to `max`. Free what was
 * filled with dai_gltf_free_geometry. External .bin buffers are NOT resolved:
 * pass a self contained .glb. */
DAI_API uint32_t dai_gltf_read_geometry(const void *bytes, size_t size,
                                        dai_mesh_data *out, uint32_t max,
                                        char *err, size_t err_len);
DAI_API void     dai_gltf_free_geometry(dai_mesh_data *m, uint32_t count);

/* ---- writing ------------------------------------------------------------ */

typedef struct dai_mesh_write {
    const dai_vertex *vertices;
    uint32_t          vertex_count;
    const uint32_t   *indices;
    uint32_t          index_count;
    const char       *name;       /* becomes both the mesh and the node name */
} dai_mesh_write;

/* Writes a self contained .glb: positions, normals and indices, one node per
 * mesh at the identity transform. No materials, no textures, no hierarchy - a
 * tool that produces geometry should not be inventing a scene.
 *
 * This exists because a baker has to hand its output back in the format the
 * engine already reads. Written through a temp file and renamed, so an
 * interrupted bake cannot destroy the previous result. */
DAI_API dai_result dai_gltf_write(const char *path, const dai_mesh_write *meshes,
                                  uint32_t count, char *err, size_t err_len);
DAI_API void       dai_model_free(dai_model *m);
/* Frees the model AND everything the import created inside the renderer: its
 * meshes, its textures, its materials. Nothing else owns those handles, so a
 * host that reloads a model wants this rather than dai_model_free - otherwise
 * every reload leaks a full copy of the geometry.
 *
 * Every handle the model handed out is dead afterwards. Anything still drawing
 * with one must be updated first; the asset layer rebuilds those nodes. */
DAI_API void       dai_model_release(dai_renderer *r, dai_model *m);

DAI_API dai_model_info        dai_model_get_info(const dai_model *m);
DAI_API uint32_t              dai_model_node_count(const dai_model *m);
DAI_API const dai_model_node *dai_model_node_at(const dai_model *m, uint32_t index);
DAI_API const dai_model_node *dai_model_find(const dai_model *m, const char *name);

/* ---- animation ---------------------------------------------------------- */

DAI_API uint32_t           dai_model_animation_count(const dai_model *m);
DAI_API dai_animation_info dai_model_animation_at(const dai_model *m, uint32_t index);

/* Poses the model: samples the animation at `time` (looping), recomputes the
 * node hierarchy and writes the joint matrices for every skin into `joints`
 * as column major 4x4 floats. Returns how many matrices were written.
 *
 * Pass animation = -1 for the bind pose. Feed the result to
 * dai_render_joints(); dai_model_instances then references it automatically. */
DAI_API uint32_t dai_model_pose(dai_model *m, int animation, float time,
                                float *joints, uint32_t max_joints);

/* Cross fades two clips. weight 0 = a, 1 = b; translations and scales are
 * linear, rotations slerp. This is what makes a walk turn into a run without
 * the character snapping - and it is the smallest useful blend: everything
 * else (state machines, layers, additive) is built out of repeated calls. */
DAI_API uint32_t dai_model_pose_blend(dai_model *m, int anim_a, float time_a,
                                      int anim_b, float time_b, float weight,
                                      float *joints, uint32_t max_joints);

/* Fills render instances for the whole model, transformed by an offset,
 * a uniform scale and a rotation. Returns how many were written. */
DAI_API uint32_t dai_model_instances(const dai_model *m, dai_render_instance *out, uint32_t max,
                                     dai_vec3 offset, dai_quat rotation, float scale);

#ifdef __cplusplus
}
#endif

#endif /* DAI_GLTF_H */
