// Daidalos - Vulkan 1.3 backend, part 1: device, targets, pipelines, meshes.
//
// Dynamic rendering (core in 1.3): no VkRenderPass, no VkFramebuffer anywhere.
// Renders offscreen into an image that can be read back, so the renderer is
// testable without a display - here through Mesa lavapipe, on a real GPU the
// exact same code path.

#include "rhi_vulkan.hpp"
#include "dai_meshgen.hpp"

#include <cstdio>
#include <cstdlib>

namespace daiimg {
bool write_png_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
bool write_ppm_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
}

// ---------------------------------------------------------------- math

Mat4 mat_identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r; }

Mat4 mat_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}

static void v_sub(const float a[3], const float b[3], float o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
static void v_cross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
static float v_dot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void v_norm(float a[3]) { float l = sqrtf(v_dot(a,a)); if (l > 1e-8f) { a[0]/=l; a[1]/=l; a[2]/=l; } }

Mat4 mat_look_at(const float eye[3], const float ctr[3], const float up[3]) {
    float f[3], s[3], u[3];
    v_sub(ctr, eye, f); v_norm(f);
    v_cross(f, up, s);  v_norm(s);
    v_cross(s, f, u);
    Mat4 r = mat_identity();
    r.m[0]=s[0]; r.m[4]=s[1]; r.m[8]=s[2];
    r.m[1]=u[0]; r.m[5]=u[1]; r.m[9]=u[2];
    r.m[2]=-f[0]; r.m[6]=-f[1]; r.m[10]=-f[2];
    r.m[12]=-v_dot(s, eye); r.m[13]=-v_dot(u, eye); r.m[14]=v_dot(f, eye);
    return r;
}

Mat4 mat_perspective(float fov_deg, float aspect, float zn, float zf) {
    float t = tanf(fov_deg * 3.14159265f / 360.0f);
    Mat4 r{};
    r.m[0]  = 1.0f / (aspect * t);
    r.m[5]  = -1.0f / t;                 // y down in Vulkan clip space
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

Mat4 mat_ortho(float l, float rr, float b, float t, float zn, float zf) {
    Mat4 r = mat_identity();
    r.m[0]  = 2.0f / (rr - l);
    r.m[5]  = -2.0f / (t - b);           // same y flip as the perspective matrix
    r.m[10] = 1.0f / (zn - zf);
    r.m[12] = -(rr + l) / (rr - l);
    r.m[13] = (t + b) / (t - b);
    r.m[14] = zn / (zn - zf);
    return r;
}

bool mat_invert(const Mat4 &in, Mat4 *out) {
    const float *m = in.m; float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out->m[i] = inv[i] * det;
    return true;
}

// ---------------------------------------------------------------- helpers

uint32_t vk_find_mem(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool vk_make_buffer(dai_renderer *r, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, GpuBuffer *out, bool map) {
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(r->dev, &bi, nullptr, &out->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(r->dev, out->buf, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_find_mem(r->phys, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(r->dev, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(r->dev, out->buf, out->mem, 0) != VK_SUCCESS) return false;
    out->size = size;
    if (map && vkMapMemory(r->dev, out->mem, 0, size, 0, &out->mapped) != VK_SUCCESS) return false;
    return true;
}

void vk_free_buffer(dai_renderer *r, GpuBuffer *b) {
    if (!b->buf) return;
    if (b->mapped) { vkUnmapMemory(r->dev, b->mem); b->mapped = nullptr; }
    vkDestroyBuffer(r->dev, b->buf, nullptr);
    vkFreeMemory(r->dev, b->mem, nullptr);
    b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE; b->size = 0;
}

void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = { aspect, 0, 1, 0, 1 };
    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

namespace {

bool make_image(dai_renderer *r, uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples,
                VkImageUsageFlags usage, VkImageAspectFlags aspect,
                VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->dev, &ii, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(r->dev, *img, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_find_mem(r->phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(r->dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(r->dev, *img, *mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    return vkCreateImageView(r->dev, &vi, nullptr, view) == VK_SUCCESS;
}

const char *spv_dir() {
    const char *e = std::getenv("DAI_SHADER_DIR");
    return e ? e : "shaders";
}

std::vector<uint32_t> load_spv(const char *path, bool *ok) {
    std::vector<uint32_t> out;
    FILE *f = std::fopen(path, "rb");
    if (!f) { *ok = false; return out; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)((n + 3) / 4));
    *ok = (std::fread(out.data(), 1, (size_t)n, f) == (size_t)n);
    std::fclose(f);
    return out;
}

VkShaderModule load_module(dai_renderer *r, const char *name, bool *ok) {
    char p[512];
    std::snprintf(p, sizeof(p), "%s/%s", spv_dir(), name);
    bool got = false;
    std::vector<uint32_t> code = load_spv(p, &got);
    if (!got) { *ok = false; return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = code.size() * 4; si.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(r->dev, &si, nullptr, &m) != VK_SUCCESS) *ok = false;
    return m;
}

// vertex layout shared by the mesh and shadow pipelines
struct VertexLayout {
    VkVertexInputBindingDescription binds[2];
    VkVertexInputAttributeDescription attrs[10];
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VertexLayout() {
        binds[0] = { 0, sizeof(dai_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
        binds[1] = { 1, sizeof(dai_render_instance), VK_VERTEX_INPUT_RATE_INSTANCE };
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(dai_vertex, position) };
        attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(dai_vertex, normal) };
        attrs[2] = { 2, 0, VK_FORMAT_R32_SFLOAT,       offsetof(dai_vertex, cap) };
        attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(dai_vertex, u) };
        attrs[4] = { 4, 1, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(dai_render_instance, position) };
        attrs[5] = { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(dai_render_instance, rotation) };
        attrs[6] = { 6, 1, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(dai_render_instance, scale) };
        attrs[7] = { 7, 1, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(dai_render_instance, color) };
        attrs[8] = { 8, 1, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(dai_render_instance, param) };
        attrs[9] = { 9, 1, VK_FORMAT_R32_UINT,            offsetof(dai_render_instance, flags) };
        vi.vertexBindingDescriptionCount = 2; vi.pVertexBindingDescriptions = binds;
        vi.vertexAttributeDescriptionCount = 10; vi.pVertexAttributeDescriptions = attrs;
    }
};

bool grow_geometry(dai_renderer *r, uint32_t need_verts, uint32_t need_idx) {
    VkDeviceSize vsize = (VkDeviceSize)(r->vtx_used + need_verts) * sizeof(dai_vertex);
    VkDeviceSize isize = (VkDeviceSize)(r->idx_used + need_idx) * sizeof(uint32_t);
    if (r->vbo.size >= vsize && r->ibo.size >= isize) return true;

    VkDeviceSize nv = r->vbo.size ? r->vbo.size : 1 << 16;
    while (nv < vsize) nv *= 2;
    VkDeviceSize ni = r->ibo.size ? r->ibo.size : 1 << 16;
    while (ni < isize) ni *= 2;

    GpuBuffer nvbo{}, nibo{};
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!vk_make_buffer(r, nv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, &nvbo, true)) return false;
    if (!vk_make_buffer(r, ni, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, &nibo, true)) return false;
    if (r->vbo.mapped) std::memcpy(nvbo.mapped, r->vbo.mapped, (size_t)r->vtx_used * sizeof(dai_vertex));
    if (r->ibo.mapped) std::memcpy(nibo.mapped, r->ibo.mapped, (size_t)r->idx_used * sizeof(uint32_t));
    vkDeviceWaitIdle(r->dev);
    vk_free_buffer(r, &r->vbo); vk_free_buffer(r, &r->ibo);
    r->vbo = nvbo; r->ibo = nibo;
    return true;
}

} // namespace

// ---------------------------------------------------------------- API

extern "C" {

dai_mesh dai_render_mesh_create(dai_renderer *r, const dai_vertex *verts, uint32_t vcount,
                                const uint32_t *indices, uint32_t icount) {
    if (!r || !verts || !vcount) return DAI_MESH_BOX;
    std::vector<uint32_t> gen;
    if (!indices || !icount) {
        gen.resize(vcount);
        for (uint32_t i = 0; i < vcount; ++i) gen[i] = i;
        indices = gen.data(); icount = vcount;
    }
    if (!grow_geometry(r, vcount, icount)) {
        std::snprintf(r->err, sizeof(r->err), "out of geometry memory");
        return DAI_MESH_BOX;
    }
    std::memcpy((dai_vertex *)r->vbo.mapped + r->vtx_used, verts, (size_t)vcount * sizeof(dai_vertex));
    std::memcpy((uint32_t *)r->ibo.mapped + r->idx_used, indices, (size_t)icount * sizeof(uint32_t));

    MeshEntry e;
    e.first_index = r->idx_used;
    e.index_count = icount;
    e.vertex_offset = (int32_t)r->vtx_used;
    r->vtx_used += vcount;
    r->idx_used += icount;
    r->meshes.push_back(e);
    return (dai_mesh)(r->meshes.size() - 1);
}

dai_mesh dai_render_mesh_load_obj(dai_renderer *r, const char *path) {
    if (!r || !path) return DAI_MESH_BOX;
    daimesh::Mesh m;
    if (!daimesh::load_obj(path, &m)) {
        std::snprintf(r->err, sizeof(r->err), "could not read OBJ: %s", path);
        return DAI_MESH_BOX;
    }
    return dai_render_mesh_create(r, m.verts.data(), (uint32_t)m.verts.size(),
                                  m.idx.data(), (uint32_t)m.idx.size());
}

uint32_t dai_render_mesh_count(dai_renderer *r) { return r ? (uint32_t)r->meshes.size() : 0; }
uint32_t dai_render_mesh_tris(dai_renderer *r, dai_mesh m) {
    if (!r || m >= r->meshes.size()) return 0;
    return r->meshes[m].index_count / 3;
}

dai_renderer *dai_render_create(const dai_render_desc *desc, char *err, size_t err_len) {
    auto fail = [&](const char *m) -> dai_renderer * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };
    dai_renderer *r = new dai_renderer();
    if (desc) {
        if (desc->width)  r->width  = desc->width;
        if (desc->height) r->height = desc->height;
        if (desc->shadow_size > 0) r->shadow_size = (uint32_t)desc->shadow_size;
        if (desc->shadow_size < 0) r->shadows = false;
        if (desc->msaa == 1) r->samples = VK_SAMPLE_COUNT_1_BIT;
        else if (desc->msaa == 2) r->samples = VK_SAMPLE_COUNT_2_BIT;
        else if (desc->msaa == 8) r->samples = VK_SAMPLE_COUNT_8_BIT;
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "daidalos";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    if (desc && desc->validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    if (vkCreateInstance(&ici, nullptr, &r->instance) != VK_SUCCESS) {
        ici.enabledLayerCount = 0;
        if (vkCreateInstance(&ici, nullptr, &r->instance) != VK_SUCCESS)
            { delete r; return fail("vkCreateInstance failed (no Vulkan 1.3 loader/driver?)"); }
    }

    uint32_t n = 0; vkEnumeratePhysicalDevices(r->instance, &n, nullptr);
    if (n == 0) { dai_render_destroy(r); return fail("no Vulkan device found"); }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(r->instance, &n, devs.data());
    r->phys = devs[0];
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { r->phys = d; break; }
    }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(r->phys, &props);
    std::snprintf(r->device_name, sizeof(r->device_name), "%s (Vulkan %u.%u.%u)",
        props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    // clamp MSAA to what the device can actually do for both attachments
    VkSampleCountFlags sup = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    while (r->samples != VK_SAMPLE_COUNT_1_BIT && !(sup & r->samples))
        r->samples = (VkSampleCountFlagBits)(r->samples >> 1);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(r->phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(r->phys, &qn, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { r->qfam = i; found = true; break; }
    if (!found) { dai_render_destroy(r); return fail("no graphics queue family"); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = r->qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f13;
    f2.features.depthClamp = VK_TRUE;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(r->phys, &dci, nullptr, &r->dev) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("vkCreateDevice failed (dynamicRendering/sync2 missing?)"); }
    vkGetDeviceQueue(r->dev, r->qfam, 0, &r->queue);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = r->qfam;
    if (vkCreateCommandPool(r->dev, &pci, nullptr, &r->pool) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("vkCreateCommandPool failed"); }
    VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = r->pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(r->dev, &cbi, &r->cmd);
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(r->dev, &fci, nullptr, &r->fence);

    // ---- targets
    if (!make_image(r, r->width, r->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->color_rt, &r->color_rt_mem, &r->color_rt_view))
        { dai_render_destroy(r); return fail("colour target could not be created"); }
    if (r->samples != VK_SAMPLE_COUNT_1_BIT &&
        !make_image(r, r->width, r->height, VK_FORMAT_R8G8B8A8_UNORM, r->samples,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    &r->color_ms, &r->color_ms_mem, &r->color_ms_view))
        { dai_render_destroy(r); return fail("multisample target could not be created"); }
    if (!make_image(r, r->width, r->height, VK_FORMAT_D32_SFLOAT, r->samples,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    &r->depth, &r->depth_mem, &r->depth_view))
        { dai_render_destroy(r); return fail("depth target could not be created"); }

    uint32_t ss = r->shadows ? r->shadow_size : 1;
    if (!make_image(r, ss, ss, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &r->shadow_img, &r->shadow_mem, &r->shadow_view))
        { dai_render_destroy(r); return fail("shadow map could not be created"); }

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.compareEnable = VK_TRUE; sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(r->dev, &sci, nullptr, &r->shadow_sampler) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("shadow sampler failed"); }

    // ---- buffers
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!vk_make_buffer(r, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, &r->ubo, true))
        { dai_render_destroy(r); return fail("uniform buffer failed"); }
    if (!vk_make_buffer(r, (VkDeviceSize)r->width * r->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        host, &r->readback, false))
        { dai_render_destroy(r); return fail("readback buffer failed"); }
    r->inst_capacity = 1024;
    if (!vk_make_buffer(r, (VkDeviceSize)r->inst_capacity * sizeof(dai_render_instance),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, &r->inst, true))
        { dai_render_destroy(r); return fail("instance buffer failed"); }

    // ---- descriptors
    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; lb[0].descriptorCount = 1;
    lb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    lb[1].binding = 1; lb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[1].descriptorCount = 1;
    lb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 2; dlc.pBindings = lb;
    if (vkCreateDescriptorSetLayout(r->dev, &dlc, nullptr, &r->dsl) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("descriptor layout failed"); }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo dpc{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = 1; dpc.poolSizeCount = 2; dpc.pPoolSizes = ps;
    if (vkCreateDescriptorPool(r->dev, &dpc, nullptr, &r->dpool) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("descriptor pool failed"); }
    VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsa.descriptorPool = r->dpool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &r->dsl;
    if (vkAllocateDescriptorSets(r->dev, &dsa, &r->dset) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("descriptor set failed"); }

    VkDescriptorBufferInfo dbi{ r->ubo.buf, 0, sizeof(FrameUBO) };
    VkDescriptorImageInfo dii{ r->shadow_sampler, r->shadow_view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr[2]{};
    wr[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr[0].dstSet = r->dset; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].pBufferInfo = &dbi;
    wr[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr[1].dstSet = r->dset; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &dii;
    vkUpdateDescriptorSets(r->dev, 2, wr, 0, nullptr);

    // ---- material descriptors: 4 samplers per material, parameters go in
    //      push constants so switching material is one bind and no upload
    VkDescriptorSetLayoutBinding mb[4]{};
    for (int i = 0; i < 4; ++i) {
        mb[i].binding = (uint32_t)i;
        mb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mb[i].descriptorCount = 1;
        mb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo mlc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    mlc.bindingCount = 4; mlc.pBindings = mb;
    if (vkCreateDescriptorSetLayout(r->dev, &mlc, nullptr, &r->mat_dsl) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("material descriptor layout failed"); }
    VkDescriptorPoolSize mps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * DAI_MAX_MATERIALS };
    VkDescriptorPoolCreateInfo mpc{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    mpc.maxSets = DAI_MAX_MATERIALS; mpc.poolSizeCount = 1; mpc.pPoolSizes = &mps;
    if (vkCreateDescriptorPool(r->dev, &mpc, nullptr, &r->mat_pool) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("material descriptor pool failed"); }

    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(r->phys, &feats);
    VkSamplerCreateInfo tsi{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    tsi.magFilter = tsi.minFilter = VK_FILTER_LINEAR;
    tsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    tsi.addressModeU = tsi.addressModeV = tsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    tsi.maxLod = VK_LOD_CLAMP_NONE;
    tsi.anisotropyEnable = VK_FALSE;      // requested feature is optional, keep it simple
    if (vkCreateSampler(r->dev, &tsi, nullptr, &r->tex_sampler) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("texture sampler failed"); }

    // ---- pipelines
    bool ok = true;
    VkShaderModule vs_mesh = load_module(r, "mesh.vert.spv", &ok);
    VkShaderModule fs_mesh = load_module(r, "mesh.frag.spv", &ok);
    VkShaderModule vs_shadow = load_module(r, "shadow.vert.spv", &ok);
    VkShaderModule vs_sky = load_module(r, "sky.vert.spv", &ok);
    VkShaderModule fs_sky = load_module(r, "sky.frag.spv", &ok);
    if (!ok) { dai_render_destroy(r); return fail("SPIR-V shaders not found (set DAI_SHADER_DIR)"); }

    VkDescriptorSetLayout sets[2] = { r->dsl, r->mat_dsl };
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(MaterialPush);
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 2; plci.pSetLayouts = sets;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(r->dev, &plci, nullptr, &r->layout);

    VertexLayout vl;
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    // Meshes are wound counter clockwise seen from OUTSIDE. The y flip in the
    // projection and the right handed view matrix cancel out, so outward faces
    // stay counter clockwise in framebuffer space. Getting this backwards is
    // subtle: you keep seeing the INSIDE of every object, which reads as
    // "everything is lit from the wrong side" rather than as a culling bug.
    // tests/test_render_visual.cpp [4] and [9] pin it down.
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = r->samples;
    VkPipelineMultisampleStateCreateInfo ms1{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 1; prc.pColorAttachmentFormats = &colorFmt;
    prc.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        VkPipelineShaderStageCreateInfo i{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        i.stage = s; i.module = m; i.pName = "main";
        return i;
    };

    VkPipelineShaderStageCreateInfo st_mesh[2] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs_mesh),
                                                   stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs_mesh) };
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &prc; gp.stageCount = 2; gp.pStages = st_mesh;
    gp.pVertexInputState = &vl.vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dys; gp.layout = r->layout;
    VkResult pr = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gp, nullptr, &r->pipe_mesh);

    // sky: no vertex input, no depth write, draws behind everything
    VkPipelineVertexInputStateCreateInfo empty_vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds_sky{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds_sky.depthTestEnable = VK_FALSE; ds_sky.depthWriteEnable = VK_FALSE;
    VkPipelineRasterizationStateCreateInfo rs_sky = rs; rs_sky.cullMode = VK_CULL_MODE_NONE;
    VkPipelineShaderStageCreateInfo st_sky[2] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs_sky),
                                                  stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs_sky) };
    VkGraphicsPipelineCreateInfo gs = gp;
    gs.pStages = st_sky; gs.pVertexInputState = &empty_vi;
    gs.pDepthStencilState = &ds_sky; gs.pRasterizationState = &rs_sky;
    VkResult sr = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gs, nullptr, &r->pipe_sky);

    // shadow: depth only, front faces culled to push peter-panning off the
    // visible surfaces, depth clamp so casters behind the light plane survive
    VkPipelineRenderingCreateInfo prc_sh{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc_sh.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkPipelineRasterizationStateCreateInfo rs_sh = rs;
    rs_sh.cullMode = VK_CULL_MODE_FRONT_BIT;
    rs_sh.depthClampEnable = VK_TRUE;
    VkPipelineColorBlendStateCreateInfo cb_sh{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st_sh[1] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs_shadow) };
    VkGraphicsPipelineCreateInfo gsh = gp;
    gsh.pNext = &prc_sh; gsh.stageCount = 1; gsh.pStages = st_sh;
    gsh.pRasterizationState = &rs_sh; gsh.pMultisampleState = &ms1;
    gsh.pColorBlendState = &cb_sh;
    VkResult hr = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gsh, nullptr, &r->pipe_shadow);

    for (VkShaderModule m : { vs_mesh, fs_mesh, vs_shadow, vs_sky, fs_sky })
        vkDestroyShaderModule(r->dev, m, nullptr);
    if (pr != VK_SUCCESS || sr != VK_SUCCESS || hr != VK_SUCCESS)
        { dai_render_destroy(r); return fail("vkCreateGraphicsPipelines failed"); }

    // ---- built in meshes, in the order of the DAI_MESH_* enum
    daimesh::Mesh gen[DAI_MESH_BUILTIN_COUNT];
    gen[DAI_MESH_BOX] = daimesh::box();
    gen[DAI_MESH_SPHERE] = daimesh::sphere();
    gen[DAI_MESH_CAPSULE] = daimesh::capsule();
    gen[DAI_MESH_CYLINDER] = daimesh::cylinder();
    gen[DAI_MESH_CONE] = daimesh::cone();
    gen[DAI_MESH_PLANE] = daimesh::plane();
    for (int i = 0; i < DAI_MESH_BUILTIN_COUNT; ++i)
        dai_render_mesh_create(r, gen[i].verts.data(), (uint32_t)gen[i].verts.size(),
                               gen[i].idx.data(), (uint32_t)gen[i].idx.size());

    if (!vk_init_default_material(r)) { dai_render_destroy(r); return fail("default material failed"); }

    return r;
}

void dai_render_destroy(dai_renderer *r) {
    if (!r) return;
    if (r->dev) {
        vkDeviceWaitIdle(r->dev);
        if (r->pipe_mesh) vkDestroyPipeline(r->dev, r->pipe_mesh, nullptr);
        if (r->pipe_sky) vkDestroyPipeline(r->dev, r->pipe_sky, nullptr);
        if (r->pipe_shadow) vkDestroyPipeline(r->dev, r->pipe_shadow, nullptr);
        if (r->layout) vkDestroyPipelineLayout(r->dev, r->layout, nullptr);
        if (r->dpool) vkDestroyDescriptorPool(r->dev, r->dpool, nullptr);
        if (r->dsl) vkDestroyDescriptorSetLayout(r->dev, r->dsl, nullptr);
        vk_free_buffer(r, &r->vbo); vk_free_buffer(r, &r->ibo);
        vk_free_buffer(r, &r->inst); vk_free_buffer(r, &r->ubo); vk_free_buffer(r, &r->readback);
        if (r->shadow_sampler) vkDestroySampler(r->dev, r->shadow_sampler, nullptr);
        if (r->tex_sampler) vkDestroySampler(r->dev, r->tex_sampler, nullptr);
        if (r->mat_pool) vkDestroyDescriptorPool(r->dev, r->mat_pool, nullptr);
        if (r->mat_dsl) vkDestroyDescriptorSetLayout(r->dev, r->mat_dsl, nullptr);
        for (TextureEntry &t : r->textures) {
            if (t.view) vkDestroyImageView(r->dev, t.view, nullptr);
            if (t.image) vkDestroyImage(r->dev, t.image, nullptr);
            if (t.mem) vkFreeMemory(r->dev, t.mem, nullptr);
        }
        for (auto v : { r->color_ms_view, r->color_rt_view, r->depth_view, r->shadow_view })
            if (v) vkDestroyImageView(r->dev, v, nullptr);
        if (r->color_ms) { vkDestroyImage(r->dev, r->color_ms, nullptr); vkFreeMemory(r->dev, r->color_ms_mem, nullptr); }
        if (r->color_rt) { vkDestroyImage(r->dev, r->color_rt, nullptr); vkFreeMemory(r->dev, r->color_rt_mem, nullptr); }
        if (r->depth) { vkDestroyImage(r->dev, r->depth, nullptr); vkFreeMemory(r->dev, r->depth_mem, nullptr); }
        if (r->shadow_img) { vkDestroyImage(r->dev, r->shadow_img, nullptr); vkFreeMemory(r->dev, r->shadow_mem, nullptr); }
        if (r->fence) vkDestroyFence(r->dev, r->fence, nullptr);
        if (r->pool) vkDestroyCommandPool(r->dev, r->pool, nullptr);
        vkDestroyDevice(r->dev, nullptr);
    }
    if (r->instance) vkDestroyInstance(r->instance, nullptr);
    delete r;
}

const char *dai_render_device_name(dai_renderer *r) { return r ? r->device_name : "none"; }
const char *dai_render_last_error(dai_renderer *r) { return r ? r->err : "no renderer"; }
uint32_t dai_render_width(dai_renderer *r) { return r ? r->width : 0; }
uint32_t dai_render_height(dai_renderer *r) { return r ? r->height : 0; }
double dai_render_last_ms(dai_renderer *r) { return r ? r->last_ms : 0.0; }
uint32_t dai_render_last_draws(dai_renderer *r) { return r ? r->last_draws : 0; }

void dai_render_camera(dai_renderer *r, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                       float fov_deg, float znear, float zfar) {
    if (!r) return;
    r->eye[0]=eye.x; r->eye[1]=eye.y; r->eye[2]=eye.z;
    r->target[0]=target.x; r->target[1]=target.y; r->target[2]=target.z;
    r->up[0]=up.x; r->up[1]=up.y; r->up[2]=up.z;
    r->fov = fov_deg; r->znear = znear; r->zfar = zfar;
}

void dai_render_light(dai_renderer *r, dai_vec3 d) {
    if (!r) return;
    float v[3] = { d.x, d.y, d.z }; v_norm(v);
    r->sun_dir[0]=v[0]; r->sun_dir[1]=v[1]; r->sun_dir[2]=v[2];
}

void dai_render_sun(dai_renderer *r, dai_vec3 dir, dai_vec3 color, float intensity) {
    if (!r) return;
    dai_render_light(r, dir);
    r->sun_color[0]=color.x; r->sun_color[1]=color.y; r->sun_color[2]=color.z;
    r->sun_intensity = intensity;
}

void dai_render_ambient(dai_renderer *r, dai_vec3 sky, dai_vec3 ground, float intensity) {
    if (!r) return;
    r->sky_color[0]=sky.x; r->sky_color[1]=sky.y; r->sky_color[2]=sky.z;
    r->ground_color[0]=ground.x; r->ground_color[1]=ground.y; r->ground_color[2]=ground.z;
    r->ambient = intensity;
}

void dai_render_clear_color(dai_renderer *r, float rr, float gg, float bb) {
    if (!r) return;
    r->clear[0]=rr; r->clear[1]=gg; r->clear[2]=bb;
}

void dai_render_sky(dai_renderer *r, int enabled) { if (r) r->sky_enabled = enabled; }

void dai_render_fog(dai_renderer *r, float density, dai_vec3 color) {
    if (!r) return;
    r->fog_density = density;
    r->fog_color[0]=color.x; r->fog_color[1]=color.y; r->fog_color[2]=color.z;
}

void dai_render_shadow_extent(dai_renderer *r, float radius) { if (r && radius > 0.1f) r->shadow_radius = radius; }
void dai_render_exposure(dai_renderer *r, float e) { if (r && e > 0.0f) r->exposure = e; }

dai_result dai_render_readback(dai_renderer *r, uint8_t *rgba, size_t size) {
    if (!r || !rgba) return DAI_ERR_INVALID_ARG;
    if (!r->have_frame) return DAI_ERR_STATE;
    size_t need = (size_t)r->width * r->height * 4;
    if (size < need) return DAI_ERR_INVALID_ARG;
    void *p = nullptr;
    if (vkMapMemory(r->dev, r->readback.mem, 0, need, 0, &p) != VK_SUCCESS) return DAI_ERR_STATE;
    std::memcpy(rgba, p, need);
    vkUnmapMemory(r->dev, r->readback.mem);
    return DAI_OK;
}

dai_result dai_render_write_ppm(dai_renderer *r, const char *path) {
    if (!r || !path) return DAI_ERR_INVALID_ARG;
    std::vector<uint8_t> px((size_t)r->width * r->height * 4);
    dai_result rr = dai_render_readback(r, px.data(), px.size());
    if (rr != DAI_OK) return rr;
    return daiimg::write_ppm_rgb(path, px.data(), r->width, r->height) ? DAI_OK : DAI_ERR_FILE;
}

dai_result dai_render_write_png(dai_renderer *r, const char *path) {
    if (!r || !path) return DAI_ERR_INVALID_ARG;
    std::vector<uint8_t> px((size_t)r->width * r->height * 4);
    dai_result rr = dai_render_readback(r, px.data(), px.size());
    if (rr != DAI_OK) return rr;
    return daiimg::write_png_rgb(path, px.data(), r->width, r->height) ? DAI_OK : DAI_ERR_FILE;
}

} // extern "C"
