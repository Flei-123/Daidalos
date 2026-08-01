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

// Push constant block, must match the shaders. 80 bytes: still well inside the
// 128 byte guarantee, so no uniform buffer traffic per material switch.
struct MaterialPush {
    float base_color[4];   // rgb + alpha cutoff
    float emissive[4];     // rgb + flags as float
    float scalars[4];      // metallic, roughness, normal strength, unused
    float extra[4];        // occlusion, has_maps, has_normal_map, shadow cascade
    float uv[4];           // tiling x, tiling y, offset x, offset y
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
    float shadow_radius = 30.0f;
    int   sky_enabled = 1;

    bool has_surface_ext = false;   // instance level VK_KHR_surface + xlib
    bool has_swapchain_ext = false;  // device level VK_KHR_swapchain

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

#endif
