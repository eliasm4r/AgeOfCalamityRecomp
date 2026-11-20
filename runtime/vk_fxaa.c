// FXAA on the 3D picture (ACR_FXAA, live; shaders/fxaa.frag), in the slot the game keeps for its own.
// Age of Calamity's frame ends its scene like Astral Chain's: after the tonemap writes the 8-bit output, one pass copies
// the output into a scratch target of the same size and the next draws it back, then the HUD is drawn over the output
// (measured with ACR_DUMP, frame 900 of the Hyrule Field battle: passes 198 and 199, both one full-screen draw of the
// game's copy shader). In Astral Chain the draw back was the game's FXAA; Age of Calamity ships with it switched off,
// so it is a plain copy. Here that draw back is replaced by our FXAA reading the scratch target: the 3D picture is
// anti-aliased, the HUD drawn afterwards stays sharp.
// The pair is recognised without shader hashes (they change with the translator): two consecutive passes, each with one
// sRGB 8-bit colour target of the same size (different images) and a full-screen first draw of at most six vertices;
// the copy out has no depth attached (the copy back keeps the output's). At most once a frame, and not in a frame DLAA
// or DLSS ran (they already anti-alias the scene).
#include "vk_fxaa.h"

#include <string.h>

#include "cfg.h"
#include "fxaa_spv.h"
#include "hdr_fullscreen_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"

static struct {
    int tried, ok, failed;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkSampler sampler;
    VkPipeline pipe;          // for these attachment formats
    VkFormat pipe_fmt[3];     // colour, depth, stencil (VK_FORMAT_UNDEFINED: none)
    // the last pass that could be the copy out
    uint64_t fence;           // its command buffer's timeline value
    uint32_t serial;          // its VkExec.pass_serial
    VkExecTarget src;         // its target
    int src_depthless;        // and it had no depth attached
    uint64_t frame_read, frame_done;
    int on;
    uint64_t runs;
} F = {.frame_read = ~0ull, .frame_done = ~0ull};

static int init(VkCtx *c) {
    F.tried = 1;
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (!F.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.sampler) != VK_SUCCESS)
        goto fail;
    return F.ok = 1;
fail:
    rt_log("fxaa: off (no layout or sampler)");
    return 0;
}

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// The pipeline for the pass's attachment formats (dynamic rendering needs them to match, the depth one too although
// FXAA does not use it; one set in practice: the game's RGBA8 sRGB output with its D32F_S8).
static int pipeline(VkCtx *c, const VkFormat fmt[3]) {
    if (F.pipe && !memcmp(F.pipe_fmt, fmt, sizeof F.pipe_fmt)) return 1;
    if (F.failed) return 0;
    if (F.pipe) {   // another format: earlier frames may still use the old pipeline
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vkDestroyPipeline(c->device, F.pipe, NULL);
        F.pipe = VK_NULL_HANDLE;
    }
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv), fs = module(c, fxaa_spv, sizeof fxaa_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    // the WITH_COUNT forms: the recorder thread routes their commands (vk_rec.h), not vkCmdSetViewport's
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt[0],
                                        .depthAttachmentFormat = fmt[1], .stencilAttachmentFormat = fmt[2]};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb,
                                       .pDynamicState = &ds, .layout = F.layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &F.pipe) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        F.pipe = VK_NULL_HANDLE;
        F.failed = 1;
        rt_log("fxaa: the pipeline could not be created (%d); FXAA is off", (int)r);
        return 0;
    }
    memcpy(F.pipe_fmt, fmt, sizeof F.pipe_fmt);
    return 1;
}

static int srgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

// A pass that can be either half of the pair: one sRGB 8-bit colour target (a whole 2D level, never in the feedback-loop
// layout, so the copy out is read in GENERAL), its first draw full-screen.
static int candidate(const VkExec *ex, int32_t count) {
    const VkExecTarget *t = &ex->color[0];
    return ex->pass_ncolor == 1 && ex->pass_samples <= 1 && !ex->fbl_mask && t->image && t->img &&
           t->img->image == t->image && !t->img->fb_learned && srgb8(t->format) && !t->is3d && !t->blocks &&
           t->layers == 1 && t->samples <= 1 && t->width == ex->pass_w && t->height == ex->pass_h && count >= 3 &&
           count <= 6;
}

int vk_fxaa_draw(VkExec *ex, int32_t count) {
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (frame != F.frame_read) F.frame_read = frame, F.on = cfg_bool("ACR_FXAA");   // once a frame: the menu switches it live
    if (!F.on) return 0;
    if (F.fence == ex->fence && F.serial == ex->pass_serial) return 0;   // a later draw of a pass already looked at
    if (!candidate(ex, count)) return 0;
    const VkExecTarget *t = &ex->color[0];
    int pair = F.fence == ex->fence && F.serial + 1 == ex->pass_serial && F.src_depthless && F.src.image != t->image &&
               F.src.width == t->width && F.src.height == t->height && F.src.format == t->format;
    VkExecTarget src = F.src;
    F.fence = ex->fence, F.serial = ex->pass_serial, F.src = *t, F.src_depthless = !ex->depth.image;
    if (!pair || frame == F.frame_done || vk_dlaa_ran()) return 0;
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    VkFormat fmt[3] = {ex->pass_color[0], ex->pass_depth, ex->pass_stencil};
    if (!F.ok || !pipeline(c, fmt)) return 0;
    VkImageView view = vk_exec_target_view(src.img, src.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                           src.level, src.base_layer, 1, 0);
    VkCommandBuffer cb = ex->cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.pipe);
    VkDescriptorImageInfo ii = {F.sampler, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout, 0, 1, &w);   // through the recorder thread (vk_rec.h)
    float rcp[2] = {1.0f / (float)src.width, 1.0f / (float)src.height};
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rcp, rcp);
    VkViewport v = {0, 0, (float)ex->pass_w, (float)ex->pass_h, 0, 1};
    VkRect2D s = {{0, 0}, {ex->pass_w, ex->pass_h}};
    vkCmdSetViewportWithCount(cb, 1, &v);
    vkCmdSetScissorWithCount(cb, 1, &s);
    vkCmdDraw(cb, 3, 1, 0, 0);
    ex->recorded += 6;
    ex->dirty |= VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();   // our pipeline, descriptors and dynamic state replaced the game's: the next draw binds again
    F.frame_done = frame;
    if (++F.runs == 1 || F.runs % 1800 == 0)
        rt_log("fxaa: %llu frames on the game's copy-back pass (%ux%u, format %d; frame %llu)", (unsigned long long)F.runs,
               t->width, t->height, (int)t->format, (unsigned long long)frame);
    return 1;
}
