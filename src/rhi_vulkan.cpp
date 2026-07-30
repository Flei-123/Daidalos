// Daidalos - Vulkan 1.3 backend.
//
// Uses dynamic rendering (core in 1.3), so there is no VkRenderPass and no
// VkFramebuffer anywhere in this file. Renders offscreen into an image that
// can be read back, which makes the renderer testable without a display -
// on this machine it runs on Mesa lavapipe, on the target machine it runs on
// the GPU with the exact same code path.

#include "dai_render.h"

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <vector>

// ---------------------------------------------------------------- math

namespace {

struct Mat4 { float m[16]; };

Mat4 mat_identity() {
    Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r;
}

Mat4 mat_mul(const Mat4 &a, const Mat4 &b) {   // column major, r = a * b
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}

void v_sub(const float a[3], const float b[3], float o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
void v_cross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
float v_dot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
void v_norm(float a[3]) {
    float l = sqrtf(v_dot(a, a)); if (l > 1e-8f) { a[0]/=l; a[1]/=l; a[2]/=l; }
}

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

// Vulkan clip space: y down, z in [0,1]. Both are folded in here so the
// shader stays a plain "viewproj * position".
Mat4 mat_perspective(float fov_deg, float aspect, float zn, float zf) {
    float t = tanf(fov_deg * 3.14159265f / 360.0f);
    Mat4 r{};
    r.m[0]  = 1.0f / (aspect * t);
    r.m[5]  = -1.0f / t;                 // flip Y for Vulkan
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

// ---------------------------------------------------------------- cube mesh

struct Vtx { float p[3]; float n[3]; };

const Vtx kCube[36] = {
    // +X
    {{ 1,-1,-1},{1,0,0}}, {{ 1, 1, 1},{1,0,0}}, {{ 1, 1,-1},{1,0,0}},
    {{ 1,-1,-1},{1,0,0}}, {{ 1,-1, 1},{1,0,0}}, {{ 1, 1, 1},{1,0,0}},
    // -X
    {{-1,-1,-1},{-1,0,0}}, {{-1, 1,-1},{-1,0,0}}, {{-1, 1, 1},{-1,0,0}},
    {{-1,-1,-1},{-1,0,0}}, {{-1, 1, 1},{-1,0,0}}, {{-1,-1, 1},{-1,0,0}},
    // +Y
    {{-1, 1,-1},{0,1,0}}, {{ 1, 1,-1},{0,1,0}}, {{ 1, 1, 1},{0,1,0}},
    {{-1, 1,-1},{0,1,0}}, {{ 1, 1, 1},{0,1,0}}, {{-1, 1, 1},{0,1,0}},
    // -Y
    {{-1,-1,-1},{0,-1,0}}, {{ 1,-1, 1},{0,-1,0}}, {{ 1,-1,-1},{0,-1,0}},
    {{-1,-1,-1},{0,-1,0}}, {{-1,-1, 1},{0,-1,0}}, {{ 1,-1, 1},{0,-1,0}},
    // +Z
    {{-1,-1, 1},{0,0,1}}, {{ 1,-1, 1},{0,0,1}}, {{ 1, 1, 1},{0,0,1}},
    {{-1,-1, 1},{0,0,1}}, {{ 1, 1, 1},{0,0,1}}, {{-1, 1, 1},{0,0,1}},
    // -Z
    {{-1,-1,-1},{0,0,-1}}, {{ 1, 1,-1},{0,0,-1}}, {{ 1,-1,-1},{0,0,-1}},
    {{-1,-1,-1},{0,0,-1}}, {{-1, 1,-1},{0,0,-1}}, {{ 1, 1,-1},{0,0,-1}},
};

struct PushConstants {
    Mat4  viewproj;
    float light[4];
};

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

} // namespace

// ---------------------------------------------------------------- renderer

struct dai_renderer {
    uint32_t width = 1280, height = 720;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;

    VkBuffer vbo = VK_NULL_HANDLE, ibo = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory vbo_mem = VK_NULL_HANDLE, ibo_mem = VK_NULL_HANDLE, readback_mem = VK_NULL_HANDLE;
    void *ibo_mapped = nullptr;
    uint32_t instance_capacity = 0;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    PushConstants pc{};
    float clear[3] = { 0.07f, 0.08f, 0.10f };
    char device_name[256] = {0};
    double last_ms = 0.0;
    bool   have_frame = false;
};

namespace {

uint32_t find_mem(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool make_buffer(dai_renderer *r, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags props, VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(r->dev, &bi, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(r->dev, *buf, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(r->phys, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(r->dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
    return vkBindBufferMemory(r->dev, *buf, *mem, 0) == VK_SUCCESS;
}

bool make_image(dai_renderer *r, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = { r->width, r->height, 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->dev, &ii, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(r->dev, *img, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(r->phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(r->dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(r->dev, *img, *mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    return vkCreateImageView(r->dev, &vi, nullptr, view) == VK_SUCCESS;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
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

bool ensure_instance_buffer(dai_renderer *r, uint32_t count) {
    if (count <= r->instance_capacity) return true;
    uint32_t cap = r->instance_capacity ? r->instance_capacity : 256;
    while (cap < count) cap *= 2;
    if (r->ibo) { vkDestroyBuffer(r->dev, r->ibo, nullptr); vkFreeMemory(r->dev, r->ibo_mem, nullptr); }
    VkDeviceSize size = (VkDeviceSize)cap * sizeof(dai_render_instance);
    if (!make_buffer(r, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &r->ibo, &r->ibo_mem)) return false;
    if (vkMapMemory(r->dev, r->ibo_mem, 0, size, 0, &r->ibo_mapped) != VK_SUCCESS) return false;
    r->instance_capacity = cap;
    return true;
}

const char *spv_dir() {
    const char *e = std::getenv("DAI_SHADER_DIR");
    return e ? e : "shaders";
}

} // namespace

// ---------------------------------------------------------------- API

extern "C" {

dai_renderer *dai_render_create(const dai_render_desc *desc, char *err, size_t err_len) {
    auto fail = [&](const char *m) -> dai_renderer * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };

    dai_renderer *r = new dai_renderer();
    if (desc) {
        if (desc->width)  r->width  = desc->width;
        if (desc->height) r->height = desc->height;
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "daidalos";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    if (desc && desc->validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    if (vkCreateInstance(&ici, nullptr, &r->instance) != VK_SUCCESS) {
        // retry without the validation layer, it is often simply not installed
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
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f13;

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

    if (!make_image(r, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->color, &r->color_mem, &r->color_view))
        { dai_render_destroy(r); return fail("colour target could not be created"); }
    if (!make_image(r, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &r->depth, &r->depth_mem, &r->depth_view))
        { dai_render_destroy(r); return fail("depth target could not be created"); }

    if (!make_buffer(r, sizeof(kCube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &r->vbo, &r->vbo_mem))
        { dai_render_destroy(r); return fail("vertex buffer failed"); }
    void *p = nullptr;
    vkMapMemory(r->dev, r->vbo_mem, 0, sizeof(kCube), 0, &p);
    std::memcpy(p, kCube, sizeof(kCube));
    vkUnmapMemory(r->dev, r->vbo_mem);

    if (!make_buffer(r, (VkDeviceSize)r->width * r->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &r->readback, &r->readback_mem))
        { dai_render_destroy(r); return fail("readback buffer failed"); }
    if (!ensure_instance_buffer(r, 1024))
        { dai_render_destroy(r); return fail("instance buffer failed"); }

    // ---- pipeline
    char vp[512], fp[512];
    std::snprintf(vp, sizeof(vp), "%s/mesh.vert.spv", spv_dir());
    std::snprintf(fp, sizeof(fp), "%s/mesh.frag.spv", spv_dir());
    bool ok1 = false, ok2 = false;
    std::vector<uint32_t> vs = load_spv(vp, &ok1), fs = load_spv(fp, &ok2);
    if (!ok1 || !ok2) { dai_render_destroy(r); return fail("SPIR-V not found (set DAI_SHADER_DIR)"); }

    VkShaderModuleCreateInfo smv{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smv.codeSize = vs.size() * 4; smv.pCode = vs.data();
    VkShaderModuleCreateInfo smf{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smf.codeSize = fs.size() * 4; smf.pCode = fs.data();
    VkShaderModule mv, mf;
    if (vkCreateShaderModule(r->dev, &smv, nullptr, &mv) != VK_SUCCESS ||
        vkCreateShaderModule(r->dev, &smf, nullptr, &mf) != VK_SUCCESS)
        { dai_render_destroy(r); return fail("shader module failed"); }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(r->dev, &plci, nullptr, &r->layout);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = mv; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = mf; stages[1].pName = "main";

    VkVertexInputBindingDescription binds[2]{};
    binds[0] = { 0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX };
    binds[1] = { 1, sizeof(dai_render_instance), VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attrs[6]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, p) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, n) };
    attrs[2] = { 2, 1, VK_FORMAT_R32G32B32_SFLOAT,   offsetof(dai_render_instance, position) };
    attrs[3] = { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(dai_render_instance, rotation) };
    attrs[4] = { 4, 1, VK_FORMAT_R32G32B32_SFLOAT,   offsetof(dai_render_instance, half_extent) };
    attrs[5] = { 5, 1, VK_FORMAT_R32G32B32_SFLOAT,   offsetof(dai_render_instance, color) };

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 2; vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vpt{ 0, 0, (float)r->width, (float)r->height, 0.0f, 1.0f };
    VkRect2D sc{ { 0, 0 }, { r->width, r->height } };
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vpt; vps.scissorCount = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 1; prc.pColorAttachmentFormats = &colorFmt;
    prc.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &prc; gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.layout = r->layout;
    VkResult pr = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gp, nullptr, &r->pipeline);
    vkDestroyShaderModule(r->dev, mv, nullptr);
    vkDestroyShaderModule(r->dev, mf, nullptr);
    if (pr != VK_SUCCESS) { dai_render_destroy(r); return fail("vkCreateGraphicsPipelines failed"); }

    r->pc.viewproj = mat_identity();
    r->pc.light[0] = 0.4f; r->pc.light[1] = 0.8f; r->pc.light[2] = 0.45f; r->pc.light[3] = 0.0f;
    dai_render_camera(r, dai_vec3{ 8, 6, 12 }, dai_vec3{ 0, 1, 0 }, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 500.0f);
    return r;
}

void dai_render_destroy(dai_renderer *r) {
    if (!r) return;
    if (r->dev) {
        vkDeviceWaitIdle(r->dev);
        if (r->pipeline) vkDestroyPipeline(r->dev, r->pipeline, nullptr);
        if (r->layout)   vkDestroyPipelineLayout(r->dev, r->layout, nullptr);
        if (r->ibo_mapped) vkUnmapMemory(r->dev, r->ibo_mem);
        if (r->ibo)      { vkDestroyBuffer(r->dev, r->ibo, nullptr); vkFreeMemory(r->dev, r->ibo_mem, nullptr); }
        if (r->vbo)      { vkDestroyBuffer(r->dev, r->vbo, nullptr); vkFreeMemory(r->dev, r->vbo_mem, nullptr); }
        if (r->readback) { vkDestroyBuffer(r->dev, r->readback, nullptr); vkFreeMemory(r->dev, r->readback_mem, nullptr); }
        if (r->color_view) vkDestroyImageView(r->dev, r->color_view, nullptr);
        if (r->depth_view) vkDestroyImageView(r->dev, r->depth_view, nullptr);
        if (r->color) { vkDestroyImage(r->dev, r->color, nullptr); vkFreeMemory(r->dev, r->color_mem, nullptr); }
        if (r->depth) { vkDestroyImage(r->dev, r->depth, nullptr); vkFreeMemory(r->dev, r->depth_mem, nullptr); }
        if (r->fence) vkDestroyFence(r->dev, r->fence, nullptr);
        if (r->pool)  vkDestroyCommandPool(r->dev, r->pool, nullptr);
        vkDestroyDevice(r->dev, nullptr);
    }
    if (r->instance) vkDestroyInstance(r->instance, nullptr);
    delete r;
}

const char *dai_render_device_name(dai_renderer *r) { return r ? r->device_name : "none"; }
uint32_t dai_render_width(dai_renderer *r)  { return r ? r->width : 0; }
uint32_t dai_render_height(dai_renderer *r) { return r ? r->height : 0; }
double   dai_render_last_ms(dai_renderer *r) { return r ? r->last_ms : 0.0; }

void dai_render_camera(dai_renderer *r, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                       float fov_deg, float znear, float zfar) {
    if (!r) return;
    float e[3] = { eye.x, eye.y, eye.z }, c[3] = { target.x, target.y, target.z }, u[3] = { up.x, up.y, up.z };
    Mat4 view = mat_look_at(e, c, u);
    Mat4 proj = mat_perspective(fov_deg, (float)r->width / (float)r->height, znear, zfar);
    r->pc.viewproj = mat_mul(proj, view);
}

void dai_render_light(dai_renderer *r, dai_vec3 d) {
    if (!r) return;
    float v[3] = { d.x, d.y, d.z }; v_norm(v);
    r->pc.light[0] = v[0]; r->pc.light[1] = v[1]; r->pc.light[2] = v[2];
}

void dai_render_clear_color(dai_renderer *r, float rr, float gg, float bb) {
    if (!r) return;
    r->clear[0] = rr; r->clear[1] = gg; r->clear[2] = bb;
}

dai_result dai_render_frame(dai_renderer *r, const dai_render_instance *inst, uint32_t count) {
    if (!r) return DAI_ERR_INVALID_ARG;
    if (!ensure_instance_buffer(r, count ? count : 1)) return DAI_ERR_OUT_OF_MEMORY;
    if (inst && count) std::memcpy(r->ibo_mapped, inst, (size_t)count * sizeof(dai_render_instance));

    auto t0 = std::chrono::high_resolution_clock::now();
    vkResetFences(r->dev, 1, &r->fence);
    vkResetCommandBuffer(r->cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(r->cmd, &bi);

    barrier(r->cmd, r->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(r->cmd, r->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = { { r->clear[0], r->clear[1], r->clear[2], 1.0f } };
    VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { r->width, r->height } };
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(r->cmd, &ri);

    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &r->pc);
    VkDeviceSize off[2] = { 0, 0 };
    VkBuffer bufs[2] = { r->vbo, r->ibo };
    vkCmdBindVertexBuffers(r->cmd, 0, 2, bufs, off);
    if (count) vkCmdDraw(r->cmd, 36, count, 0, 0);
    vkCmdEndRendering(r->cmd);

    barrier(r->cmd, r->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { r->width, r->height, 1 };
    vkCmdCopyImageToBuffer(r->cmd, r->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->readback, 1, &region);

    vkEndCommandBuffer(r->cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &r->cmd;
    if (vkQueueSubmit(r->queue, 1, &si, r->fence) != VK_SUCCESS) return DAI_ERR_STATE;
    vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, UINT64_MAX);

    r->last_ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    r->have_frame = true;
    return DAI_OK;
}

dai_result dai_render_readback(dai_renderer *r, uint8_t *rgba, size_t size) {
    if (!r || !rgba) return DAI_ERR_INVALID_ARG;
    if (!r->have_frame) return DAI_ERR_STATE;
    size_t need = (size_t)r->width * r->height * 4;
    if (size < need) return DAI_ERR_INVALID_ARG;
    void *p = nullptr;
    if (vkMapMemory(r->dev, r->readback_mem, 0, need, 0, &p) != VK_SUCCESS) return DAI_ERR_STATE;
    std::memcpy(rgba, p, need);
    vkUnmapMemory(r->dev, r->readback_mem);
    return DAI_OK;
}

dai_result dai_render_write_ppm(dai_renderer *r, const char *path) {
    if (!r || !path) return DAI_ERR_INVALID_ARG;
    std::vector<uint8_t> px((size_t)r->width * r->height * 4);
    dai_result rr = dai_render_readback(r, px.data(), px.size());
    if (rr != DAI_OK) return rr;
    FILE *f = std::fopen(path, "wb");
    if (!f) return DAI_ERR_FILE;
    std::fprintf(f, "P6\n%u %u\n255\n", r->width, r->height);
    for (size_t i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
    std::fclose(f);
    return DAI_OK;
}

} // extern "C"
