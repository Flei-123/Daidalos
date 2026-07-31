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

struct Range { uint32_t mesh, material, first, count; };

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
    // sort by (casts shadow, material, mesh): shadow casters form a prefix the
    // depth pass can draw without touching materials, and inside that every
    // material/mesh pair becomes exactly one draw call
    auto key = [&](uint32_t i) {
        uint32_t m = inst[i].mesh < r->meshes.size() ? inst[i].mesh : (uint32_t)DAI_MESH_BOX;
        uint32_t mat = inst[i].material < r->materials.size() ? inst[i].material : 0u;
        uint64_t caster = (inst[i].flags & DAI_RI_NO_SHADOW) ? 1u : 0u;
        return (caster << 40) | ((uint64_t)mat << 20) | m;
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
        if (v.material >= r->materials.size()) v.material = 0;
        if (!(v.flags & DAI_RI_NO_SHADOW)) casters = i + 1;
        if (!ranges.empty() && ranges.back().mesh == v.mesh && ranges.back().material == v.material)
            ranges.back().count++;
        else ranges.push_back({ v.mesh, v.material, i, 1 });
    }

    // ---- 2. uniforms
    float aspect = (float)r->width / (float)r->height;
    Mat4 view = mat_look_at(r->eye, r->target, r->up);
    Mat4 proj = mat_perspective(r->fov, aspect, r->znear, r->zfar);
    Mat4 viewproj = mat_mul(proj, view);

    // ---- cascaded shadow maps
    //
    // One shadow map over the whole view distance is either blurry up close or
    // useless far away. Split the view range into N slices, fit a light space
    // box around each slice's bounding sphere, and let the fragment stage pick
    // the tightest one that covers it. The sphere (rather than the frustum
    // corners) is what keeps the box size constant as the camera turns, so the
    // shadow edges do not shimmer.
    float R = r->shadow_radius;
    float splits[DAI_SHADOW_CASCADES + 1];
    {
        float near_d = 0.5f, far_d = R * 2.0f;
        splits[0] = near_d;
        for (uint32_t i = 1; i <= r->cascades; ++i) {
            float t = (float)i / (float)r->cascades;
            float log_split = near_d * powf(far_d / near_d, t);
            float lin_split = near_d + (far_d - near_d) * t;
            splits[i] = 0.72f * log_split + 0.28f * lin_split;   // practical split scheme
        }
    }

    Mat4 lightvp[DAI_SHADOW_CASCADES];
    float fwd[3] = { r->target[0] - r->eye[0], r->target[1] - r->eye[1], r->target[2] - r->eye[2] };
    {
        float l = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        if (l > 1e-6f) { fwd[0]/=l; fwd[1]/=l; fwd[2]/=l; }
    }
    float lup[3] = { 0, 1, 0 };
    if (fabsf(r->sun_dir[1]) > 0.98f) { lup[1] = 0; lup[2] = 1; }

    for (uint32_t c = 0; c < r->cascades; ++c) {
        float n = splits[c], f = splits[c + 1];
        // bounding sphere of the slice, centred on the view axis
        float mid = (n + f) * 0.5f;
        float half_fov = r->fov * 3.14159265f / 360.0f;
        float t = tanf(half_fov);
        float aspect_r = (float)r->width / (float)r->height;
        float rn = n * t * sqrtf(1.0f + aspect_r * aspect_r);
        float rf = f * t * sqrtf(1.0f + aspect_r * aspect_r);
        float radius = sqrtf(fmaxf(rn * rn + n * n, rf * rf + f * f));
        // centre the sphere so it covers both slice caps
        float centre_d = mid + (rf * rf - rn * rn) / (4.0f * fmaxf(mid - n, 1e-3f));
        if (centre_d < n) centre_d = mid;
        float cx = r->eye[0] + fwd[0] * centre_d;
        float cy = r->eye[1] + fwd[1] * centre_d;
        float cz = r->eye[2] + fwd[2] * centre_d;
        radius = fmaxf(radius, 1.0f);

        float ctr[3] = { cx, cy, cz };
        float lp[3] = { cx + r->sun_dir[0] * radius * 2.5f,
                        cy + r->sun_dir[1] * radius * 2.5f,
                        cz + r->sun_dir[2] * radius * 2.5f };
        Mat4 lview = mat_look_at(lp, ctr, lup);
        // snap the light space origin to whole texels: without this the shadow
        // edges crawl every time the camera moves a fraction of a texel
        float texels = (float)r->shadow_size / (2.0f * radius);
        float ox = lview.m[12] * texels, oy = lview.m[13] * texels;
        lview.m[12] = floorf(ox) / texels;
        lview.m[13] = floorf(oy) / texels;
        Mat4 lproj = mat_ortho(-radius, radius, -radius, radius, 0.05f, radius * 6.0f);
        lightvp[c] = mat_mul(lproj, lview);
    }

    FrameUBO u{};
    u.viewproj = viewproj;
    if (!mat_invert(viewproj, &u.invviewproj)) u.invviewproj = mat_identity();
    for (uint32_t c = 0; c < DAI_SHADOW_CASCADES; ++c)
        u.lightviewproj[c] = lightvp[c < r->cascades ? c : r->cascades - 1];
    for (uint32_t c = 0; c < 4; ++c)
        u.cascade_split[c] = splits[c + 1 <= r->cascades ? c + 1 : r->cascades];
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
    // billboard basis: the view matrix rows are the camera axes in world space
    u.cam_right[0] = view.m[0]; u.cam_right[1] = view.m[4]; u.cam_right[2] = view.m[8];
    u.cam_up[0] = view.m[1];    u.cam_up[1] = view.m[5];    u.cam_up[2] = view.m[9];
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

    // --- shadow passes: one per cascade (always runs, so the image ends up in
    //     the layout the descriptor set expects even with nothing to draw)
    // NOTE: DEPTH_READ_ONLY -> DEPTH_ATTACHMENT (not UNDEFINED): the layers we
    // skip this frame must keep their contents
    vk_barrier(r->cmd, r->shadow_img, VK_IMAGE_ASPECT_DEPTH_BIT,
               r->shadow_valid ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               r->cascades);
    // Cascade caching: the distant cascades cover tens of metres and their
    // light matrix is texel snapped, so it only changes when the camera has
    // actually moved far enough. Re-rendering an identical cascade is pure
    // waste - and in a fixed camera shot it is ALL of the shadow cost.
    // A cascade may only be reused if NOTHING that casts into it moved. Hashing
    // the caster instances is ~10 us for a thousand of them and is the
    // difference between a fast shadow and a wrong one.
    uint64_t caster_hash = 1469598103934665603ULL;
    {
        const uint8_t *bytes = (const uint8_t *)dst;
        size_t n = (size_t)casters * sizeof(dai_render_instance);
        for (size_t i = 0; i < n; ++i) { caster_hash ^= bytes[i]; caster_hash *= 1099511628211ULL; }
    }

    bool redraw[DAI_SHADOW_CASCADES];
    for (uint32_t c = 0; c < r->cascades; ++c) {
        bool same = r->shadow_valid && casters == r->last_casters && caster_hash == r->last_caster_hash;
        if (same)
            for (int i = 0; i < 16 && same; ++i)
                if (fabsf(lightvp[c].m[i] - r->last_lightvp[c].m[i]) > 1e-6f) same = false;
        redraw[c] = !same;
        r->last_lightvp[c] = lightvp[c];
    }
    r->last_casters = casters;
    r->last_caster_hash = caster_hash;
    uint32_t cascades_drawn = 0;
    for (uint32_t c = 0; c < r->cascades; ++c) {
        if (!redraw[c]) continue;
        ++cascades_drawn;
        uint32_t ss = r->shadows ? r->shadow_size : 1;
        VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = r->shadow_layer[c]; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { ss, ss } };
        ri.layerCount = 1; ri.pDepthAttachment = &da;
        vkCmdBeginRendering(r->cmd, &ri);
        if (r->shadows && casters) {
            vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_shadow);
            vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->dset, 0, nullptr);
            vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 1, 1,
                                    &r->materials[0].set, 0, nullptr);
            // which cascade this pass writes travels in the push constant slot
            // the shadow vertex stage reads as "extra.w"
            MaterialPush pc = r->materials[0].p;
            pc.extra[3] = (float)c;
            vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(MaterialPush), &pc);
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
    r->shadow_valid = true;
    vk_barrier(r->cmd, r->shadow_img, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               r->cascades);

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
        uint32_t bound_material = 0xFFFFFFFFu;
        for (const Range &g : ranges) {
            if (g.material != bound_material) {
                const MaterialEntry &mat = r->materials[g.material];
                vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 1, 1, &mat.set, 0, nullptr);
                vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(MaterialPush), &mat.p);
                bound_material = g.material;
            }
            const MeshEntry &me = r->meshes[g.mesh];
            vkCmdDrawIndexed(r->cmd, me.index_count, g.count, me.first_index, me.vertex_offset, g.first);
        }
    }
    // particles last: they are transparent, depth tested against the opaque
    // scene, and must not write depth
    if (r->particle_count && r->pipe_particle) {
        vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_particle);
        const MaterialEntry &pm = r->materials[r->particle_material < r->materials.size() ? r->particle_material : 0];
        vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 1, 1, &pm.set, 0, nullptr);
        MaterialPush pc{};
        pc.base_color[0] = r->particle_atlas[0];   // atlas columns
        pc.base_color[1] = r->particle_atlas[1];   // atlas rows
        pc.base_color[2] = r->particle_atlas[2];   // 1 = sample the atlas
        vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(MaterialPush), &pc);
        VkDeviceSize poff = 0;
        vkCmdBindVertexBuffers(r->cmd, 0, 1, &r->particles.buf, &poff);
        vkCmdDraw(r->cmd, 6, r->particle_count, 0, 0);
    }
    // UI last of all: screen space, no depth, one batch per texture
    if (r->ui_vertex_count && r->pipe_ui) {
        vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_ui);
        MaterialPush pc{};
        pc.base_color[0] = (float)r->width;
        pc.base_color[1] = (float)r->height;
        vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(MaterialPush), &pc);
        VkDeviceSize uoff = 0;
        vkCmdBindVertexBuffers(r->cmd, 0, 1, &r->ui_verts.buf, &uoff);
        uint32_t first = 0;
        for (size_t i = 0; i < r->ui_batch_counts.size(); ++i) {
            uint32_t tex = r->ui_batch_textures[i];
            // the UI names a TEXTURE; the pipeline needs a descriptor set, so
            // materials double as texture bindings here
            uint32_t mat = 0;
            for (uint32_t m = 0; m < r->materials.size(); ++m)
                if (r->materials[m].base_tex == tex) { mat = m; break; }
            vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 1, 1,
                                    &r->materials[mat].set, 0, nullptr);
            vkCmdDraw(r->cmd, r->ui_batch_counts[i], 1, first, 0);
            first += r->ui_batch_counts[i];
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
    r->last_draws = (uint32_t)ranges.size() + cascades_drawn;
    r->have_frame = true;
    return DAI_OK;
}
