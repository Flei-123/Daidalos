/*
 * Daidalos rendering interface.
 *
 * Backend agnostic and general purpose: meshes, materials, a sun, a sky and
 * shadows. The simulation core never calls into it - the host does, once per
 * frame, with whatever it wants on screen. Swapping Vulkan for D3D12, Metal
 * or a Unity bridge means replacing one .cpp, not touching the engine.
 *
 * The reference backend is Vulkan 1.3 with dynamic rendering (no VkRenderPass,
 * no framebuffer objects). It renders offscreen and can read the frame back,
 * which is how it is regression tested without a display: render a canonical
 * scene, read the pixels, check them against arithmetic. See
 * tests/test_render_visual.cpp.
 *
 * Conventions, pinned by those tests:
 *   right handed world, +Y up
 *   +X is right on screen, +Y is up on screen
 *   front faces are the OUTSIDE of a closed mesh
 *   scale is a half extent for boxes, a radius for spheres
 */
#ifndef DAI_RENDER_H
#define DAI_RENDER_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_renderer dai_renderer;

/* ---- creation ---------------------------------------------------------- */

typedef struct dai_render_desc {
    uint32_t width;         /* 0 -> 1280 */
    uint32_t height;        /* 0 -> 720  */
    int      msaa;          /* 0/1 -> off, 2, 4, 8. Falls back to the highest supported. */
    int      shadow_size;   /* 0 -> 2048, <0 -> no shadow map */
    int      prefer_device; /* 0 -> first discrete GPU, else the first device found */
    int      validation;    /* 1 -> enable VK_LAYER_KHRONOS_validation if present */
} dai_render_desc;

DAI_API dai_renderer *dai_render_create(const dai_render_desc *desc, char *err, size_t err_len);
DAI_API void          dai_render_destroy(dai_renderer *r);
DAI_API const char   *dai_render_device_name(dai_renderer *r);

/* ---- meshes ------------------------------------------------------------ */

/* A mesh handle. The built in ones exist in every renderer and are always
 * valid; anything above DAI_MESH_BUILTIN_COUNT comes from mesh_create/load. */
typedef uint32_t dai_mesh;

enum {
    DAI_MESH_BOX = 0,      /* unit cube,      scale = half extent            */
    DAI_MESH_SPHERE,       /* unit sphere,    scale = radius                 */
    DAI_MESH_CAPSULE,      /* radius 1,       scale = radius, param = half height of the shaft */
    DAI_MESH_CYLINDER,     /* radius 1, y +-1,scale = (radius, half height, radius) */
    DAI_MESH_CONE,         /* radius 1, y +-1                                */
    DAI_MESH_PLANE,        /* 1x1 quad in XZ, faces +Y, scale = half extent  */
    DAI_MESH_BUILTIN_COUNT
};

typedef struct dai_vertex {
    dai_vec3 position;
    dai_vec3 normal;
    float    cap;          /* capsule shaft offset along Y, 0 for normal meshes */
    float    u, v;         /* texture coordinates, currently used for the checker floor */
} dai_vertex;

/* Uploads a mesh. Indices may be NULL for a non indexed triangle list.
 * Returns 0 (DAI_MESH_BOX) on failure - check dai_render_last_error. */
DAI_API dai_mesh dai_render_mesh_create(dai_renderer *r, const dai_vertex *verts, uint32_t vcount,
                                        const uint32_t *indices, uint32_t icount);

/* Loads a Wavefront OBJ (v/vn/f, triangles and quads, normals generated when
 * the file has none). Enough to get real art into the engine. */
DAI_API dai_mesh dai_render_mesh_load_obj(dai_renderer *r, const char *path);

DAI_API uint32_t dai_render_mesh_count(dai_renderer *r);
DAI_API uint32_t dai_render_mesh_tris(dai_renderer *r, dai_mesh m);

/* ---- what to draw ------------------------------------------------------ */

typedef struct dai_render_instance {
    dai_vec3 position;
    dai_quat rotation;
    dai_vec3 scale;        /* per axis; box: half extent, sphere: radius     */
    dai_vec3 color;        /* linear albedo, 0..1                            */
    uint32_t mesh;         /* dai_mesh                                       */
    float    param;        /* capsule: half height of the shaft              */
    float    roughness;    /* 0 = mirror-ish highlight, 1 = matte (default 1) */
    float    emissive;     /* 0..1, lifts the object out of the lighting      */
    uint32_t flags;        /* dai_render_flags                                */
} dai_render_instance;

typedef enum dai_render_flags {
    DAI_RI_NO_SHADOW   = 1 << 0,   /* does not cast a shadow                 */
    DAI_RI_CHECKER     = 1 << 1,   /* checkerboard pattern in world XZ       */
    DAI_RI_WIREFRAME   = 1 << 2    /* reserved                               */
} dai_render_flags;

/* Convenience: fills in the sane defaults (scale 1, identity rotation,
 * roughness 1). Use it instead of zero initialising, a zero scale draws
 * nothing and a zero quaternion is not a rotation. */
DAI_API dai_render_instance dai_render_instance_default(void);

/* ---- camera, sun, sky -------------------------------------------------- */

DAI_API void dai_render_camera(dai_renderer *r, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                               float fov_deg, float znear, float zfar);
/* Direction the light travels FROM (i.e. towards the scene from that side). */
DAI_API void dai_render_light(dai_renderer *r, dai_vec3 dir);
DAI_API void dai_render_sun(dai_renderer *r, dai_vec3 dir, dai_vec3 color, float intensity);
DAI_API void dai_render_ambient(dai_renderer *r, dai_vec3 sky_color, dai_vec3 ground_color, float intensity);
DAI_API void dai_render_clear_color(dai_renderer *r, float rr, float gg, float bb);
/* 1 -> procedural sky gradient (default), 0 -> flat clear colour. */
DAI_API void dai_render_sky(dai_renderer *r, int enabled);
/* density 0 disables distance fog. */
DAI_API void dai_render_fog(dai_renderer *r, float density, dai_vec3 color);
/* Radius of the world the shadow map covers, centred on the camera target. */
DAI_API void dai_render_shadow_extent(dai_renderer *r, float radius);
DAI_API void dai_render_exposure(dai_renderer *r, float exposure);

/* ---- frame ------------------------------------------------------------- */

DAI_API dai_result dai_render_frame(dai_renderer *r, const dai_render_instance *inst, uint32_t count);

DAI_API dai_result dai_render_readback(dai_renderer *r, uint8_t *rgba, size_t size);
DAI_API dai_result dai_render_write_ppm(dai_renderer *r, const char *path);
DAI_API dai_result dai_render_write_png(dai_renderer *r, const char *path); /* zlib-free, stored deflate */

DAI_API uint32_t    dai_render_width(dai_renderer *r);
DAI_API uint32_t    dai_render_height(dai_renderer *r);
DAI_API double      dai_render_last_ms(dai_renderer *r);
DAI_API uint32_t    dai_render_last_draws(dai_renderer *r);
DAI_API const char *dai_render_last_error(dai_renderer *r);

#ifdef __cplusplus
}
#endif

#endif /* DAI_RENDER_H */
