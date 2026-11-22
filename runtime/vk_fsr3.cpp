// AMD FSR 3.1 temporal upscaler (ACR_DLAA_ENGINE = fsr3, or auto without DLSS): the FidelityFX SDK 1.1.4's upscaler
// context (third_party/FidelityFX, MIT) on the SDK's Vulkan backend, driven directly, without the ffx-api DLL layer.
//
// Adapted from TotK Recomp (https://github.com/valentderah/totk-pc-recomp @ afae80c908ab9fc9cdbafa9ed5295c946d80aa21,
// MIT, Copyright (c) 2026 Valiantsin Dzerakh), runtime/vk_fsr3.cpp and the shader lookup of runtime/vk_fg.cpp, itself
// ported from Savage Wind (https://github.com/Revivedx/SavageWindRecomp, MIT), runtime/nvn/vulkan/fsr3.cpp: the context
// with the three images FSR 3.1 has the application own (dilated depth and motion vectors, the previous frame's
// nearest depth), the resource wrapping and the dispatch parameters.
//
// What differs from TotK Recomp: the scene is drawn into the top-left part of its targets (the game's dynamic
// resolution), so the render size is that part and the images keep their own size; the depth is an R32F copy of the
// game's D32F/S8 depth (vk_dlaa.c); in the DLSS modes the input is the game's tonemapped 8-bit picture, which the HDR
// shaders take as values within 0..1. No frame generation: the lookup below serves the upscaler alone.
//
// Everything is recorded into the replay's command buffer by vk_dlaa.c evaluate, on the replay thread, after it drained
// the recorder thread; the SDK binds pipelines and descriptor sets of its own, so vk_dlaa.c makes the next draw bind
// everything again.
#include "vk_fsr3.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>

extern "C" {
#include "cfg.h"
#include "host.h"
#include "vk.h"
}

// The shaders: tools/ffx_shaders.py (wave32; HDR input, render-size motion vectors without jitter, the game's inverted
// depth; 32-bit and 16-bit floats) in place of the SDK's FidelityFX_SC permutation tables
#include "ffx_fsr3upscaler_vk.h"

enum { FU_PERM_DEPTH_INVERTED = 1 << 4, FU_PERM_FP16 = 1 << 7 };  // ffx_fsr3upscaler_private.h FSR3UPSCALER_SHADER_PERMUTATION_*

static struct {
    bool live, failed, fp16;
    FfxFsr3UpscalerContext ctx;
    FfxInterface iface;
    void *scratch;
    FfxUInt32 shared_id;
    FfxResourceInternal shared[3];
    uint32_t ow, oh;
    // the timeline values of the last dispatches: the backend reuses its descriptor sets, constant buffers and image
    // views after FFX_MAX_QUEUED_FRAMES dispatches (wait_ring)
    uint64_t ring[FFX_MAX_QUEUED_FRAMES];
    uint32_t n;
    uint64_t waits;
} F;

// The backend's shader lookup (ffx_shader_blobs.h): the upscaler's passes; the 16-bit version where the SDK asks for it
// (the backend found shaderFloat16) and the device has what its SPIR-V declares (Float16, Int16).
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options,
                                                     FfxShaderBlob *out) {
    if (effect == FFX_EFFECT_FSR3UPSCALER && (uint32_t)pass < sizeof ffx_fu_blobs[0] / sizeof ffx_fu_blobs[0][0] &&
        ffx_fu_blobs[0][pass].data && !(options & FU_PERM_DEPTH_INVERTED) == !FFX_FU_DEPTH_INVERTED) {
        int half = (options & FU_PERM_FP16) && F.fp16;
        memcpy(out, &ffx_fu_blobs[half][pass], sizeof *out);
        return FFX_OK;
    }
    rt_log("fsr3: no shader for effect %d pass %d (options %#x)", (int)effect, (int)pass, options);
    return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;  // (only wave32 blobs: the backend then asks for no subgroup size)
    return FFX_OK;
}

// The backend's hook into its frame interpolation swapchain, which isn't built
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }

// The backend calls a few extension functions unchecked: the debug labels (VK_EXT_debug_utils, an instance extension
// the runtime enables only with validation) and vkGetBufferMemoryRequirements2KHR (core since 1.1 without the suffix)
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return f;
    if (!strcmp(name, "vkGetBufferMemoryRequirements2KHR")) return vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2");
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    return nullptr;
}

// The backend's scratch memory holds its context, whose members the compiler aligns past malloc's 16 bytes
static void *scratch_alloc(size_t n) {
#ifdef _WIN32
    void *p = _aligned_malloc(n, 256);
#else
    void *p = aligned_alloc(256, n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

static void scratch_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static void message(FfxMsgType type, const wchar_t *msg) {
    static int n;
    if (n++ >= 32) return;   // (the debug checks repeat every frame)
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    rt_log("fsr3: SDK %s: %s", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

// The SDK's objects: the device is idle or the context never recorded anything.
static void destroy(void) {
    if (F.live) {
        for (int i = 0; i < 3; i++) F.iface.fpDestroyResource(&F.iface, F.shared[i], F.shared_id);
        F.iface.fpDestroyBackendContext(&F.iface, F.shared_id);
        ffxFsr3UpscalerContextDestroy(&F.ctx);
    }
    scratch_free(F.scratch);
    F.scratch = nullptr;
    F.live = false;
}

static int fp16_usable(VkPhysicalDevice phys) {
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    return f12.shaderFloat16 && f2.features.shaderInt16;
}

#define TRY(x, what)                                                                                                   \
    do {                                                                                                               \
        FfxErrorCode e_ = (x);                                                                                         \
        if (e_ != FFX_OK) {                                                                                            \
            rt_log("fsr3: %s failed (%#x) at %ux%u: FSR 3.1 off for this run", what, (unsigned)e_, ow, oh);        \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool create(const VkCtx *c, uint32_t ow, uint32_t oh) {
    enum { CONTEXTS = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1 };  // (+ the shared images')
    size_t n = (ffxGetScratchMemorySizeVK(c->phys, CONTEXTS) + 255) & ~(size_t)255;
    if (!(F.scratch = scratch_alloc(n))) {
        rt_log("fsr3: no memory for the backend (%zu bytes): FSR 3.1 off for this run", n);
        return false;
    }
    VkDeviceContext dc = {c->device, c->phys, device_proc};   // (ffxGetDeviceVK keeps a copy)
    FfxFsr3UpscalerContextDescription cd = {};
    TRY(ffxGetInterfaceVK(&cd.backendInterface, ffxGetDeviceVK(&dc), F.scratch, n, CONTEXTS), "the backend");
    // HDR shaders (the scene light; the tonemapped picture is within 0..1). Render-size motion vectors without the
    // jitter (the defaults), the game's inverted depth with a finite far plane, a render size that moves (the dynamic
    // resolution). ACR_FSR3_DEBUG=1: the SDK's checks of the inputs, in the log.
    cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION |
               (FFX_FU_DEPTH_INVERTED ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED : 0) |
               (rt_knob("FSR3_DEBUG", "0") ? FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING : 0);
    cd.maxRenderSize = {ow, oh};
    cd.maxUpscaleSize = {ow, oh};
    cd.fpMessage = message;
    TRY(ffxFsr3UpscalerContextCreate(&F.ctx, &cd), "context creation");
    F.iface = cd.backendInterface;
    F.live = true;   // (from here destroy() takes the context down)
    FfxFsr3UpscalerSharedResourceDescriptions sd = {};
    TRY(ffxFsr3UpscalerGetSharedResourceDescriptions(&F.ctx, &sd), "the shared images' descriptions");
    TRY(F.iface.fpCreateBackendContext(&F.iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &F.shared_id), "the shared context");
    const FfxCreateResourceDescription *rd[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3; i++) TRY(F.iface.fpCreateResource(&F.iface, rd[i], F.shared_id, &F.shared[i]), "a shared image");
    F.ow = ow, F.oh = oh, F.n = 0;
    memset(F.ring, 0, sizeof F.ring);
    FfxEffectMemoryUsage mu = {};
    ffxFsr3UpscalerContextGetGpuMemoryUsage(&F.ctx, &mu);
    rt_log("fsr3: FSR 3.1 context for a %ux%u output, scenes up to that size (%s floats), %.1f MiB", ow, oh,
           F.fp16 ? "16-bit" : "32-bit", (double)mu.totalUsageInBytes / 1048576.0);
    return true;
}

// An image of the runtime as the SDK sees it: every one is in GENERAL, which the backend takes both states as and
// returns it to after the dispatch.
static FfxResource res(VkImage img, VkFormat f, uint32_t w, uint32_t h, bool write, const wchar_t *name) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    return ffxGetResourceVK((void *)img, ffxGetImageResourceDescriptionVK(img, ci, write ? FFX_RESOURCE_USAGE_UAV
                            : FFX_RESOURCE_USAGE_READ_ONLY), name, write ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
                            : FFX_RESOURCE_STATE_GENERIC_READ);
}

// The backend writes a dispatch's descriptor sets and constants into rings FFX_MAX_QUEUED_FRAMES deep and destroys the
// image views of the dispatch that many back at the end of each one, without knowing the GPU. With the game's two or
// three frames in flight that dispatch is long done; should it not be, its submission is waited for here (counted).
static void wait_ring(uint64_t fence) {
    if (F.n >= FFX_MAX_QUEUED_FRAMES - 1) {
        uint64_t back = F.ring[(F.n + 1) % FFX_MAX_QUEUED_FRAMES];   // the dispatch FFX_MAX_QUEUED_FRAMES - 1 back
        if (back && vk_sync_fence_done() < back) {
            if (!F.waits++) rt_log("fsr3: waiting for the submission of a dispatch %d back (more frames in flight than "
                                   "the SDK's rings hold)", FFX_MAX_QUEUED_FRAMES - 1);
            vk_sync_fence_wait(back, 2000000000ull);
        }
    }
    F.ring[F.n % FFX_MAX_QUEUED_FRAMES] = fence;
}

extern "C" int fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f) {
    if (F.failed) return 0;
    const VkCtx *c = vk_ctx();
    if (!F.live || f->ow != F.ow || f->oh != F.oh) {
        if (F.live) {
            vkDeviceWaitIdle(c->device);   // what the old context recorded is queued or submitted
            destroy();
        }
        F.fp16 = fp16_usable(c->phys) && rt_knob("FSR3_FP16", "1");   // ACR_FSR3_FP16=0: the 32-bit shaders (test)
        if (!create(c, f->ow, f->oh)) {
            destroy();
            F.failed = true;
            return 0;
        }
    }
    if (f->rw > F.ow || f->rh > F.oh) {
        rt_log("fsr3: a %ux%u scene is larger than the %ux%u output: FSR 3.1 off for this run", f->rw, f->rh, F.ow, F.oh);
        F.failed = true;
        return 0;
    }
    wait_ring(f->fence);
    FfxFsr3UpscalerDispatchDescription dd = {};
    dd.commandList = ffxGetCommandListVK(cb);
    dd.color = res(f->color, f->color_format, f->w, f->h, false, L"FSR3_Color");
    dd.depth = res(f->depth, VK_FORMAT_R32_SFLOAT, f->w, f->h, false, L"FSR3_Depth");
    dd.motionVectors = res(f->mv, VK_FORMAT_R16G16_SFLOAT, f->rw, f->rh, false, L"FSR3_MotionVectors");
    dd.output = res(f->out, VK_FORMAT_R16G16B16A16_SFLOAT, f->ow, f->oh, true, L"FSR3_Output");
    dd.dilatedDepth = F.iface.fpGetResource(&F.iface, F.shared[0]);
    dd.dilatedMotionVectors = F.iface.fpGetResource(&F.iface, F.shared[1]);
    dd.reconstructedPrevNearestDepth = F.iface.fpGetResource(&F.iface, F.shared[2]);
    // (no reactive or transparency mask, no exposure: the SDK's defaults, zero and exposure 1)
    dd.jitterOffset = {f->jx, f->jy};
    dd.motionVectorScale = {1.0f, 1.0f};   // render pixels already
    dd.renderSize = {f->rw, f->rh};
    dd.upscaleSize = {f->ow, f->oh};
    dd.enableSharpening = f->sharpness > 0.0f;
    dd.sharpness = f->sharpness > 1.0f ? 1.0f : f->sharpness;
    dd.frameTimeDelta = f->dt_ms;
    dd.preExposure = 1.0f;
    dd.reset = f->reset != 0;
    dd.cameraNear = f->near_m;
    dd.cameraFar = f->far_m;
    dd.cameraFovAngleVertical = f->fov_y;
    dd.viewSpaceToMetersFactor = f->meters_per_unit;
    static int view = -1;                  // ACR_FSR3_VIEW=1: FSR's own debug view of its inputs and state
    if (view < 0) view = rt_knob("FSR3_VIEW", "0");
    if (view) dd.flags |= FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&F.ctx, &dd);
    F.n++;
    if (e != FFX_OK) {
        rt_log("fsr3: dispatch failed (%#x) at %ux%u -> %ux%u: FSR 3.1 off for this run", (unsigned)e, f->rw, f->rh,
               f->ow, f->oh);
        F.failed = true;
        return 0;
    }
    return 1;
}

extern "C" void fsr3_release(void) {
    const VkCtx *c = vk_ctx();
    if (!c || !c->device || (!F.live && !F.scratch)) return;
    vkDeviceWaitIdle(c->device);
    if (F.waits) rt_log("fsr3: %llu dispatches waited for an earlier one's submission", (unsigned long long)F.waits);
    destroy();
}
