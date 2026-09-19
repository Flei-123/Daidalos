// Shared internals of the Vulkan backend. Split in two translation units:
//   rhi_vulkan.cpp        device, targets, pipelines, meshes, state
//   rhi_vulkan_frame.cpp  the frame itself (shadow pass, sky, meshes, resolve)
#ifndef DAI_RHI_VULKAN_HPP
#define DAI_RHI_VULKAN_HPP

#include "dai_render.h"
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------- math

struct Mat4 { float m[16]; };   // column major, same as GLSL

Mat4 mat_identity();
Mat4 mat_mul(const Mat4 &a, const Mat4 &b);
Mat4 mat_look_at(const float eye[3], const float ctr[3], const float up[3]);
// Vulkan clip space: y down, depth 0..1. Both folded in here.
Mat4 mat_perspective(float fov_deg, float aspect, float zn, float zf);
Mat4 mat_ortho(float l, float r, float b, float t, float zn, float zf);
bool mat_invert(const Mat4 &in, Mat4 *out);

// ---------------------------------------------------------------- gpu data

// Must match the uniform block in the shaders, std140. Every member is a
// vec4/mat4 so the layout needs no padding gymnastics.
#define DAI_SHADOW_CASCADES 3

struct FrameUBO {
    Mat4  viewproj;
    Mat4  invviewproj;
    Mat4  lightviewproj[DAI_SHADOW_CASCADES];
    float cascade_split[4];      // view depth where each cascade ends
    float sun_dir[4];       // xyz, w = intensity
    float sun_color[4];     // rgb, w = shadow texel size
    float sky_color[4];     // rgb, w = ambient intensity
    float ground_color[4];  // rgb, w = fog density
    float fog_color[4];     // rgb, w = exposure
    float cam_pos[4];       // xyz, w = shadows enabled
    float cam_right[4];     // billboard basis
    float cam_up[4];
};

#define DAI_MAX_MATERIALS 512
#define DAI_MAX_UI_TEXTURES 256

struct TextureEntry {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, mips = 1;
    // The UI draws by TEXTURE, not by material - a font atlas is a texture that
    // no material ever refers to. Without a set of its own it cannot be bound,
    // and the pass falls back to something else entirely.
    VkDescriptorSet ui_set = VK_NULL_HANDLE;
};

// Push constant block, must match the shaders. 96 bytes: still inside the
// 128 byte guarantee, so no uniform buffer traffic per material switch.
struct MaterialPush {
    float base_color[4];   // rgb + alpha cutoff
    float emissive[4];     // rgb + flags as float
    float scalars[4];      // metallic, roughness, normal strength, unused
    float extra[4];        // occlusion, has_maps, has_normal_map, shadow cascade
    float uv[4];           // tiling x, tiling y, offset x, offset y
    float tri[4];          // triplanar: on, 1/metres per repeat, blend, unused
};

struct MaterialEntry {
    MaterialPush p{};
    uint32_t base_tex = 0, orm_tex = 0, normal_tex = 0, emissive_tex = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    char name[48] = {0};
};

#define DAI_MAX_LIGHTS 256

// std430 layout, matching the shader's Lights buffer
struct GpuLight {
    float position[3]; float range;
    float color[3];    float intensity;
    float direction[3]; float cos_inner;
    float cos_outer;   float type; float pad0, pad1;
};

struct MeshEntry {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t  vertex_offset = 0;
    // Geometry lives in one big buffer filled by a bump allocator, so freeing
    // a mesh cannot hand memory back - but it CAN hand the range back, and a
    // reload of the same model asks for the same sizes. Capacity is what the
    // range can hold, index_count what it currently holds.
    uint32_t index_cap = 0;
    uint32_t vertex_cap = 0;
    bool     alive = true;
};

struct GpuBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

struct dai_renderer {
    uint32_t width = 1280, height = 720;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
    uint32_t shadow_size = 2048;
    bool     shadows = true;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // colour targets: ms is the multisampled attachment, resolve is what gets
    // read back. With msaa == 1 only resolve exists and is drawn into directly.
    VkImage color_ms = VK_NULL_HANDLE, color_rt = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkDeviceMemory color_ms_mem = VK_NULL_HANDLE, color_rt_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE;
    VkImageView color_ms_view = VK_NULL_HANDLE, color_rt_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;

    // one array image, one layer per cascade: a layer view to render into and
    // an array view to sample from
    VkImage shadow_img = VK_NULL_HANDLE;
    VkDeviceMemory shadow_mem = VK_NULL_HANDLE;
    VkImageView shadow_view = VK_NULL_HANDLE;                       // 2D array, for sampling
    VkImageView shadow_layer[DAI_SHADOW_CASCADES] = {};             // per cascade, for rendering
    VkSampler shadow_sampler = VK_NULL_HANDLE;
    uint32_t cascades = DAI_SHADOW_CASCADES;
    Mat4 last_lightvp[DAI_SHADOW_CASCADES] = {};   // for skipping unchanged cascades
    uint32_t last_casters = 0;
    uint64_t last_caster_hash = 0;
    bool shadow_valid = false;   // false until the first full shadow render

    GpuBuffer vbo, ibo, inst, ubo, readback;
    uint32_t vtx_used = 0, idx_used = 0, inst_capacity = 0;

    std::vector<MeshEntry> meshes;
    std::vector<uint32_t>  free_meshes;      // slots whose range can be reused
    std::vector<uint32_t>  free_textures;
    std::vector<uint32_t>  free_materials;
    std::vector<TextureEntry> textures;
    std::vector<MaterialEntry> materials;

    VkDescriptorSetLayout mat_dsl = VK_NULL_HANDLE;
    VkDescriptorPool mat_pool = VK_NULL_HANDLE;
    VkSampler tex_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe_mesh = VK_NULL_HANDLE, pipe_shadow = VK_NULL_HANDLE, pipe_sky = VK_NULL_HANDLE;
    VkPipeline pipe_particle = VK_NULL_HANDLE;
    GpuBuffer particles;
    uint32_t particle_capacity = 0, particle_count = 0;

    dai_material particle_material = 0;   // holds the atlas texture
    float particle_atlas[4] = { 1, 1, 0, 0 };  // cols, rows, has_texture, unused

    VkPipeline pipe_ui = VK_NULL_HANDLE;
    GpuBuffer ui_verts;

    // World-space line list with depth testing (editor grid, debug geometry):
    // an overlay would shine through every object in the scene.
    VkPipeline pipe_lines = VK_NULL_HANDLE;
    GpuBuffer lines;
    uint32_t lines_count = 0, lines_capacity = 0;
    float lines_color[4] = { 1, 1, 1, 1 };
    uint32_t ui_capacity = 0, ui_vertex_count = 0;
    std::vector<uint32_t> ui_batch_counts, ui_batch_textures;

    GpuBuffer lights;
    uint32_t light_capacity = 0, light_count = 0;
    int culling = 1;
    uint32_t last_culled = 0, last_visible = 0;

    GpuBuffer joints;                 // storage buffer of mat4, all characters
    uint32_t joint_capacity = 0, joint_count = 0;

    // state the host sets
    float eye[3] = { 8, 6, 12 }, target[3] = { 0, 1, 0 }, up[3] = { 0, 1, 0 };
    float fov = 55.0f, znear = 0.1f, zfar = 500.0f;
    // Orthographic when > 0: the value is half the visible height in world
    // units. This is the whole 2D mode - the renderer draws the same world,
    // it just stops applying perspective.
    float ortho_size = 0.0f;
    float view2_ortho = 0.0f;
    float sun_dir[3] = { 0.35f, 0.8f, 0.45f };
    float sun_color[3] = { 1.0f, 0.96f, 0.9f };
    float sun_intensity = 1.3f;
    float sky_color[3] = { 0.20f, 0.36f, 0.72f };
    float ground_color[3] = { 0.28f, 0.26f, 0.24f };
    float ambient = 0.30f;
    float fog_color[3] = { 0.55f, 0.63f, 0.74f };
    float fog_density = 0.0022f;
    float exposure = 0.55f;
    float clear[3] = { 0.07f, 0.08f, 0.10f };
    // The rectangle the WORLD pass draws into (pixels). 0 size = the whole
    // frame. The UI pass always gets the whole frame - panels must be able to
    // overlap the scene, the scene must not spill over the panels.
    float world_clip[4] = { 0, 0, 0, 0 };
    // Second world view (the Game panel docked next to the Scene panel): the
    // main pass then draws the world twice, once per rectangle, each with its
    // own camera. Re-armed by the host every frame, cleared at frame end.
    int   view2_active = 0;
    float view2_clip[4] = { 0, 0, 0, 0 };
    float view2_eye[3] = { 8, 6, 12 }, view2_target[3] = { 0, 1, 0 }, view2_up[3] = { 0, 1, 0 };
    float view2_fov = 60.0f;
    uint32_t ubo_stride = 0;   // FrameUBO aligned to minUniformBufferOffsetAlignment
    float shadow_radius = 30.0f;
    int   sky_enabled = 1;

    bool has_surface_ext = false;   // instance level VK_KHR_surface + xlib
    bool has_swapchain_ext = false;  // device level VK_KHR_swapchain

    // ---- post processing ------------------------------------------------
    // post_rt is the composite's destination and the readback source WHEN the
    // chain is on; with it off the frame never touches any of this and the
    // copy still comes straight out of color_rt, which is what keeps the
    // visual tests bit identical. bloom_a/bloom_b are the quarter resolution
    // ping pong pair - the blur cannot read and write one image, so there
    // have to be two.
    VkImage post_rt = VK_NULL_HANDLE, bloom_a = VK_NULL_HANDLE, bloom_b = VK_NULL_HANDLE;
    VkDeviceMemory post_rt_mem = VK_NULL_HANDLE, bloom_a_mem = VK_NULL_HANDLE, bloom_b_mem = VK_NULL_HANDLE;
    VkImageView post_rt_view = VK_NULL_HANDLE, bloom_a_view = VK_NULL_HANDLE, bloom_b_view = VK_NULL_HANDLE;
    uint32_t bloom_w = 0, bloom_h = 0;

    // A sampler of its own rather than tex_sampler: that one REPEATs, and a
    // blur tap past the edge of the bloom image would then wrap in light from
    // the opposite side of the screen - a bright sign on the left glowing
    // faintly off the right edge. Clamp is the only correct choice here.
    VkSampler post_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_dsl = VK_NULL_HANDLE;
    VkDescriptorPool post_pool = VK_NULL_HANDLE;
    // One set per image that is ever SAMPLED by a post pass. Allocated once at
    // init and rewritten on resize - allocating per frame would drain a fixed
    // pool in a few seconds of running.
    VkDescriptorSet set_color = VK_NULL_HANDLE, set_bloom_a = VK_NULL_HANDLE, set_bloom_b = VK_NULL_HANDLE;
    VkPipelineLayout post_layout = VK_NULL_HANDLE;
    VkPipeline pipe_post_bright = VK_NULL_HANDLE, pipe_post_blur = VK_NULL_HANDLE,
               pipe_post_comp = VK_NULL_HANDLE;
    dai_postfx postfx{};        // enabled = 0: the chain does nothing at all
    bool post_ready = false;    // images + pipelines all built
    // Did the LAST recorded frame go through the chain? What the window
    // backends blit from depends on it, and it is not the same question as
    // "is the chain enabled right now".
    bool post_used = false;

    char device_name[256] = {0};
    char err[256] = {0};
    double last_ms = 0.0;
    uint32_t last_draws = 0;
    bool have_frame = false;
};

bool vk_init_default_material(dai_renderer *r);

uint32_t vk_find_mem(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags want);
bool vk_make_buffer(dai_renderer *r, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, GpuBuffer *out, bool map);
void vk_free_buffer(dai_renderer *r, GpuBuffer *b);
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                uint32_t layers = 1);

/* A descriptor set bound to one texture, for draws that name a texture instead
 * of a material - the UI. Built on first use and cached on the entry. */
VkDescriptorSet vk_texture_set(dai_renderer *r, uint32_t tex);

// ---- post processing (src/rhi_vulkan_post.cpp) ----------------------------

// Shared with rhi_vulkan.cpp, which owns image creation and the shader loader.
bool make_image(dai_renderer *r, uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples,
                VkImageUsageFlags usage, VkImageAspectFlags aspect,
                VkImage *img, VkDeviceMemory *mem, VkImageView *view);
VkShaderModule load_module(dai_renderer *r, const char *name, bool *ok);

// Builds the sampler, the layouts and the three pipelines. Called once from
// dai_render_create. Returns false only if the SPIR-V is missing.
bool vk_post_init(dai_renderer *r);
// Creates post_rt and the two bloom images at the current size and points the
// descriptor sets at them. Called from create and again from every resize -
// a chain left pointing at a freed image is the resize bug this exists to
// prevent.
bool vk_post_make_targets(dai_renderer *r);
// Frees only the size dependent half, so a resize can rebuild it.
void vk_post_free_targets(dai_renderer *r);
void vk_post_destroy(dai_renderer *r);
// Records bright -> blur -> blur -> composite into an OPEN command buffer,
// between the UI pass and the readback copy. Returns the image the readback
// should be taken from: post_rt when it ran, color_rt when it did not.
VkImage vk_post_record(dai_renderer *r);

// The image the FINISHED frame lives in: post_rt when the chain ran for the
// last frame, color_rt otherwise. The window backends blit from this rather
// than naming color_rt, or a window would show the unprocessed frame while
// dai_render_readback returned the processed one - the same frame looking
// like two different pictures depending on how you asked for it.
VkImage vk_present_image(dai_renderer *r);

#endif
