// Daidalos - Vulkan 1.3 backend, part 2: the frame.
//
//   1. sort instances by (casts shadow, mesh) so every mesh is one draw call
//   2. depth only pass from the sun -> shadow map
//   3. sky, then the meshes, into the multisampled target
//   4. resolve and copy to the readback buffer
//
// Everything is submitted and waited on inside this call: the renderer is a
// screenshot machine first and a real time loop second, which is what makes
// it testable.

#include "rhi_vulkan.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {

bool ensure_instances(dai_renderer *r, uint32_t count) {
    if (count <= r->inst_capacity) return true;
    uint32_t cap = r->inst_capacity ? r->inst_capacity : 256;
    while (cap < count) cap *= 2;
    GpuBuffer nb{};
    if (!vk_make_buffer(r, (VkDeviceSize)cap * sizeof(dai_render_instance),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &nb, true)) return false;
    vkDeviceWaitIdle(r->dev);
    vk_free_buffer(r, &r->inst);
    r->inst = nb;
    r->inst_capacity = cap;
    return true;
}

struct Range { uint32_t mesh, first, count; };

void set_vp(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkViewport vp{ 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc{ { 0, 0 }, { w, h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

} // namespace

extern "C" dai_result dai_render_frame(dai_renderer *r, const dai_render_instance *inst, uint32_t count) {
    if (!r) return DAI_ERR_INVALID_ARG;
    if (count && !inst) return DAI_ERR_INVALID_ARG;
    if (!ensure_instances(r, count ? count : 1)) return DAI_ERR_OUT_OF_MEMORY;
    auto t0 = std::chrono::high_resolution_clock::now();

    // ---- 1. sort: shadow casters first, then by mesh
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    auto key = [&](uint32_t i) {
        uint32_t m = inst[i].mesh < r->meshes.size() ? inst[i].mesh : (uint32_t)DAI_MESH_BOX;
        uint32_t caster = (inst[i].flags & DAI_RI_NO_SHADOW) ? 1u : 0u;
        return (uint64_t)caster << 32 | m;
    };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });

    dai_render_instance *dst = (dai_render_instance *)r->inst.mapped;
    uint32_t casters = 0;
    std::vector<Range> ranges;
    for (uint32_t i = 0; i < count; ++i) {
        dai_render_instance v = inst[order[i]];
        if (v.mesh >= r->meshes.size()) v.mesh = DAI_MESH_BOX;
        if (v.roughness <= 0.0f) v.roughness = 1.0f;
        dst[i] = v;
        if (!(v.flags & DAI_RI_NO_SHADOW)) casters = i + 1;
        if (!ranges.empty() && ranges.back().mesh == v.mesh) ranges.back().count++;
        else ranges.push_back({ v.mesh, i, 1 });
    }

    // ---- 2. uniforms
    float aspect = (float)r->width / (float)r->height;
    Mat4 view = mat_look_at(r->eye, r->target, r->up);
    Mat4 proj = mat_perspective(r->fov, aspect, r->znear, r->zfar);
    Mat4 viewproj = mat_mul(proj, view);

    float R = r->shadow_radius;
    float lp[3] = { r->target[0] + r->sun_dir[0] * R * 2.0f,
                    r->target[1] + r->sun_dir[1] * R * 2.0f,
                    r->target[2] + r->sun_dir[2] * R * 2.0f };
    float lup[3] = { 0, 1, 0 };
    if (fabsf(r->sun_dir[1]) > 0.98f) { lup[1] = 0; lup[2] = 1; }
    Mat4 lview = mat_look_at(lp, r->target, lup);
    Mat4 lproj = mat_ortho(-R, R, -R, R, 0.05f, R * 4.5f);
    Mat4 lightvp = mat_mul(lproj, lview);

    FrameUBO u{};
    u.viewproj = viewproj;
    if (!mat_invert(viewproj, &u.invviewproj)) u.invviewproj = mat_identity();
    u.lightviewproj = lightvp;
    for (int i = 0; i < 3; ++i) {
        u.sun_dir[i] = r->sun_dir[i];
        u.sun_color[i] = r->sun_color[i];
        u.sky_color[i] = r->sky_color[i];
        u.ground_color[i] = r->ground_color[i];
        u.fog_color[i] = r->fog_color[i];
        u.cam_pos[i] = r->eye[i];
    }
    u.sun_dir[3] = r->sun_intensity;
    u.sun_color[3] = 1.0f / (float)r->shadow_size;
    u.sky_color[3] = r->ambient;
    u.ground_color[3] = r->fog_density;
    u.fog_color[3] = r->exposure;
    u.cam_pos[3] = (r->shadows && casters) ? 1.0f : 0.0f;
    if (u.cam_pos[3] > 0.0f && getenv("DAI_DEBUG_SHADOW")) u.cam_pos[3] = 2.0f;
    std::memcpy(r->ubo.mapped, &u, sizeof(u));

    // ---- 3. record
    vkResetFences(r->dev, 1, &r->fence);
    vkResetCommandBuffer(r->cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(r->cmd, &bi);

    VkDeviceSize off[2] = { 0, 0 };
    VkBuffer bufs[2] = { r->vbo.buf, r->inst.buf };

    // --- shadow pass (always runs: it also puts the image in a layout the
    //     descriptor set expects, even when there is nothing to draw)
    vk_barrier(r->cmd, r->shadow_img, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    {
        uint32_t ss = r->shadows ? r->shadow_size : 1;
        VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = r->shadow_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { ss, ss } };
        ri.layerCount = 1; ri.pDepthAttachment = &da;
        vkCmdBeginRendering(r->cmd, &ri);
        if (r->shadows && casters) {
            vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_shadow);
            vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->dset, 0, nullptr);
            set_vp(r->cmd, ss, ss);
            vkCmdBindVertexBuffers(r->cmd, 0, 2, bufs, off);
            vkCmdBindIndexBuffer(r->cmd, r->ibo.buf, 0, VK_INDEX_TYPE_UINT32);
            for (const Range &g : ranges) {
                if (g.first >= casters) break;
                uint32_t n = std::min(g.count, casters - g.first);
                const MeshEntry &me = r->meshes[g.mesh];
                vkCmdDrawIndexed(r->cmd, me.index_count, n, me.first_index, me.vertex_offset, g.first);
            }
        }
        vkCmdEndRendering(r->cmd);
    }
    vk_barrier(r->cmd, r->shadow_img, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // --- main pass
    bool msaa = r->samples != VK_SAMPLE_COUNT_1_BIT;
    if (msaa)
        vk_barrier(r->cmd, r->color_ms, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vk_barrier(r->cmd, r->color_rt, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vk_barrier(r->cmd, r->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = msaa ? r->color_ms_view : r->color_rt_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = { { r->clear[0], r->clear[1], r->clear[2], 1.0f } };
    if (msaa) {
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = r->color_rt_view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { r->width, r->height } };
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(r->cmd, &ri);
    set_vp(r->cmd, r->width, r->height);
    vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->dset, 0, nullptr);

    if (r->sky_enabled) {
        vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_sky);
        vkCmdDraw(r->cmd, 3, 1, 0, 0);
    }
    if (count) {
        vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_mesh);
        vkCmdBindVertexBuffers(r->cmd, 0, 2, bufs, off);
        vkCmdBindIndexBuffer(r->cmd, r->ibo.buf, 0, VK_INDEX_TYPE_UINT32);
        for (const Range &g : ranges) {
            const MeshEntry &me = r->meshes[g.mesh];
            vkCmdDrawIndexed(r->cmd, me.index_count, g.count, me.first_index, me.vertex_offset, g.first);
        }
    }
    vkCmdEndRendering(r->cmd);

    vk_barrier(r->cmd, r->color_rt, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { r->width, r->height, 1 };
    vkCmdCopyImageToBuffer(r->cmd, r->color_rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->readback.buf, 1, &region);

    vkEndCommandBuffer(r->cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &r->cmd;
    if (vkQueueSubmit(r->queue, 1, &si, r->fence) != VK_SUCCESS) return DAI_ERR_STATE;
    vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, UINT64_MAX);

    r->last_ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    r->last_draws = (uint32_t)ranges.size();
    r->have_frame = true;
    return DAI_OK;
}
