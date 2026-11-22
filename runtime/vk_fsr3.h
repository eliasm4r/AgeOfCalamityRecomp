// AMD FSR 3.1 temporal upscaler (vk_fsr3.cpp): the FidelityFX SDK 1.1.4's upscaler (third_party/FidelityFX, MIT) on
// its Vulkan backend, the other engine of vk_dlaa.c's DLSS point (ACR_DLAA_ENGINE). The same picture, depth, motion
// vectors and jitter go to FSR 3.1 instead of NGX, on any GPU the back end runs on. Plain C interface: the SDK is C++.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage color;              // the picture (level 0, layer 0): the scene light (DLAA) or the tonemapped 8-bit one
    VkFormat color_format;
    VkImage depth;              // R32F copy of the scene depth (vk_dlaa.c fsr_eval), w x h, inverted (1 near, 0 far)
    VkImage mv;                 // RG16F motion vectors, w x h with the scene in rw x rh: last frame's window position
                                // minus this frame's, render pixels, without the jitter (vk_dlaa.c mv_pass)
    VkImage out;                // RGBA16F with storage, ow x oh: the upscaled picture
    uint32_t w, h;              // the colour and depth images' size
    uint32_t rw, rh;            // the part of them the scene was drawn in, from the top-left (the dynamic resolution)
    uint32_t ow, oh;
    float jx, jy;               // the jitter the scene was drawn with, render pixels (x right, y down)
    float near_m, far_m, fov_y; // the camera's planes (world units) and vertical field of view (radians)
    float meters_per_unit;      // the world's unit in metres
    float dt_ms;                // time since the last upscaled frame
    int reset;                  // no usable history (a cut, a new size, the first frame)
    float sharpness;            // FSR 3.1's own sharpening (RCAS) on its output, 0..1; 0 leaves the pass out
    uint64_t fence;             // the submission's timeline value (VkExec.fence)
} Fsr3Frame;

// Records FSR 3.1 into cb (every image in VK_IMAGE_LAYOUT_GENERAL, left so; no render pass open; recorder drained by the
// caller). The context is made at the first call and made again when the output size changes; the scene may be any
// size up to the output. 0 when it failed (logged; FSR 3.1 stays off for the run).
int fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f);
// Before the device goes away; harmless when nothing was made.
void fsr3_release(void);

#ifdef __cplusplus
}
#endif
