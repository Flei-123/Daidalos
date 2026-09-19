// The post processing chain: bloom, vignette, grain, chromatic aberration,
// scanlines, flash.
//
// A third translation unit rather than more of rhi_vulkan_frame.cpp, for the
// same reason that file was split off in the first place: the frame recorder
// is already the longest function in the backend, and a chain of four passes
// with its own images, pipelines and descriptor sets is a self contained
// subsystem that is easier to read - and to switch off - when it lives alone.
//
// The one structural rule here: with dai_postfx.enabled == 0 NOTHING in this
// file executes and no image it owns is touched, so the frame that reaches the
// readback buffer is the same frame, bit for bit, that reached it before this
// file existed. tests/test_postfx.cpp [1] holds that claim.

#include "rhi_vulkan.hpp"

#include <cstring>

namespace {

// The bloom runs at a QUARTER of the frame's width and height - an sixteenth
// of the pixels. A glow needs a blur radius of tens of pixels to read as light
// rather than as a soft edge, and a Gaussian that wide at full resolution
// needs either a huge kernel or many passes. Downsampling first is the
// standard trade and it costs nothing visible, because the thing being blurred
// has no detail left to lose. Measured in docs/POSTFX.md.
const uint32_t kBloomDiv = 4;

// Push constants for the post passes. Same 96 bytes as MaterialPush so the two
// pipeline layouts can declare an identical range and no pipeline has to be
// rebuilt when one of them changes; the shaders name the fields themselves.
struct PostPush {
    float p0[4];
    float p1[4];
    float p2[4];
    float p3[4];
    float p4[4];
    float p5[4];
};
static_assert(sizeof(PostPush) == sizeof(MaterialPush),
              "post and material pushes share a range size on purpose");

// A set pointing at one image, for a post pass to sample. Written once and
// rewritten on resize - see the note on set_color in the struct.
void write_set(dai_renderer *r, VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo info{};
    info.sampler = r->post_sampler;
    info.imageView = view;
    // Every post source is sampled, so it is always in this layout by the time
    // the set is used; the barriers in vk_post_record put it there.
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(r->dev, 1, &w, 0, nullptr);
}

// One fullscreen pass: begin rendering into `view`, bind, draw three vertices,
// end. Every pass in this file is exactly this shape, so it is written once -
// four copies of eleven lines of VkRenderingInfo is where a wrong extent hides.
void full_pass(dai_renderer *r, VkImageView dst, uint32_t w, uint32_t h,
               VkPipeline pipe, const VkDescriptorSet *sets, uint32_t set_count,
               const PostPush &push) {
    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = dst;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // DONT_CARE, not CLEAR: the pass writes every pixel of its target anyway,
    // and clearing first would be a second full write of the image.
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { w, h } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    // No depth attachment anywhere in the chain: these are screen space passes
    // over a triangle that covers everything, and the pipelines below disable
    // the depth test to match.
    vkCmdBeginRendering(r->cmd, &ri);

    VkViewport vp{ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc{ { 0, 0 }, { w, h } };
    vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vkCmdSetScissor(r->cmd, 0, 1, &sc);

    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    // Set 0 is the frame UBO's layout, which the post shaders do not use but
    // the layout still declares, so the sampled images start at set 1 and the
    // sets bound here go to index 1 upwards.
    vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->post_layout,
                            1, set_count, sets, 0, nullptr);
    vkCmdPushConstants(r->cmd, r->post_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PostPush), &push);
    vkCmdDraw(r->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(r->cmd);
}

}  // namespace

bool vk_post_init(dai_renderer *r) {
    // CLAMP_TO_EDGE, and the reason is written on post_sampler in the header:
    // a repeating sampler makes the blur wrap light around the screen.
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(r->dev, &si, nullptr, &r->post_sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding lb{};
    lb.binding = 0;
    lb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lb.descriptorCount = 1;
    lb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 1; dlc.pBindings = &lb;
    if (vkCreateDescriptorSetLayout(r->dev, &dlc, nullptr, &r->post_dsl) != VK_SUCCESS) return false;

    // Three sets, one per image that is ever read: color_rt, bloom_a, bloom_b.
    // post_rt is only ever written, so it needs none.
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
    VkDescriptorPoolCreateInfo dpc{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = 3; dpc.poolSizeCount = 1; dpc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(r->dev, &dpc, nullptr, &r->post_pool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[3] = { r->post_dsl, r->post_dsl, r->post_dsl };
    VkDescriptorSet sets[3] = {};
    VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsa.descriptorPool = r->post_pool; dsa.descriptorSetCount = 3; dsa.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(r->dev, &dsa, sets) != VK_SUCCESS) return false;
    r->set_color = sets[0]; r->set_bloom_a = sets[1]; r->set_bloom_b = sets[2];

    // The layout declares THREE sets: the frame UBO's layout at 0 (unused by
    // these shaders but kept so one pipeline layout could serve both if it
    // ever needs to), the scene/source at 1, and the bloom at 2. The composite
    // is the only pass that binds two images, and a layout is allowed to
    // declare more sets than a given pipeline reads.
    VkDescriptorSetLayout sl[3] = { r->dsl, r->post_dsl, r->post_dsl };
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PostPush);
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 3; plci.pSetLayouts = sl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(r->dev, &plci, nullptr, &r->post_layout) != VK_SUCCESS) return false;

    bool ok = true;
    VkShaderModule vs = load_module(r, "post.vert.spv", &ok);
    VkShaderModule fs_bright = load_module(r, "post_bright.frag.spv", &ok);
    VkShaderModule fs_blur = load_module(r, "post_blur.frag.spv", &ok);
    VkShaderModule fs_comp = load_module(r, "post_comp.frag.spv", &ok);
    if (!ok) {
        // A renderer without post processing is still a working renderer, so a
        // missing post shader must not take the whole device down with it -
        // unlike the mesh shader, which has nothing to fall back to. The chain
        // simply never becomes ready and every frame stays as it was.
        for (VkShaderModule m : { vs, fs_bright, fs_blur, fs_comp })
            if (m) vkDestroyShaderModule(r->dev, m, nullptr);
        return false;
    }

    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        VkPipelineShaderStageCreateInfo i{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        i.stage = s; i.module = m; i.pName = "main";
        return i;
    };

    // No vertex input at all - post.vert builds its triangle from
    // gl_VertexIndex, exactly as sky.vert does.
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
    // Dynamic viewport and scissor are what lets ONE blur pipeline run at the
    // bloom's size and the composite at the frame's size without a second
    // pipeline object per resolution.
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // NONE: the fullscreen triangle's winding is whatever gl_VertexIndex makes
    // it, and a culled post pass is an invisible black screen.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // Always 1 sample. The post chain runs on the RESOLVED image - color_rt,
    // not color_ms - so it never sees MSAA, and a pipeline declaring the
    // renderer's sample count would not match the attachment it draws into.
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
    // No blending: every post pass REPLACES its target. The bloom is added
    // inside the composite shader, where it can be weighted, rather than by
    // the blend unit, which cannot.
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 1; prc.pColorAttachmentFormats = &fmt;
    // Explicitly UNDEFINED: dynamic rendering matches a pipeline against the
    // attachments it is used with, and claiming a depth format the pass does
    // not attach is a validation error.
    prc.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &prc;
    gp.stageCount = 2;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dys; gp.layout = r->post_layout;

    VkPipelineShaderStageCreateInfo st_bright[2] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                                     stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs_bright) };
    VkPipelineShaderStageCreateInfo st_blur[2] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                                   stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs_blur) };
    VkPipelineShaderStageCreateInfo st_comp[2] = { stage(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                                   stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs_comp) };
    VkGraphicsPipelineCreateInfo gb = gp; gb.pStages = st_bright;
    VkGraphicsPipelineCreateInfo gl = gp; gl.pStages = st_blur;
    VkGraphicsPipelineCreateInfo gc = gp; gc.pStages = st_comp;

    VkResult a = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gb, nullptr, &r->pipe_post_bright);
    VkResult b = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gl, nullptr, &r->pipe_post_blur);
    VkResult c = vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gc, nullptr, &r->pipe_post_comp);

    for (VkShaderModule m : { vs, fs_bright, fs_blur, fs_comp })
        vkDestroyShaderModule(r->dev, m, nullptr);

    return a == VK_SUCCESS && b == VK_SUCCESS && c == VK_SUCCESS;
}

bool vk_post_make_targets(dai_renderer *r) {
    // Integer division rounds a 1279 px frame's bloom down to 319; the +div-1
    // keeps at least one texel and stops a very small window (a torn off panel
    // one pixel wide) from asking for a zero sized image, which fails
    // creation outright.
    r->bloom_w = (r->width + kBloomDiv - 1) / kBloomDiv;
    r->bloom_h = (r->height + kBloomDiv - 1) / kBloomDiv;
    if (r->bloom_w < 1) r->bloom_w = 1;
    if (r->bloom_h < 1) r->bloom_h = 1;

    // post_rt needs TRANSFER_SRC as well as COLOR_ATTACHMENT, because when the
    // chain is on this is the image the readback copies out of.
    if (!make_image(r, r->width, r->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->post_rt, &r->post_rt_mem, &r->post_rt_view))
        return false;
    if (!make_image(r, r->bloom_w, r->bloom_h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->bloom_a, &r->bloom_a_mem, &r->bloom_a_view))
        return false;
    if (!make_image(r, r->bloom_w, r->bloom_h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->bloom_b, &r->bloom_b_mem, &r->bloom_b_view))
        return false;

    // The sets are allocated once and REWRITTEN here. color_rt is recreated by
    // every resize too, so its set has to be rewritten with the rest or the
    // composite samples an image that was freed - the resize bug this function
    // exists for.
    write_set(r, r->set_color, r->color_rt_view);
    write_set(r, r->set_bloom_a, r->bloom_a_view);
    write_set(r, r->set_bloom_b, r->bloom_b_view);
    return true;
}

void vk_post_free_targets(dai_renderer *r) {
    for (auto v : { r->post_rt_view, r->bloom_a_view, r->bloom_b_view })
        if (v) vkDestroyImageView(r->dev, v, nullptr);
    r->post_rt_view = r->bloom_a_view = r->bloom_b_view = VK_NULL_HANDLE;
    if (r->post_rt) { vkDestroyImage(r->dev, r->post_rt, nullptr); vkFreeMemory(r->dev, r->post_rt_mem, nullptr); }
    if (r->bloom_a) { vkDestroyImage(r->dev, r->bloom_a, nullptr); vkFreeMemory(r->dev, r->bloom_a_mem, nullptr); }
    if (r->bloom_b) { vkDestroyImage(r->dev, r->bloom_b, nullptr); vkFreeMemory(r->dev, r->bloom_b_mem, nullptr); }
    r->post_rt = r->bloom_a = r->bloom_b = VK_NULL_HANDLE;
    r->post_rt_mem = r->bloom_a_mem = r->bloom_b_mem = VK_NULL_HANDLE;
}

void vk_post_destroy(dai_renderer *r) {
    vk_post_free_targets(r);
    if (r->pipe_post_bright) vkDestroyPipeline(r->dev, r->pipe_post_bright, nullptr);
    if (r->pipe_post_blur) vkDestroyPipeline(r->dev, r->pipe_post_blur, nullptr);
    if (r->pipe_post_comp) vkDestroyPipeline(r->dev, r->pipe_post_comp, nullptr);
    r->pipe_post_bright = r->pipe_post_blur = r->pipe_post_comp = VK_NULL_HANDLE;
    if (r->post_layout) vkDestroyPipelineLayout(r->dev, r->post_layout, nullptr);
    r->post_layout = VK_NULL_HANDLE;
    // The pool owns the three sets; freeing it frees them, and freeing a set
    // from a pool that is about to be destroyed is the classic double free.
    if (r->post_pool) vkDestroyDescriptorPool(r->dev, r->post_pool, nullptr);
    r->post_pool = VK_NULL_HANDLE;
    r->set_color = r->set_bloom_a = r->set_bloom_b = VK_NULL_HANDLE;
    if (r->post_dsl) vkDestroyDescriptorSetLayout(r->dev, r->post_dsl, nullptr);
    r->post_dsl = VK_NULL_HANDLE;
    if (r->post_sampler) vkDestroySampler(r->dev, r->post_sampler, nullptr);
    r->post_sampler = VK_NULL_HANDLE;
    r->post_ready = false;
}

VkImage vk_post_record(dai_renderer *r) {
    const dai_postfx &fx = r->postfx;
    // The one early out that the whole "off by default" contract rests on: the
    // frame goes on being read back out of color_rt, and not a single command
    // from this file enters the buffer.
    r->post_used = false;
    if (!fx.enabled || !r->post_ready) return r->color_rt;
    r->post_used = true;

    // Bloom is skipped when it would contribute nothing. Three passes over a
    // quarter of the frame is the most expensive part of the chain, and a host
    // that wants only grain and a vignette should not pay for them.
    const bool want_bloom = fx.bloom_intensity > 0.0f;

    // The scene has just been written as a colour attachment; every path below
    // SAMPLES it, so it has to change layout once, here, rather than once per
    // pass.
    vk_barrier(r->cmd, r->color_rt, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    if (want_bloom) {
        // ---- bright pass: color_rt -> bloom_a, at a quarter of the size ----
        vk_barrier(r->cmd, r->bloom_a, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        PostPush p{};
        p.p0[0] = fx.bloom_threshold;
        p.p0[1] = fx.bloom_knee;
        p.p0[2] = fx.bloom_intensity;
        // Half a SOURCE texel: the four taps in the bright pass straddle the
        // source pixel centres that the destination texel covers. Derived from
        // the frame's size, not the bloom's, because that is what it samples.
        p.p1[0] = 0.5f / (float)r->width;
        p.p1[1] = 0.5f / (float)r->height;
        full_pass(r, r->bloom_a_view, r->bloom_w, r->bloom_h, r->pipe_post_bright,
                  &r->set_color, 1, p);

        // ---- separable Gaussian, two full iterations -----------------------
        // Two H+V rounds rather than one: a single sigma-2 pass on a quarter
        // resolution image spreads light about 8 full resolution pixels, which
        // is a soft edge, not a glow. Running the pair twice widens it to
        // roughly 24 and is what makes neon actually radiate. Measured in
        // tests/test_postfx.cpp [2] as the width of a bright spot.
        for (int it = 0; it < 2; ++it) {
            // horizontal: bloom_a -> bloom_b
            vk_barrier(r->cmd, r->bloom_a, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            vk_barrier(r->cmd, r->bloom_b, VK_IMAGE_ASPECT_COLOR_BIT,
                       it == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            PostPush h{};
            h.p0[0] = 1.0f / (float)r->bloom_w;
            h.p0[1] = 0.0f;
            full_pass(r, r->bloom_b_view, r->bloom_w, r->bloom_h, r->pipe_post_blur,
                      &r->set_bloom_a, 1, h);

            // vertical: bloom_b -> bloom_a, so the chain ends in bloom_a every
            // time and the composite always knows which image to read.
            vk_barrier(r->cmd, r->bloom_b, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            vk_barrier(r->cmd, r->bloom_a, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            PostPush v{};
            v.p0[0] = 0.0f;
            v.p0[1] = 1.0f / (float)r->bloom_h;
            full_pass(r, r->bloom_a_view, r->bloom_w, r->bloom_h, r->pipe_post_blur,
                      &r->set_bloom_b, 1, v);
        }

        vk_barrier(r->cmd, r->bloom_a, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else {
        // The composite's layout binds set 2 whether or not it reads it, and a
        // descriptor pointing at an image in the wrong layout is invalid even
        // when the shader never samples it. Put bloom_a somewhere legal.
        vk_barrier(r->cmd, r->bloom_a, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // ---- composite: color_rt + bloom -> post_rt ---------------------------
    vk_barrier(r->cmd, r->post_rt, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    PostPush c{};
    c.p0[0] = fx.bloom_intensity;
    c.p0[1] = fx.vignette;
    c.p0[2] = fx.grain;
    c.p0[3] = fx.aberration;
    c.p1[0] = fx.scanlines;
    // The counter goes to the shader as a float, so it is reduced modulo 4096
    // first: a float32 stops being able to represent consecutive integers at
    // 2^24, and a session left running would eventually freeze the grain on
    // one pattern. 4096 distinct patterns is more than the eye can follow and
    // keeps every value exact.
    c.p1[1] = (float)(fx.frame_index & 0xFFFu);
    c.p1[2] = want_bloom ? 1.0f : 0.0f;
    c.p2[0] = fx.flash_color[0];
    c.p2[1] = fx.flash_color[1];
    c.p2[2] = fx.flash_color[2];
    c.p2[3] = fx.flash_amount;
    c.p3[0] = 1.0f / (float)r->width;
    c.p3[1] = 1.0f / (float)r->height;

    VkDescriptorSet comp_sets[2] = { r->set_color, r->set_bloom_a };
    full_pass(r, r->post_rt_view, r->width, r->height, r->pipe_post_comp, comp_sets, 2, c);

    vk_barrier(r->cmd, r->post_rt, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    // color_rt is deliberately LEFT in SHADER_READ_ONLY. The next frame
    // re-acquires it from VK_IMAGE_LAYOUT_UNDEFINED (rhi_vulkan_frame.cpp, the
    // barrier before the main pass), which discards the contents - exactly
    // what a frame that clears its colour target wants - so transitioning it
    // back here would be a second barrier for a layout nobody reads.

    return r->post_rt;
}

VkImage vk_present_image(dai_renderer *r) {
    // post_used is set by the frame that ran, not derived from postfx.enabled
    // here: a host may switch the chain off AFTER a frame was drawn with it
    // on, and the picture waiting to be presented is still the processed one.
    return (r->post_used && r->post_rt) ? r->post_rt : r->color_rt;
}

extern "C" void dai_render_postfx(dai_renderer *r, const dai_postfx *fx) {
    if (!r) return;
    // NULL means off, which is the same state a zeroed struct describes - a
    // host tearing down an effect should not have to remember which fields to
    // clear.
    if (!fx) { r->postfx = dai_postfx{}; return; }
    r->postfx = *fx;
}
