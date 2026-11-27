// FXAA on the 3D picture (ACR_FXAA, live; shaders/fxaa.frag), where the game keeps the slot of its own.
// Age of Calamity's frame ends its scene with a tonemap: one full-screen draw from the HDR scene (B10G11R11) into an
// sRGB 8-bit target of the scene's size. When the scene is the output's size that target is the output, and a pair of
// copies follows (out to a scratch target and back: in Astral Chain that pair was the game's FXAA, switched off here);
// at any other size one draw scales the target onto the output (measured with ACR_DUMP in the Hyrule Field battle).
// Those draws carry the game's screen transforms (dynamic resolution, camera zooms) in their vertex uniforms, so they
// stay the game's (replacing one showed the scene in a corner, or zoomed), and FXAA runs on the tonemap's target just
// before the first pass that reads it: the replay's pass is closed, the target is copied into a work image, FXAA draws
// from it back into the target, and the game's pass opens again. The HUD, drawn after, stays sharp.
// Recognised without shader hashes (they change with the translator): the tonemap is a pass with one sRGB 8-bit colour
// target (the depth attached at 100 %) whose first draw is full-screen (at most six vertices) and samples, in the
// fragment shader's slot 0, a B10G11R11 texture of the target's size; the reader is the next pass of the same command
// buffer whose first draw samples that target in slot 0. Matching the pair of copies instead, FXAA ran at one size
// alone (1920x1080 output, render scale 100 %). At most once a frame, and not in a frame DLAA or DLSS ran (they already
// anti-alias it).
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
    // the last pass looked at (its first draw)
    uint64_t fence;           // its command buffer's timeline value
    uint32_t serial;          // its VkExec.pass_serial
    VkImage tm;               // the tonemap's target, until a pass reads it
    VkFormat tm_fmt;          // its format (sRGB)
    uint64_t tm_fence;        // its command buffer
    VkImage work;             // a copy of the output for FXAA to read (the output's size and image format)
    VmaAllocation work_mem;
    VkImageView work_view;    // in the target's format
    uint32_t work_w, work_h;
    VkFormat work_fmt, work_view_fmt;
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

static void free_work(VkCtx *c) {
    if (F.work_view) vkDestroyImageView(c->device, F.work_view, NULL);
    if (F.work) vmaDestroyImage(c->vma, F.work, F.work_mem);
    F.work_view = VK_NULL_HANDLE, F.work = VK_NULL_HANDLE, F.work_mem = NULL;
}

// The work image for an output of w x h in image format fmt, read through a view of format vfmt. It is in GENERAL from
// its first use on (*fresh: not yet).
static int ensure_work(VkCtx *c, uint32_t w, uint32_t h, VkFormat fmt, VkFormat vfmt, int *fresh) {
    *fresh = 0;
    if (F.work && F.work_w == w && F.work_h == h && F.work_fmt == fmt && F.work_view_fmt == vfmt) return 1;
    if (F.work) {   // another output size or format: earlier frames may still read the old image
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        free_work(c);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &F.work, &F.work_mem, NULL) != VK_SUCCESS) {
        F.work = VK_NULL_HANDLE;
        rt_log("fxaa: no work image (%ux%u); FXAA is off", w, h);
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.work,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vfmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &F.work_view) != VK_SUCCESS) {
        F.work_view = VK_NULL_HANDLE;
        free_work(c);
        return 0;
    }
    F.work_w = w, F.work_h = h, F.work_fmt = fmt, F.work_view_fmt = vfmt;
    *fresh = 1;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int srgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

// The source image can take a view of the target's format: the same, or its UNORM twin.
static int same_kind(VkFormat img, VkFormat target) {
    if (img == target) return 1;
    return (target == VK_FORMAT_R8G8B8A8_SRGB && img == VK_FORMAT_R8G8B8A8_UNORM) ||
           (target == VK_FORMAT_B8G8R8A8_SRGB && img == VK_FORMAT_B8G8R8A8_UNORM);
}

// A pass that can be the tonemap: one sRGB 8-bit colour target (a whole 2D level, never in the feedback-loop layout), its
// first draw full-screen.
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
    F.fence = ex->fence, F.serial = ex->pass_serial;
    uint64_t h = ex->tex[1][0];   // the fragment shader's slot 0 (stage 1)
    if (!h) return 0;
    view_t tv;
    int has_view = 0;
    const VkTexImage *s = vk_texture_by_id(NVN_HANDLE_TEX(h), &tv, &has_view);
    if (!s || !s->image) return 0;
    const VkExecTarget *t = &ex->color[0];
    if (candidate(ex, count) && s->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
        s->width == t->width && s->height == t->height) {   // the tonemap
        F.tm = t->image, F.tm_fmt = t->format, F.tm_fence = ex->fence;
        return 0;
    }
    if (!F.tm || s->image != F.tm || ex->fence != F.tm_fence) return 0;
    F.tm = VK_NULL_HANDLE;   // its first reader
    if (frame == F.frame_done || vk_dlaa_ran() || (ex->pass_ncolor && t->image == s->image) || s->fb_learned ||
        s->layers != 1 || s->samples > 1 || s->depth > 1 || !same_kind(s->format, F.tm_fmt))
        return 0;
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    VkFormat fmt[3] = {F.tm_fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};   // our own pass: the target alone
    int fresh;
    if (!F.ok || !pipeline(c, fmt) || !ensure_work(c, s->width, s->height, s->format, F.tm_fmt, &fresh)) return 0;
    // the tonemap's target through a view of its format (colour images are created mutable, vk_texture.c)
    VkImageView out = vk_exec_target_view(s, F.tm_fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    VkCommandBuffer cb = ex->cb;
    uint32_t w = s->width, hgt = s->height;
    vk_exec_pass_end(ex);
    // the target, as the tonemap left it, into the work image
    barrier(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, F.work, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, hgt, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, F.work, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    barrier(cb, F.work, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // FXAA from the work image back into the target
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = out,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, hgt}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.pipe);
    VkDescriptorImageInfo ii = {F.sampler, F.work_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout, 0, 1, &wr);   // through the recorder thread (vk_rec.h)
    float rcp[2] = {1.0f / (float)w, 1.0f / (float)hgt};
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rcp, rcp);
    VkViewport v = {0, 0, (float)w, (float)hgt, 0, 1};
    VkRect2D sc = {{0, 0}, {w, hgt}};
    vkCmdSetViewportWithCount(cb, 1, &v);
    vkCmdSetScissorWithCount(cb, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    ex->recorded += 14;
    // the game's reader goes on in its pass, opened again, with everything bound again
    ex->dirty |= VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    vk_exec_pass_begin(ex);
    F.serial = ex->pass_serial;   // its pass serial moved: its later draws are not looked at again
    F.frame_done = frame;
    if (++F.runs == 1 || F.runs % 1800 == 0)
        rt_log("fxaa: %llu frames on the tonemap's target (%ux%u, format %d; frame %llu)", (unsigned long long)F.runs, w,
               hgt, (int)F.tm_fmt, (unsigned long long)frame);
    return 0;   // the game's draw goes on
}
