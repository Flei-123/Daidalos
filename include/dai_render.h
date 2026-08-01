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
    float    u, v;         /* texture coordinates                             */
    uint8_t  joints[4];    /* skinning: which joints influence this vertex    */
    float    weights[4];   /* their weights; all zero = not skinned           */
} dai_vertex;

/* Uploads a mesh. Indices may be NULL for a non indexed triangle list.
 * Returns 0 (DAI_MESH_BOX) on failure - check dai_render_last_error. */
DAI_API dai_mesh dai_render_mesh_create(dai_renderer *r, const dai_vertex *verts, uint32_t vcount,
                                        const uint32_t *indices, uint32_t icount);

/* Loads a Wavefront OBJ (v/vn/f, triangles and quads, normals generated when
 * the file has none). Enough to get real art into the engine. */
DAI_API dai_mesh dai_render_mesh_load_obj(dai_renderer *r, const char *path);

DAI_API uint32_t dai_render_mesh_count(dai_renderer *r);
/* Gives the mesh's slot and its slice of the geometry buffer back. The handle
 * is dead afterwards - drawing it draws nothing - and the slice is handed to
 * the next mesh that fits in it, which is what stops an editor that reloads
 * the same model all afternoon from growing without bound. Builtin meshes are
 * shared and cannot be destroyed. */
DAI_API void     dai_render_mesh_destroy(dai_renderer *r, dai_mesh m);
/* Slots ever handed out vs slots currently alive - they differ once anything
 * has been freed. */
DAI_API uint32_t dai_render_mesh_live(dai_renderer *r);
DAI_API uint32_t dai_render_mesh_tris(dai_renderer *r, dai_mesh m);

/* ---- textures and materials -------------------------------------------- */

/* A texture handle. 0 is a 1x1 white texture that always exists, so a material
 * with no maps is still valid. */
typedef uint32_t dai_texture;

/* A material handle. 0 is the default: white, matte, no maps. */
typedef uint32_t dai_material;

/* srgb = 1 for anything the artist picked by eye (base colour, emissive),
 * 0 for data (normal, roughness, metallic, occlusion, height). Getting this
 * backwards is the single most common texture bug there is. */
DAI_API dai_texture dai_render_texture_create(dai_renderer *r, const uint8_t *rgba,
                                              uint32_t w, uint32_t h, int srgb);
DAI_API dai_texture dai_render_texture_load(dai_renderer *r, const char *path, int srgb);
DAI_API uint32_t    dai_render_texture_count(dai_renderer *r);
/* Destroys the image and hands the slot back. Any material still sampling it
 * is pointed at the default texture first, so no descriptor is left dangling.
 * Texture 0 is the default and is never destroyed. */
DAI_API void        dai_render_texture_destroy(dai_renderer *r, dai_texture t);

/* One material model, glTF 2.0 metallic-roughness. Four maps, no node graph:
 *
 *   base_color_tex   sRGB   albedo, alpha in A
 *   orm_tex          linear R = ambient occlusion, G = roughness, B = metallic
 *   normal_tex       linear tangent space normal map
 *   emissive_tex     sRGB   glow
 *
 * That is exactly what Blender's Principled BSDF exports through glTF, so the
 * round trip needs no shader graph and no baking - see docs/MATERIALS.md.
 * Scalars multiply their map, so a material can be pure numbers, pure
 * textures, or both. */
typedef struct dai_material_desc {
    dai_vec3    base_color;      /* multiplies base_color_tex, default 1,1,1   */
    float       metallic;        /* multiplies orm.b, default 0                */
    float       roughness;       /* multiplies orm.g, default 1                */
    dai_vec3    emissive;        /* multiplies emissive_tex, default 0         */
    float       normal_strength; /* 0 -> 1                                     */
    float       occlusion;       /* how much of orm.r is applied, 0 -> 1       */
    float       alpha_cutoff;    /* >0 enables alpha testing                   */
    dai_vec2    uv_scale;        /* tiling per axis, 0 -> 1                    */
    dai_vec2    uv_offset;       /* scrolling; the sampler repeats, so any      *
                                  * value works and 1.0 is one full wrap        */
    dai_texture base_color_tex;
    dai_texture orm_tex;
    dai_texture normal_tex;
    dai_texture emissive_tex;
    uint32_t    flags;           /* dai_material_flags                          */
    const char *name;
} dai_material_desc;

typedef enum dai_material_flags {
    DAI_MAT_TRIPLANAR = 1 << 0,  /* reserved */
    DAI_MAT_CHECKER   = 1 << 1   /* procedural checker, needs no texture at all */
} dai_material_flags;

DAI_API dai_material_desc dai_material_desc_default(void);
DAI_API dai_material      dai_render_material_create(dai_renderer *r, const dai_material_desc *desc);
DAI_API uint32_t          dai_render_material_count(dai_renderer *r);
/* Rewrites an existing material in place - same handle, same descriptor set
 * unless the textures changed. This is what animates a material: bump
 * uv_offset every frame for water or lava instead of leaking a new material
 * per frame. Returns DAI_OK, or DAI_ERR_NOT_FOUND for an unknown handle. */
DAI_API dai_result        dai_render_material_update(dai_renderer *r, dai_material mat,
                                                     const dai_material_desc *desc);
/* Hands the slot back for reuse. The descriptor set is kept and rewritten by
 * the next material that lands in this slot. Material 0 is the default. */
DAI_API void              dai_render_material_destroy(dai_renderer *r, dai_material mat);

/* ---- what to draw ------------------------------------------------------ */

typedef struct dai_render_instance {
    dai_vec3 position;
    dai_quat rotation;
    dai_vec3 scale;        /* per axis; box: half extent, sphere: radius     */
    dai_vec3 color;        /* linear albedo, 0..1                            */
    uint32_t mesh;         /* dai_mesh                                       */
    float    param;        /* capsule: half height of the shaft              */
    float    roughness;    /* multiplies the material roughness (default 1)  */
    float    emissive;     /* 0..1, lifts the object out of the lighting      */
    uint32_t flags;        /* dai_render_flags                                */
    uint32_t material;     /* dai_material, 0 = default                       */
    uint32_t joint_offset; /* first joint matrix for this instance            */
    uint32_t joint_count;  /* 0 = not skinned                                 */
    /* Per instance UV. Tiling: 0,0 falls back to the material's uv_scale, so
     * a zero initialised instance keeps whatever the material says. Offset is
     * ADDED to the material's, which is what lets a hundred conveyor belts
     * share one material and still scroll out of phase. */
    dai_vec2 uv_scale;
    dai_vec2 uv_offset;
} dai_render_instance;

/* Uploads joint matrices for the NEXT frame. Instances index into this array
 * through joint_offset; one buffer holds every skinned character in the frame,
 * so skinning costs one upload rather than one per character. */
DAI_API void dai_render_joints(dai_renderer *r, const float *matrices4x4, uint32_t count);
DAI_API uint32_t dai_render_max_joints(dai_renderer *r);

typedef enum dai_render_flags {
    DAI_RI_NO_SHADOW   = 1 << 0,   /* does not cast a shadow                 */
    DAI_RI_CHECKER     = 1 << 1,   /* checkerboard pattern in world XZ       */
    DAI_RI_WIREFRAME   = 1 << 2    /* reserved                               */
} dai_render_flags;

/* Convenience: fills in the sane defaults (scale 1, identity rotation,
 * roughness 1). Use it instead of zero initialising, a zero scale draws
 * nothing and a zero quaternion is not a rotation. */
DAI_API dai_render_instance dai_render_instance_default(void);

/* One drawable piece of something that is not a single shape - an imported
 * model with five objects in it, say. Mesh and material are its own; the
 * transform is relative to whatever the piece belongs to. */
typedef struct dai_render_part {
    uint32_t mesh;
    uint32_t material;
    dai_vec3 position;
    dai_quat rotation;
    dai_vec3 scale;
} dai_render_part;

/* ---- particles --------------------------------------------------------- */

/* What the renderer needs per particle. Filled by dai_particles_fill (see
 * dai_particles.h) or by hand - the renderer does not care where they come
 * from. Drawn as camera facing billboards after the opaque pass, depth tested
 * against the scene but not writing depth. */
typedef struct dai_particle {
    dai_vec3 position;
    float    size;        /* world units, diameter                          */
    dai_vec3 color;       /* linear                                          */
    float    alpha;       /* 0..1; with DAI_BLEND_ADD it is the intensity    */
    float    rotation;    /* radians                                         */
    uint32_t blend;       /* dai_particle_blend: 0 = alpha, 1 = additive     */
    uint32_t frame;       /* atlas cell, row major; ignored without an atlas  */
} dai_particle;

/* Points the particle pass at a texture atlas: `cols` x `rows` cells, indexed
 * row major by dai_particle.frame. Pass texture 0 to go back to the built in
 * soft dot. One atlas per frame - particles are the one place where a texture
 * switch per effect would cost more than it buys. */
DAI_API void dai_render_particle_atlas(dai_renderer *r, dai_texture tex, uint32_t cols, uint32_t rows);

/* Hands the renderer the particles for the NEXT dai_render_frame call. The
 * pointer is copied, not retained. Pass count 0 to clear. */
DAI_API void dai_render_particles(dai_renderer *r, const dai_particle *particles, uint32_t count);

/* ---- lights ------------------------------------------------------------ */

typedef enum dai_light_type {
    DAI_LIGHT_POINT = 0,
    DAI_LIGHT_SPOT  = 1
} dai_light_type;

/* Punctual lights, on top of the directional sun. No shadows from these yet -
 * a point light shadow is six more depth passes, and a spot is one; both are
 * worth having, neither is worth pretending to have. */
typedef struct dai_light {
    dai_vec3 position;
    float    range;        /* metres; intensity reaches zero here            */
    dai_vec3 color;
    float    intensity;    /* multiplies colour                              */
    dai_vec3 direction;    /* spot only, points away from the light          */
    float    inner_deg;    /* spot cone, full brightness inside this angle   */
    float    outer_deg;    /* spot cone, zero outside this angle             */
    uint32_t type;         /* dai_light_type                                 */
} dai_light;

DAI_API dai_light dai_light_point(dai_vec3 position, dai_vec3 color, float intensity, float range);
DAI_API dai_light dai_light_spot(dai_vec3 position, dai_vec3 direction, dai_vec3 color,
                                 float intensity, float range, float inner_deg, float outer_deg);

/* Uploads the lights for the NEXT frame. Count 0 clears them. */
DAI_API void     dai_render_lights(dai_renderer *r, const dai_light *lights, uint32_t count);
DAI_API uint32_t dai_render_max_lights(dai_renderer *r);

/* ---- culling ----------------------------------------------------------- */

/* Frustum culling is on by default. Instances whose bounding sphere is outside
 * the camera frustum never reach the GPU. dai_render_last_culled() reports how
 * many were dropped, which is the number that tells you whether your scene
 * layout is doing anything sensible. */
DAI_API void     dai_render_culling(dai_renderer *r, int enabled);
DAI_API uint32_t dai_render_last_culled(dai_renderer *r);
DAI_API uint32_t dai_render_last_visible(dai_renderer *r);

/* ---- UI overlay -------------------------------------------------------- */

/* Hands the renderer the UI batches for the NEXT frame; drawn last, in screen
 * space, with no depth test. Pass count 0 to clear. The vertex layout is
 * dai_ui_vertex from dai_ui.h, but the renderer does not include that header -
 * it takes raw floats, so a host with its own UI can feed this too. */
DAI_API void dai_render_ui(dai_renderer *r, const void *vertices, uint32_t vertex_count,
                           const uint32_t *batch_counts, const uint32_t *batch_textures,
                           uint32_t batch_count);

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

/* ---- window and presentation ------------------------------------------- */

/* The renderer draws offscreen and stays that way; a window is a consumer of
 * the finished frame, not a different rendering path. That is what keeps the
 * headless tests and the on screen build byte identical - and it means a
 * window backend is ~300 lines instead of a rewrite.
 *
 * X11, Wayland and Win32 today, one file each, same four functions. */
typedef struct dai_window dai_window;

DAI_API dai_window *dai_window_open(dai_renderer *r, const char *title,
                                    uint32_t width, uint32_t height, char *err, size_t err_len);
DAI_API void        dai_window_close(dai_window *w);

/* Pumps events. Returns 0 once the window has been closed. */
DAI_API int  dai_window_poll(dai_window *w);
/* Blits the last rendered frame to the screen. */
DAI_API dai_result dai_window_present(dai_window *w);

/* Key codes. These ARE the X11 keysym values, which for letters and digits are
 * just ASCII - the X11 backend therefore needs no translation at all, and every
 * other backend maps its own codes onto these when a key arrives.
 *
 * The alternative was letting each backend hand out its own numbers, which is
 * what this used to do: the host then had to spell every key twice, once as
 * XK_w and once as 'W', inside an #ifdef. A key code is part of the engine's
 * API, not an artefact of whatever windowing system happens to be underneath.
 *
 * Letters are lower case; the backend normalises so a shifted 'W' still reads
 * as DAI_KEY_W. */
typedef enum dai_key {
    DAI_KEY_SPACE     = 0x0020,
    DAI_KEY_0         = 0x0030,   /* .. DAI_KEY_9 are 0x30 + digit  */
    DAI_KEY_9         = 0x0039,
    DAI_KEY_A         = 0x0061,   /* .. DAI_KEY_Z are 0x61 + letter */
    DAI_KEY_D         = 0x0064,
    DAI_KEY_E         = 0x0065,
    DAI_KEY_F         = 0x0066,
    DAI_KEY_G         = 0x0067,
    DAI_KEY_Q         = 0x0071,
    DAI_KEY_R         = 0x0072,
    DAI_KEY_S         = 0x0073,
    DAI_KEY_W         = 0x0077,
    DAI_KEY_X         = 0x0078,
    DAI_KEY_Y         = 0x0079,
    DAI_KEY_Z         = 0x007A,

    DAI_KEY_ESCAPE    = 0xFF1B,
    DAI_KEY_TAB       = 0xFF09,
    DAI_KEY_RETURN    = 0xFF0D,
    DAI_KEY_BACKSPACE = 0xFF08,
    DAI_KEY_DELETE    = 0xFFFF,
    DAI_KEY_LEFT      = 0xFF51,
    DAI_KEY_UP        = 0xFF52,
    DAI_KEY_RIGHT     = 0xFF53,
    DAI_KEY_DOWN      = 0xFF54,
    DAI_KEY_F1        = 0xFFBE,
    DAI_KEY_F2        = 0xFFBF,
    DAI_KEY_F5        = 0xFFC2,

    /* Modifiers come in pairs because X11 reports them that way. A backend that
     * cannot tell the sides apart - Win32 for a plain shift - sets both. */
    DAI_KEY_SHIFT_L   = 0xFFE1,
    DAI_KEY_SHIFT_R   = 0xFFE2,
    DAI_KEY_CTRL_L    = 0xFFE3,
    DAI_KEY_CTRL_R    = 0xFFE4,
    DAI_KEY_ALT_L     = 0xFFE9,
    DAI_KEY_ALT_R     = 0xFFEA
} dai_key;

/* Minimal input. Pass a dai_key; anything else is undefined but harmless. */
DAI_API int dai_window_key_down(dai_window *w, uint32_t key);

/* Text events, the way the OS actually delivers them: one code point per key
 * PRESS (with the OS's own repeat), shift and dead keys already resolved.
 * dai_window_key_down answers "is it held" - the right question for a camera
 * and the wrong one for a text field, where held means repeating the letter
 * every frame. Fills `out` with up to `max` code points typed since the last
 * poll and returns the count, 0 terminated like dai_ui_input.text. */
DAI_API uint32_t dai_window_text(dai_window *w, uint32_t *out, uint32_t max);
/* Pointer position IN RENDERER PIXELS, not window pixels. The frame is blitted
 * onto the window and stretched to fit it, so those two only agree when the
 * window happens to be exactly the render resolution. Reporting the mouse in
 * the space the host actually drew in is what makes hit testing a gizmo work
 * after someone maximises the window. */
DAI_API int dai_window_mouse(dai_window *w, int *x, int *y, uint32_t *buttons);
/* Wheel notches accumulated since the last call, and resets. Polling a button
 * bit cannot work: a wheel click is a press and a release in the same frame,
 * so a frame that is even slightly late misses it entirely. */
DAI_API float dai_window_wheel(dai_window *w);
DAI_API void dai_window_size(dai_window *w, uint32_t *width, uint32_t *height);

/* ---- frame ------------------------------------------------------------- */

DAI_API dai_result dai_render_frame(dai_renderer *r, const dai_render_instance *inst, uint32_t count);

/* Re-creates the render targets at a new size.
 *
 * The frame is blitted onto the window and stretched to fit it, so a renderer
 * that keeps a fixed resolution while the window changes shows a picture of the
 * wrong shape and the wrong sharpness - a 16:9 frame smeared across a 21:9
 * screen, with 17 pixel text scaled up until it is a smear of white. A host
 * with a resizable window should call this whenever dai_window_size changes.
 *
 * Costs a device idle and three image allocations, so call it on the size
 * actually changing, not every frame. Returns DAI_OK, or DAI_ERR_OUT_OF_MEMORY
 * with the previous size still working. */
DAI_API dai_result dai_render_resize(dai_renderer *r, uint32_t width, uint32_t height);

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
