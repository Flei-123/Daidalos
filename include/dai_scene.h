/*
 * Daidalos scene layer - the general purpose glue between a simulated body
 * and what you see.
 *
 * The engine core only knows transforms; it has no idea what anything looks
 * like. Without this layer every demo ends up with a switch on user_data to
 * guess sizes and colours, which is exactly how "why does that box look like
 * a plank" bugs happen.
 *
 * A scene owns:
 *   - an entity per body: physics description + mesh, colour, material
 *   - compound bodies expanded into one render instance per part
 *   - name lookup, so tools and tests can find things
 *   - a camera helper (orbit / follow), still just data, no rendering
 *
 * It stays render backend agnostic: it produces dai_render_instance arrays,
 * it never calls a dai_render_* function. Linking it does not pull in Vulkan.
 */
#ifndef DAI_SCENE_H
#define DAI_SCENE_H

#include "daidalos.h"
#include "dai_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_scene dai_scene;
typedef uint32_t dai_entity;
#define DAI_INVALID_ENTITY ((dai_entity)0)

typedef struct dai_entity_desc {
    dai_body_desc body;         /* physics side, as usual                       */
    uint32_t      mesh;         /* 0xFFFFFFFF -> pick from body.shape           */
    dai_vec3      color;        /* 0,0,0 -> a stable colour derived from the id */
    dai_vec3      render_scale; /* 0,0,0 -> derive from the collision shape     */
    float         roughness;    /* 0 -> 1 (matte)                               */
    float         emissive;
    uint32_t      material;     /* dai_material, 0 = default                    */
    uint32_t      render_flags; /* dai_render_flags                             */
    int           invisible;
    const char   *name;         /* optional, copied                             */
} dai_entity_desc;

DAI_API dai_entity_desc dai_entity_desc_default(void);

DAI_API dai_scene *dai_scene_create(dai_world *w);
DAI_API void       dai_scene_destroy(dai_scene *s);
DAI_API dai_world *dai_scene_world(dai_scene *s);

/* Creates the body and remembers how to draw it. */
DAI_API dai_entity dai_scene_spawn(dai_scene *s, const dai_entity_desc *desc);
/* Attaches render data to a body that already exists. */
DAI_API dai_entity dai_scene_attach(dai_scene *s, dai_body b, const dai_entity_desc *desc);
DAI_API dai_result dai_scene_remove(dai_scene *s, dai_entity e);

DAI_API dai_body   dai_scene_body(dai_scene *s, dai_entity e);
DAI_API dai_entity dai_scene_find(dai_scene *s, const char *name);
DAI_API uint32_t   dai_scene_count(dai_scene *s);
DAI_API dai_result dai_scene_set_color(dai_scene *s, dai_entity e, dai_vec3 color);
/* What the entity is ACTUALLY drawn in. Not the same as what was asked for: a
 * spawn with colour 0,0,0 means "pick one", and the scene then took one from
 * the palette. An editor that shows the requested colour shows three zeros and
 * paints the object black the moment anyone touches the field. */
DAI_API dai_result dai_scene_color(const dai_scene *s, dai_entity e, dai_vec3 *out);
DAI_API dai_result dai_scene_set_visible(dai_scene *s, dai_entity e, int visible);
/* Graphics side only - shape and size stay with the body. mesh 0xFFFFFFFF
 * keeps the current mesh. */
DAI_API dai_result dai_scene_set_render(dai_scene *s, dai_entity e, uint32_t mesh,
                                        float roughness, float emissive, uint32_t flags);
DAI_API dai_result dai_scene_set_material(dai_scene *s, dai_entity e, uint32_t material);
DAI_API dai_result dai_scene_set_name(dai_scene *s, dai_entity e, const char *name);

/* Fills `out` with everything visible, interpolated by alpha (0..1) between
 * the last two ticks. Returns the number written. Purely presentation: safe
 * to call at any frame rate, never touches simulation state. */
/* Draw this entity as several pieces instead of one shape - what an imported
 * model with more than one object needs. The pieces are positioned relative to
 * the entity, so the body stays one body: a crate with a lid is one rigid body
 * and two meshes, not two entities that have to be kept in step.
 *
 * Passing count = 0 goes back to the single mesh. Parts win over both the
 * single mesh and the compound shape expansion - if an asset resolved, that is
 * what the user wants to see. */
DAI_API dai_result dai_scene_set_parts(dai_scene *s, dai_entity e,
                                       const dai_render_part *parts, uint32_t count);
DAI_API uint32_t   dai_scene_part_count(const dai_scene *s, dai_entity e);

DAI_API uint32_t dai_scene_instances(dai_scene *s, dai_render_instance *out, uint32_t max, float alpha);

/* ---- camera helper ----------------------------------------------------- */

typedef struct dai_camera {
    dai_vec3 target;
    float    distance;
    float    yaw;        /* radians, 0 = looking along -Z */
    float    pitch;      /* radians, positive = looking down */
    float    fov;        /* degrees */
    float    znear, zfar;
} dai_camera;

DAI_API dai_camera dai_camera_default(void);
DAI_API dai_vec3   dai_camera_eye(const dai_camera *c);
/* Moves the target towards `p` with an exponential follow, framerate aware. */
DAI_API void       dai_camera_follow(dai_camera *c, dai_vec3 p, float smoothing, float dt);
/* Frames a bounding sphere: sets the distance so it fits the vertical fov. */
DAI_API void       dai_camera_frame(dai_camera *c, dai_vec3 center, float radius);

#ifdef __cplusplus
}
#endif

#endif /* DAI_SCENE_H */
