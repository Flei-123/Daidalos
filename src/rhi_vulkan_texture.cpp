// Daidalos - Vulkan backend, part 3: textures and materials.
//
// Textures are uploaded through a staging buffer and get a full mip chain
// built with vkCmdBlitImage. Materials are a descriptor set with four
// samplers; the scalar parameters ride in push constants, so switching
// material costs one descriptor bind and no buffer traffic.

#include "rhi_vulkan.hpp"
#include <cstdio>
#include <cstring>

namespace daiimg {
bool read_png_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                   char *err, size_t err_len);
}

namespace {

uint32_t mip_levels(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; ++n; }
    return n;
}

void one_shot_begin(dai_renderer *r, VkCommandBuffer *cb) {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = r->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(r->dev, &ai, cb);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(*cb, &bi);
}

void one_shot_end(dai_renderer *r, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->queue);
    vkFreeCommandBuffers(r->dev, r->pool, 1, &cb);
}

// layout transition for one mip level of a texture
void tex_barrier(VkCommandBuffer cb, VkImage img, uint32_t mip, uint32_t count,
                 VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                 VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1 };
    VkDependencyInfo d{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

} // namespace

// Creates the descriptor set for a material. Called for the default material
// during startup and for every user material afterwards.
static bool build_material_set(dai_renderer *r, MaterialEntry &m) {
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = r->mat_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &r->mat_dsl;
    if (vkAllocateDescriptorSets(r->dev, &ai, &m.set) != VK_SUCCESS) return false;

    VkDescriptorImageInfo info[4]{};
    const uint32_t tex[4] = { m.base_tex, m.orm_tex, m.normal_tex, m.emissive_tex };
    VkWriteDescriptorSet w[4]{};
    for (int i = 0; i < 4; ++i) {
        uint32_t t = tex[i] < r->textures.size() ? tex[i] : 0;
        info[i].sampler = r->tex_sampler;
        info[i].imageView = r->textures[t].view;
        info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = m.set; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(r->dev, 4, w, 0, nullptr);
    return true;
}

extern "C" {

dai_texture dai_render_texture_create(dai_renderer *r, const uint8_t *rgba, uint32_t w, uint32_t h, int srgb) {
    if (!r || !rgba || !w || !h) return 0;
    TextureEntry t{};
    t.width = w; t.height = h; t.mips = mip_levels(w, h);
    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = { w, h, 1 }; ii.mipLevels = t.mips; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->dev, &ii, nullptr, &t.image) != VK_SUCCESS)
        { std::snprintf(r->err, sizeof(r->err), "texture image failed"); return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(r->dev, t.image, &req);
    VkMemoryAllocateInfo ma{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = vk_find_mem(r->phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(r->dev, &ma, nullptr, &t.mem) != VK_SUCCESS) return 0;
    vkBindImageMemory(r->dev, t.image, t.mem, 0);

    GpuBuffer staging{};
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (!vk_make_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &staging, true)) return 0;
    std::memcpy(staging.mapped, rgba, (size_t)bytes);

    VkCommandBuffer cb;
    one_shot_begin(r, &cb);
    tex_barrier(cb, t.image, 0, t.mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(cb, staging.buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // build the mip chain by successive halving blits
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t i = 1; i < t.mips; ++i) {
        tex_barrier(cb, t.image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
        blit.srcOffsets[1] = { mw, mh, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[1] = { mw > 1 ? mw / 2 : 1, mh > 1 ? mh / 2 : 1, 1 };
        vkCmdBlitImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        tex_barrier(cb, t.image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (mw > 1) mw /= 2;
        if (mh > 1) mh /= 2;
    }
    tex_barrier(cb, t.image, t.mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    one_shot_end(r, cb);
    vk_free_buffer(r, &staging);

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips, 0, 1 };
    if (vkCreateImageView(r->dev, &vi, nullptr, &t.view) != VK_SUCCESS) return 0;

    r->textures.push_back(t);
    return (dai_texture)(r->textures.size() - 1);
}

dai_texture dai_render_texture_load(dai_renderer *r, const char *path, int srgb) {
    if (!r || !path) return 0;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    char err[200] = {0};
    if (!daiimg::read_png_file(path, px, &w, &h, err, sizeof(err))) {
        std::snprintf(r->err, sizeof(r->err), "%s", err);
        return 0;
    }
    return dai_render_texture_create(r, px.data(), w, h, srgb);
}

uint32_t dai_render_texture_count(dai_renderer *r) { return r ? (uint32_t)r->textures.size() : 0; }
uint32_t dai_render_material_count(dai_renderer *r) { return r ? (uint32_t)r->materials.size() : 0; }

dai_material_desc dai_material_desc_default(void) {
    dai_material_desc d{};
    d.base_color = { 1, 1, 1 };
    d.metallic = 0.0f;
    d.roughness = 1.0f;
    d.normal_strength = 1.0f;
    d.occlusion = 1.0f;
    d.uv_scale = 1.0f;
    return d;
}

dai_material dai_render_material_create(dai_renderer *r, const dai_material_desc *desc) {
    if (!r || !desc) return 0;
    MaterialEntry m{};
    m.p.base_color[0] = desc->base_color.x; m.p.base_color[1] = desc->base_color.y;
    m.p.base_color[2] = desc->base_color.z; m.p.base_color[3] = desc->alpha_cutoff;
    m.p.emissive[0] = desc->emissive.x; m.p.emissive[1] = desc->emissive.y;
    m.p.emissive[2] = desc->emissive.z; m.p.emissive[3] = (float)desc->flags;
    m.p.scalars[0] = desc->metallic;
    m.p.scalars[1] = desc->roughness <= 0.0f ? 1.0f : desc->roughness;
    m.p.scalars[2] = desc->normal_strength;
    m.p.scalars[3] = desc->uv_scale <= 0.0f ? 1.0f : desc->uv_scale;
    m.p.extra[0] = desc->occlusion;
    m.p.extra[1] = (desc->base_color_tex || desc->orm_tex || desc->normal_tex || desc->emissive_tex) ? 1.0f : 0.0f;
    m.p.extra[2] = desc->normal_tex ? 1.0f : 0.0f;
    m.p.extra[3] = 0.0f;
    m.base_tex = desc->base_color_tex;
    m.orm_tex = desc->orm_tex;
    m.normal_tex = desc->normal_tex;
    m.emissive_tex = desc->emissive_tex;
    if (desc->name) std::snprintf(m.name, sizeof(m.name), "%s", desc->name);
    if (!build_material_set(r, m)) {
        std::snprintf(r->err, sizeof(r->err), "out of material descriptor sets (max %u)", DAI_MAX_MATERIALS);
        return 0;
    }
    r->materials.push_back(m);
    return (dai_material)(r->materials.size() - 1);
}

} // extern "C"

// Called once during renderer creation: a 1x1 white texture, a flat normal
// map, and the default material - so every code path has something valid to
// bind and "no texture" never becomes a special case in the shader.
bool vk_init_default_material(dai_renderer *r) {
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t flat[4]  = { 128, 128, 255, 255 };
    // note: the first texture legitimately gets handle 0, so success is
    // checked through the registry, not through the returned handle
    dai_render_texture_create(r, white, 1, 1, 0);                       // 0: white
    dai_render_texture_create(r, flat, 1, 1, 0);                        // 1: flat normal
    if (r->textures.size() < 2) return false;
    dai_material_desc d = dai_material_desc_default();
    d.name = "default";
    MaterialEntry m{};
    // build material 0 by hand so it gets index 0 even if creation order changes
    m.p.base_color[0] = m.p.base_color[1] = m.p.base_color[2] = 1.0f;
    m.p.scalars[1] = 1.0f; m.p.scalars[2] = 1.0f; m.p.scalars[3] = 1.0f;
    m.p.extra[0] = 1.0f;
    std::snprintf(m.name, sizeof(m.name), "default");
    if (!build_material_set(r, m)) return false;
    r->materials.push_back(m);
    return true;
}
