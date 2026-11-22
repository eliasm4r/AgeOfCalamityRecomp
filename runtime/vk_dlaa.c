// DLAA: NVIDIA DLSS at the scene's own size (ACR_DLAA, settings.ini dlaa). NGX is loaded at run time from the driver's
// NGX core (_nvngx.dll, found through HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore); the DLSS model
// (nvngx_dlss.dll) is loaded by NGX from the folder of AgeOfCalamityRecomp. No NVIDIA SDK code or header is compiled in: the few
// NGX types used are declared here from the SDK's documented ABI, the way the Metroid Dread recomp's dlss.c reaches
// the core (read for behaviour only).
//
// The core's parameter object is a C++ class whose virtual functions are called through the MSVC vtable: the Set
// overloads in slots 0-7 and the Get overloads in 8-15, each group in reverse declaration order. vtable_ok checks that
// layout with a set-then-get round trip before anything relies on it.
//
// Where DLAA runs in the frame, the jitter and the motion vectors: see the sections below. AMD FSR 3.1 (vk_fsr3.cpp)
// takes NGX's place on any GPU and system (ACR_DLAA_ENGINE); NGX itself is loaded on Windows only.
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_fsr3.h"
#include "vk_rec.h"
#include "nvn_api.h"
#include "dlaa_mv_spv.h"

typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERFQ_DLAA 5   // NVSDK_NGX_PerfQuality_Value: 0 performance, 1 balanced, 2 quality, 3 ultra performance
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {   // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {   // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;      // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);

static struct {
    void *core;   // the NGX core's module (HMODULE)
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    int tried, loaded, available;
    int wanted;   // -1 not read yet
} N = {.wanted = -1};

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
#ifdef _WIN32   // (vtable_ok and ngx_init)
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }
#endif

// NGX is brought up only when dlaa is on at start: its device extensions can only be asked for at device creation, and
// loading the driver's NGX core on every NVIDIA machine would cost start time and change the device with DLAA off. So
// dlaa applies after a restart; switched off, nothing else of DLAA runs.
int vk_dlaa_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("ACR_DLAA");
    return wanted;
}

// The engine behind the DLSS point, picked at start (ACR_DLAA_ENGINE): dlss, NVIDIA DLSS through NGX; fsr3, AMD FSR 3.1
// (vk_fsr3.cpp) on any GPU; auto, DLSS where NGX reports it, else FSR 3.1. Both take the same picture, depth, motion
// vectors and jitter at the same point of the frame (evaluate).
enum { ENG_NONE, ENG_DLSS, ENG_FSR3 };
static int engine;
static int engine_may_dlss(void) { return strcmp(cfg_value("ACR_DLAA_ENGINE"), "fsr3") != 0; }

// dlaa was on at start and vk_dlaa_init found no DLSS (no NVIDIA RTX, no NGX core, no nvngx_dlss.dll): the menu says so
// instead of showing a switch that changes nothing.
int vk_dlaa_missing(void) { return vk_dlaa_wanted() && N.tried && !N.available; }

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
#ifndef _WIN32
    rt_log("dlaa: NGX (DLSS) is loaded on Windows only");
    return 0;
#else
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS) {
        rt_log("dlaa: the driver has no NGX core (registry NGXCore missing): DLAA off");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (error %lu): DLAA off", path, GetLastError());
        return 0;
    }
#define FN(f, name) N.f = (void *)GetProcAddress((HMODULE)N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("dlaa: the NGX core %s lacks Vulkan entry points: DLAA off", path);
        return 0;
    }
    N.loaded = 1;
    rt_log("dlaa: NGX core %s", path);
    return 1;
#endif
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !engine_may_dlss() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n,
                         uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !engine_may_dlss() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) rt_log("dlaa: the device lacks %s, which NGX asks for", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

#ifdef _WIN32   // (the MSVC layout of the parameter object: NGX is loaded on Windows only)
static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "ACR.Test.I", -12345);
    p_setu(p, "ACR.Test.U", 4000000000u);
    p_setf(p, "ACR.Test.F", 1.25f);
    p_setp(p, "ACR.Test.P", (void *)p);
    int ok = p_geti(p, "ACR.Test.I", &i) && i == -12345 && p_getu(p, "ACR.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "ACR.Test.F", &f) && f == 1.25f && p_getp(p, "ACR.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) rt_log("dlaa: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLAA off", i, u, f, v);
    return ok;
}
#endif

static void ngx_init(void);

void vk_dlaa_init(void) {
    if (!vk_dlaa_wanted()) return;
    const char *want = cfg_value("ACR_DLAA_ENGINE");
    if (engine_may_dlss()) ngx_init();
    if (N.available) {
        engine = ENG_DLSS;
        return;
    }
    if (!strcmp(want, "dlss")) return;   // DLSS asked for and missing: the menu says so (vk_dlaa_missing)
    // FSR 3.1 needs nothing of the driver beyond Vulkan 1.3: its context is made at the first frame (vk_fsr3.cpp)
    engine = ENG_FSR3;
    N.available = 1;
    rt_log("dlaa: AMD FSR 3.1 anti-aliases and upscales the scene (dlaa_engine = %s)", want);
}

static void ngx_init(void) {
    if (!core_load()) return;
#ifdef _WIN32
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
    // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
    const VkCtx *c = vk_ctx();
    NgxResult r = N.init(231313132ull, wdir, c->instance, c->phys, c->device, vkGetInstanceProcAddr,
                         vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        rt_log("dlaa: NGX init failed (%08x): DLAA off", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    rt_log("dlaa: NGX up; DLSS %s%s", avail ? "available" : "NOT available (is nvngx_dlss.dll next to AgeOfCalamityRecomp?)",
           driver ? "; the driver needs an update" : "");
#endif
}

// ---------------------------------------------------------------- where DLAA runs in the frame
// Measured (ACR_DUMP=900, Hyrule Field battle, 960x540 scene): the G-buffer pass draws six colour targets with the
// scene depth (D32S8 at the scene size); lighting and the transparents then draw into the R11G11B10F light target
// alone with that depth; the bloom chain (480x270 and smaller) reads it, then colour grading, the tonemap into an 8-bit
// sRGB picture at the scene size, the scaling draw into the output, and the HUD over the output.
// So: after the G-buffer, a pass at the scene size with exactly one colour target (R11G11B10F or RGBA16F) and the D32F
// depth arms the frame with that pair; the next pass at most a quarter as wide ends the scene (no jitter after it) and,
// when the scene is as wide as the output, runs DLAA on the armed target first. Bloom, tonemap, post and HUD then all
// work on the anti-aliased picture.
// Upscaling (a scene smaller than the output: ACR_DLSS quality/balanced/performance, or the game's own 5/6): Age of
// Calamity tonemaps at the scene size before it scales, so DLSS runs later, at the game's scaling draw ("the scaling
// draw" below). The HUD, drawn afterwards, is never jittered nor passed through DLSS.
static struct {
    VkExecTarget color, depth;   // the armed pair, valid in the frame it was armed
    int armed, done, gbuf;       // this frame: armed on the light target, DLAA ran, the G-buffer was drawn
    int ended, up;               // this frame: the scene is drawn (jitter over), DLSS wrote the scaled output
    struct Size {                // a DLSS feature and its work images per scene size (dynamic resolution flips it)
        uint32_t w, h, ow, oh;   // input size (the scene's targets), output size
        uint32_t cw, ch;         // the render size the feature takes (the scene's share of w x h): NGX refuses any other
        VkImage out, mv;
        VmaAllocation out_mem, mv_mem;
        VkImageView out_view, mv_view;
        NgxHandle *feat;
        VkImage zc;              // FSR 3.1: the scene depth's copy as R32F (the SDK takes no D32F/S8), w x h,
        VmaAllocation zc_mem;    // through the buffer zb
        VkImageView zc_view;
        VkBuffer zb;
        VmaAllocation zb_mem;
        int ldr;                 // the feature takes the 8-bit tonemapped picture (the DLSS modes), not scene light
        uint64_t used;           // evals count when it last ran
    } sizes[4], *last;           // last: the one the previous DLAA frame ran with
    VkImage last_color;          // the target DLAA ran on last frame: another one means a new history
    NgxParam *params;
    int failed, reset;
    uint64_t evals, frames_missed;
    // jitter (vk_dlaa_viewport)
    VkImage jdepth;              // the scene depth of the last frame DLAA ran on: passes drawing with it are jittered
    float jx, jy;                // the offset the next scene draws take, render pixels (window x right, y down)
    float ux, uy;                // the offset this frame's scene draws took (DLSS and the motion vectors get these)
    uint32_t jframe;
    float cam_vp[16];            // its four columns, copied when found
    uint64_t last_eval_ns;       // kern_now_ns of the previous evaluation (FrameTimeDeltaInMsec)
    int jit_ok, jit_used, hold;  // jitter allowed this frame / a draw used it / frames left without it
    uint64_t holds;
    // camera (vk_dlaa_draw) and motion vectors (mv_pass)
    uint64_t cam_addr;           // this frame's scene view-projection (4 vec4 columns in a scene draw's c3), 0: none seen
    VkBuffer mvm;                // MVM_SLOTS pairs of 256-byte blocks the shader reads: identity, reprojection
    VmaAllocation mvm_mem;
    uint8_t *mvm_map;
    uint32_t mvm_slot;
    int cam_prev_ok;             // cam_prev holds last frame's view-projection
    double cam_prev[16];         // row-major
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    int mv_ok;                   // -1 not built, 0 failed, 1 ready
    uint64_t mv_frames, mv_none;
    uint64_t why[3];             // history resets: scene size changed, no camera, scene target changed
    uint64_t other, sub_logs;    // scaling draws left to the game (not a plain scale); dynamic resolution changes
    uint32_t sub_w, sub_h;       // the scene's share of its targets at the last scaling draw
    int on;                      // the dlaa setting, read at each present (the menu switches it live)
    int preset, preset_dirty;    // the DLSS model features are created with (preset_value); a change pending
    unsigned cfg_seen;           // cfg_version when preset was read
} D = {.mv_ok = -1, .cfg_seen = ~0u};   // cfg_seen: the first frame reads the preset

static int dl_active(void) { return N.available && !D.failed && D.on; }

static void dl_fail(const char *what, NgxResult r) {
    rt_log("dlaa: %s failed (%08x): DLAA off for this run", what, (unsigned)r);
    D.failed = 1;
}

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static void free_size(struct Size *e) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);   // a third scene size: the evicted images may still be in flight
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->feat && engine == ENG_DLSS) N.release(e->feat);
    if (e->zc_view) vkDestroyImageView(c->device, e->zc_view, NULL);
    if (e->zc) vmaDestroyImage(c->vma, e->zc, e->zc_mem);
    if (e->zb) vmaDestroyBuffer(c->vma, e->zb, e->zb_mem);
    memset(e, 0, sizeof *e);
}

// The entry for a scene and output size: its output (RGBA16F at the output size, written by DLSS, then blitted into
// the target) and motion vectors (RG16F at the scene size, render pixels), both kept in GENERAL like every image of the
// back end. The game's dynamic resolution moves the scene's share a step at a time; each share keeps its feature, the
// least recently used goes for a new one.
// sw x sh: the render subrect, which the entry's feature must take (a new entry is created at it).
static struct Size *size_for(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int ldr, uint32_t sw,
                             uint32_t sh) {
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < 4; i++) {
        const struct Size *x = &D.sizes[i];
        if (x->w == w && x->h == h && x->ow == ow && x->oh == oh && x->ldr == ldr &&
            x->cw == sw && x->ch == sh)
            return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        free_size(e = lru);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(sw, sh, VK_FORMAT_R16G16_SFLOAT, u, &e->mv, &e->mv_mem, &e->mv_view))   // at the feature's size
        return NULL;
    int nim = 2;
    if (engine == ENG_FSR3) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!image2d(w, h, VK_FORMAT_R32_SFLOAT, u, &e->zc, &e->zc_mem, &e->zc_view) ||
            vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &e->zb, &e->zb_mem, NULL) != VK_SUCCESS)
            return NULL;
        nim = 3;
    }
    VkImageMemoryBarrier2 ib[3];
    VkImage im[3] = {e->out, e->mv, e->zc};
    for (int i = 0; i < nim; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)nim,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->ow = ow, e->oh = oh, e->ldr = ldr, e->cw = sw, e->ch = sh;
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a,
                               int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

// ---------------------------------------------------------------- motion vectors
// Camera motion only (runtime/shaders/dlaa_mv.comp). Age of Calamity has no camera block of its own: every scene draw's
// vertex c3 holds the view-projection as four vec4 columns (measured with ACR_DUMP_UBO in the first battle; at vec4 0
// or 1 of the block depending on the shader, the model matrix further down), and the depth is reversed. The
// reprojection from this frame's clip space to last frame's, last * inverse(this), is made on the CPU in double
// precision, as TotK's recomp does since its fix of the shimmer on distant surfaces (a float inverse in the shader with
// world coordinates in the tens of thousands gave noise of a fraction of a pixel, which DLSS followed into a heat-haze
// shimmer): the shader's "this frame" block gets the identity and its "last frame" block that matrix. Characters and
// anything else that moves on its own get the camera's motion only, which DLSS mostly copes with by rejecting their
// history.
#define CAM_BYTES 256u   // a block of the shader: vec4 rows 3-6 are the matrix it applies, row-major
#define MVM_SLOTS 16u    // frames the GPU may still be reading the reprojection blocks of

static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MVM_SLOTS * 2 * CAM_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo minfo;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &bi, &ai, &D.mvm, &D.mvm_mem, &minfo) != VK_SUCCESS)
        goto fail;
    D.mvm_map = minfo.pMappedData;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return D.mv_ok = 1;
fail:
    rt_log("dlaa: the motion-vector pass could not be set up: DLAA runs with zero motion");
    return 0;
}

static int inv4(const double m[16], double o[16]) {   // Gauss-Jordan with partial pivoting, row-major
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

static void mul4(const double a[16], const double b[16], double o[16]) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double v = 0;
            for (int j = 0; j < 4; j++) v += a[r * 4 + j] * b[j * 4 + k];
            o[r * 4 + k] = v;
        }
}

// The pass, recorded through the recorder like the replay's own commands. 0: no camera this frame (the vectors stay
// zero and DLSS starts a new history).
// w x h: the scene's part of the depth (its viewport, from the top-left corner)
static int mv_pass(VkCommandBuffer cb, const struct Size *e, VkImageView zv, uint32_t w, uint32_t h, float jx,
                   float jy) {
    const float *f = D.cam_addr && mv_init() ? D.cam_vp : NULL;
    if (!f) {
        D.mv_none++;
        D.cam_prev_ok = 0;
        return 0;
    }
    double cur_vp[16], ci[16], m[16];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) cur_vp[r * 4 + k] = f[k * 4 + r];   // column k, component r
    int had_prev = D.cam_prev_ok, ok = inv4(cur_vp, ci);
    if (ok && had_prev) {
        // The shader rebuilds clip z as depth * 2 - 1 (NEAR_IS_MINUS_W); with the 0..1 depth mode the depth is clip z
        // itself, so z' -> (z' + 1) / 2 goes in front of the inverse.
        double t[16];
        mul4(D.cam_prev, ci, t);
        if (!nvn_depth_minus_w()) {
            static const double s[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5, 0.5, 0, 0, 0, 1};
            mul4(t, s, m);
        } else {
            memcpy(m, t, sizeof m);
        }
    }
    memcpy(D.cam_prev, cur_vp, sizeof cur_vp);
    D.cam_prev_ok = ok;
    if (!ok || !had_prev) return 0;   // nothing to reproject into: zero motion, new history
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t ms = D.mvm_slot++ % MVM_SLOTS;
    float *blk = (float *)(D.mvm_map + (size_t)ms * 2 * CAM_BYTES);   // rows 3-6 of each block are read
    memset(blk, 0, 2 * CAM_BYTES);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            blk[12 + r * 4 + k] = r == k ? 1.0f : 0.0f, blk[64 + 12 + r * 4 + k] = (float)m[r * 4 + k];
    vmaFlushAllocation(vk_ctx()->vma, D.mvm_mem, (VkDeviceSize)ms * 2 * CAM_BYTES, 2 * CAM_BYTES);
    VkDescriptorBufferInfo cur = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES + CAM_BYTES, CAM_BYTES};
    VkWriteDescriptorSet wr[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cur},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &prev}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 4, wr);
    float pc[4] = {(float)w, (float)h, jx, jy};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    D.mv_frames++;
    return 1;
}

// The scene depth: D32F in TotK, D32F with stencil in Age of Calamity (NVN d32fs8, measured with ACR_DUMP in the first
// battle: the depth prepass, the G-buffer, the light target and the transparents all use it).
static int scene_depth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

// ---------------------------------------------------------------- jitter
// The scene's draws (every pass drawing with the scene depth, until the DLAA point) have their viewport moved by a
// sub-pixel Halton(2,3) offset, which shifts what they rasterise as a jittered projection would; full-screen passes
// that shade by window position are unaffected. Jitter only follows a frame DLSS ran on: a jittered frame that DLSS
// does not resolve shakes on screen, so a missed DLAA point stops the jitter for a second.
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    if (!D.jit_ok || D.ended || !ex->depth.image || ex->depth.image != D.jdepth) return;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy, D.jit_used = 1;
}

// The scene camera: the view-projection in the vertex c3 of a scene draw (one drawing with the scene depth). It is four
// vec4 columns whose w components, the camera's forward axis, make a unit vector; the block's other matrices (the model
// matrix and the normal matrix) do not, nor do the zero rows before it in some shaders.
static uint64_t find_vp(uint64_t addr, uint64_t size) {
    if (!addr || size < 64) return 0;
    uint64_t n = size < 112 ? size : 112;   // the view-projection starts at vec4 0..3
    const float *f = (const float *)vk_usnap_bytes(addr, n);
    if (!f) return 0;
    for (uint64_t k = 0; (k + 4) * 16 <= n; k++) {
        const float *c = f + 4 * k;
        double fw = (double)c[3] * c[3] + (double)c[7] * c[7] + (double)c[11] * c[11];
        double x = fabs(c[0]) + fabs(c[1]) + fabs(c[2]);
        if (fabs(fw - 1.0) < 0.01 && x > 1e-3 && fabs(c[15]) > 1e-3) return addr + 16 * k;
    }
    return 0;
}

// The camera is taken from the first G-buffer draw of the frame: later scene draws (the sky, effects) can carry a
// view-projection of their own, and taking the last one gave DLSS wrong motion on some frames (flicker, resets). Once
// it is found the other draws are left alone: scanning each one's constants cost half the replay's busy time in a
// battle (1200 draws a frame).
void vk_dlaa_draw(VkExec *ex) {
    if (!dl_active() || D.ended || D.cam_addr) return;
    const p_bind_buf *b = &ex->ubo[0][0];   // the vertex stage's c3: the replay keeps user banks from c3 on
    const VkExecTarget *z = &ex->depth;
    if (!b->addr || !z->image || z->width < 640 || !scene_depth(z->format) || (D.jdepth && z->image != D.jdepth)) return;
    if (ex->pass_ncolor >= 3 && (D.cam_addr = find_vp(b->addr, b->size)) != 0)
        memcpy(D.cam_vp, vk_usnap_bytes(D.cam_addr, 64), 64);   // the snapshot may be gone by the DLAA point
}

// The jitter moves on right after DLAA ran, not at the present: the replay can record the next frame's scene before
// the present of this one is handled, and a step taken there landed in the middle of a frame (worse after an off/on
// switch, which shifted that order). Jitter needs a frame DLSS ran on; a frame without the DLAA point stops it.
// The sequence is as long as NVIDIA asks for the upscaling ratio, 8 * (output / scene)^2 phases, at least 16 (as TotK's
// recomp does): a shorter one leaves output pixels the jitter never samples in the upscaling modes.
static void next_jitter(uint32_t w, uint32_t ow) {
    static int nj = -1;
    if (nj < 0) nj = !rt_knob("DLAA_JITTER", "1");   // ACR_DLAA_JITTER=0: no jitter (test)
    double k = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * k * k - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = D.jframe++ % phases + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && !nj;
}

// The DLSS model (ACR_DLSS_PRESET, live), as TotK's recomp offers it: default takes NVIDIA's model per mode (K for
// DLAA, Quality and Balanced, M for Performance, L for Ultra Performance), 0 here. A letter a..m takes that model in
// every mode: k pins K, j is the other transformer model, which NVIDIA describes as slightly less ghosting at the cost
// of extra flicker. The value is an NVSDK_NGX_DLSS_Hint_Render_Preset (1 A ... 13 M), read when a feature is created;
// a change recreates the features (preset_check).
static int preset_value(void) {
    const char *v = cfg_value("ACR_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

// The quality setting DLSS is created with, from the scene's share of the output width.
static int perf_quality(uint32_t w, uint32_t ow) {
    if (w >= ow) return NGX_PERFQ_DLAA;
    double k = (double)w / ow;
    return k >= 0.66 ? 2 : k >= 0.57 ? 1 : k >= 0.49 ? 0 : 3;
}

// The camera's near and far planes and vertical field of view, which FSR 3.1 takes the depth back to distances with,
// from the scene's view-projection (D.cam_vp, vk_dlaa_draw): its last row is the view's forward axis (unit, find_vp),
// its third row a * forward + b (a perspective), so clip depth over w is a + b / distance. Age of Calamity's depth is
// reversed, 0..1 (measured in the Hyrule Field charge: a = -0.000305, b = 64.02, so 1 at 64 units and 0 at 210,000;
// the units look like centimetres: 0.64 m to 2.1 km); the -1..1 mode would store the same window depth. The second
// row's length is cot(fov / 2) (2.99 there: 37 degrees). Without a camera the last planes found stay, else defaults.
static void camera_planes(float *near_m, float *far_m, float *fov_y) {
    static float n = 64.0f, fa = 210000.0f, fov = 0.6458f;   // (the charge's)
    if (D.cam_addr) {
        const float *f = D.cam_vp;   // column k, component r: f[k * 4 + r]
        double a = 0, l1 = 0, l3 = 0;
        for (int k = 0; k < 3; k++)
            a += (double)f[k * 4 + 2] * f[k * 4 + 3], l1 += (double)f[k * 4 + 1] * f[k * 4 + 1],
                l3 += (double)f[k * 4 + 3] * f[k * 4 + 3];
        a /= l3;
        double b = f[14] - a * f[15], lo = nvn_depth_minus_w() ? -1.0 : 0.0;
        // b > 0: the depth falls with the distance (reversed, the game's: 1 at near, 0 at far), else it rises
        double nn = b > 0 ? b / (1.0 - a) : b / (lo - a), ff = b > 0 ? b / (lo - a) : b / (1.0 - a);
        if (b <= 0) {
            static int warned;
            if (!warned++) rt_log("dlaa: the scene's depth is not reversed: FSR 3.1's shaders are built for reversed depth");
        }
        double fy = l1 > 1e-12 ? 2.0 * atan(sqrt(l3 / l1)) : 0;
        if (nn > 1e-4 && ff > nn && isfinite(ff) && fy > 0.05 && fy < 3.0) {
            if (fabs(nn - n) > 1e-3 * n || fabs(ff - fa) > 1e-3 * fa) {
                static int logs;
                if (logs++ < 8) rt_log("dlaa: camera planes %.3f .. %.1f, vertical field of view %.1f degrees", nn, ff,
                                       fy * 57.29578);
            }
            n = (float)nn, fa = (float)ff, fov = (float)fy;
        }
    }
    *near_m = n, *far_m = fa, *fov_y = fov;
}

// FSR 3.1 (vk_fsr3.cpp) in place of NGX: the depth goes to it as R32F, copied from the depth aspect of the game's
// D32F/S8 through a buffer (no view of that format can be sampled as colour). One entry per target size: FSR 3.1 takes
// the scene's share as it moves (dynamic resolution) without a new context or history. 1 when it ran.
static int fsr_eval(VkExec *ex, struct Size *e, const VkExecTarget *t, const VkExecTarget *z, uint32_t sw, uint32_t sh,
                    uint32_t ow, uint32_t oh, float jx, float jy, double dt) {
    VkCommandBuffer cb = ex->cb;
    VkBufferImageCopy rz = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, z->level, z->base_layer, 1},
                            .imageExtent = {e->w, e->h, 1}};
    vkCmdCopyImageToBuffer(cb, z->image, VK_IMAGE_LAYOUT_GENERAL, e->zb, 1, &rz);
    full_barrier(cb);
    rz.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    vkCmdCopyBufferToImage(cb, e->zb, e->zc, VK_IMAGE_LAYOUT_GENERAL, 1, &rz);
    full_barrier(cb);
    // FSR 3.1's own sharpening (RCAS; fsr3_sharpness, live): its temporal accumulation softens the picture; 80 % by
    // default, as TotK Recomp has it
    int sharp = cfg_int("ACR_FSR3_SHARPNESS");
    sharp = sharp < 0 ? 0 : sharp > 100 ? 100 : sharp;
    Fsr3Frame f = {.color = t->image, .color_format = t->format, .depth = e->zc, .mv = e->mv, .out = e->out,
                   .w = e->w, .h = e->h, .rw = sw, .rh = sh, .ow = ow, .oh = oh, .jx = jx, .jy = jy,
                   .dt_ms = dt > 0 && dt < 100 ? (float)dt : 33.3f, .reset = D.reset, .fence = ex->fence,
                   .meters_per_unit = 0.01f, .sharpness = sharp / 100.0f};   // (centimetres: camera_planes)
    camera_planes(&f.near_m, &f.far_m, &f.fov_y);
    if (!e->feat) {   // (marks the entry's first run: the log line, a new history)
        e->feat = (NgxHandle *)(uintptr_t)1;
        f.reset = 1;
        rt_log("dlaa: FSR 3.1 on %ux%u targets -> %ux%u, %s input (format %d, depth format %d)", e->w, e->h, ow, oh,
               e->ldr ? "tonemapped" : "scene light", (int)t->format, (int)z->format);
    }
    return fsr3_dispatch(cb, &f);
}

// src: DLSS's input, NULL for the armed light target (DLAA in place, scene light); otherwise the 8-bit tonemapped
// picture at the scene size. dst: where DLSS's picture goes, NULL for the input itself. sw x sh: the part of the
// input the scene was drawn in from its top-left corner (the game's dynamic resolution), the whole of it for DLAA.
static void evaluate(VkExec *ex, const VkExecTarget *src, const VkExecTarget *dst, uint32_t sw, uint32_t sh) {
    const VkExecTarget *t = src ? src : &D.color, *z = &D.depth;
    int ldr = src != NULL;
    if (!dst) dst = t;
    uint32_t tw = t->width, th = t->height, ow = dst->width, oh = dst->height;
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);   // what drew the scene is done (ends an open pass)
    if (D.preset_dirty) {   // another model: features are created with it, so every size gets a new one
        for (size_t i = 0; i < sizeof D.sizes / sizeof *D.sizes; i++)
            if (D.sizes[i].out) free_size(&D.sizes[i]);
        D.last = NULL, D.preset_dirty = 0;
    }
    int fsr = engine == ENG_FSR3;
    struct Size *e = fsr ? size_for(cb, tw, th, ow, oh, ldr, tw, th) : size_for(cb, tw, th, ow, oh, ldr, sw, sh);
    uint32_t w = e ? e->cw : sw, h = e ? e->ch : sh;   // the feature's render size
    if (!e) return dl_fail("work image creation", 0);
    if (e != D.last) D.reset = 1, D.why[0]++;   // another size's history is from another moment
    if (!fsr && !D.params && !NGX_OK(N.alloc_params(&D.params))) return dl_fail("parameter allocation", 0);
    NgxParam *p = D.params;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");   // ACR_DLAA_MV=0: zero motion (test)
    if (no_mv || !mv_pass(cb, e, zv, sw, sh, jx, jy)) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv) D.reset = 1, D.why[1]++;   // no camera: the history cannot be moved along
    }
    rec_drain();   // NGX records into the command buffer itself: the recorder thread must have made every call so far
    if (D.last_color != t->image) D.reset = 1, D.why[2]++, D.last_color = t->image;
    // The time since the previous evaluation, which DLSS weighs its history by (Age of Calamity runs at 30 fps, not
    // the 60 assumed before); 60 fps before the first and after a gap (a pause, a load).
    uint64_t now = kern_now_ns();
    double dt = D.last_eval_ns ? (double)(now - D.last_eval_ns) / 1e6 : 0;
    D.last_eval_ns = now;
    if (fsr) {
        if (!fsr_eval(ex, e, t, z, sw, sh, ow, oh, jx, jy, dt)) return dl_fail("FSR 3.1", 0);
        w = sw, h = sh;
        goto resolved;
    }
    if (!e->feat) {
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", w);
        p_setu(p, "Height", h);
        p_setu(p, "OutWidth", ow);
        p_setu(p, "OutHeight", oh);
        // The feature's render size is the scene's share of its targets, which the game's dynamic resolution moves
        // between 70 and 100 % (Hyrule Field battle): measured, NGX's evaluation refuses (bad00005) any render subrect
        // but the size the feature was created at, smaller (671x377 on features created at 960x540 as Performance,
        // Quality or Ultra Performance, or at 894x502 as Quality) or larger (682x383 on 671x377), although NGX's
        // optimal settings announce a dynamic range (Quality at 1920x1080: 960x540 to 1920x1080). So each share gets
        // its own feature (size_for keeps four). The model is chosen by the mode's scale (the scene's targets).
        int q = perf_quality(w, ow);
        p_seti(p, "PerfQualityValue", q);
        // DLAA: IsHDR | MVLowRes | AutoExposure: linear scene light, motion vectors at the render size, no exposure
        // texture. The DLSS modes: MVLowRes alone: the tonemapped picture, 0..1 through its sRGB view, exposure 1.
        p_seti(p, "DLSS.Feature.Create.Flags", ldr ? (1 << 1) : (1 << 0) | (1 << 1) | (1 << 6));
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        // The model per mode as NVIDIA recommends and TotK's recomp uses (NVSDK_NGX_DLSS_Hint_Render_Preset, 1 A ... 13 M):
        // K, the transformer model, for DLAA to Balanced; M for Performance; L for Ultra Performance. dlss_preset
        // (D.preset) takes one model for every mode.
        unsigned pk = D.preset ? (unsigned)D.preset : 11, pm = D.preset ? (unsigned)D.preset : 13,
                 pl = D.preset ? (unsigned)D.preset : 12;
        int mq = perf_quality(tw, ow);   // the mode's scale (the scene's targets), which picks the model
        if (ldr) pk = pm = pl = mq == 3 ? pl : mq == 0 ? pm : pk;   // the mode's model whatever the share
        p_setu(p, "DLSS.Hint.Render.Preset.DLAA", pk);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraQuality", pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Quality", pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Balanced", pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Performance", pm);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraPerformance", pl);
        uint64_t t0 = kern_now_ns();
        NgxResult r = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &e->feat);
        if (!NGX_OK(r) || !e->feat) return dl_fail("DLSS feature creation", r);
        D.reset = 1;
        rt_log("dlaa: DLSS feature created at %ux%u -> %ux%u in %.1f ms, quality value %d, preset %c, %s input (format %d, "
               "depth format %d)", w, h, ow, oh, (double)(kern_now_ns() - t0) / 1e6, q,
               'A' + (int)(q == 0 ? pm : q == 3 ? pl : pk) - 1, ldr ? "tonemapped" : "scene light", (int)t->format,
               (int)z->format);
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    NgxResourceVK rc = res_image(cv, t->image, t->format, tw, th, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(e->out_view, e->out, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->image, z->format, tw, th, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(e->mv_view, e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", jx);
    p_setf(p, "Jitter.Offset.Y", jy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", D.reset);
    p_setf(p, "MV.Scale.X", 1.0f);
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", sw);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", sh);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    p_setf(p, "FrameTimeDeltaInMsec", dt > 0 && dt < 100 ? (float)dt : 16.67f);
    NgxResult r = N.evaluate(cb, e->feat, p, NULL);
    if (!NGX_OK(r)) {
        rt_log("dlaa: the evaluation refused: subrect %ux%u of a feature created at %ux%u (input %ux%u, output %ux%u)", sw,
               sh, w, h, tw, th, ow, oh);
        return dl_fail("DLSS evaluation", r);
    }
resolved:
    D.reset = 0;
    D.last = e, e->used = D.evals + 1;
    full_barrier(cb);
    // the result into the scene target (DLAA: the bloom, the post passes and the HUD read it) or into the scaling
    // draw's target (the DLSS modes: the HUD draws over it)
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, dst->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
    vkCmdBlitImage(cb, e->out, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    ex->recorded += 4;
    vk_exec_draw_new_cb();   // NGX or FSR 3.1 bound pipelines and descriptor sets of their own: the next draw binds
                             // everything again
    D.jdepth = z->image;
    next_jitter(sw, ow);
    if (++D.evals == 1 || D.evals % 3600 == 0)
        rt_log("dlaa: %llu frames anti-aliased (scene %ux%u, output %ux%u; %llu frames without the DLAA point, %llu with camera "
               "motion, %llu without a camera, %llu jitter holds; resets: %llu size, %llu camera, %llu target)",
               (unsigned long long)D.evals, w, h, ow, oh, (unsigned long long)D.frames_missed,
               (unsigned long long)D.mv_frames, (unsigned long long)D.mv_none, (unsigned long long)D.holds,
               (unsigned long long)D.why[0], (unsigned long long)D.why[1], (unsigned long long)D.why[2]);
}

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void vk_dlaa_pass(VkExec *ex) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (!dl_active() || D.done) return;
    // The G-buffer: several colour targets with a D32F depth of at least 640 wide. Only the light target drawn after it
    // arms the frame; at night the game draws into the light target alone with the scene depth before the G-buffer too,
    // and DLAA then ran at a small pass ahead of the whole scene, which reached the screen jittered.
    if (ex->pass_ncolor >= 3 && z->image && scene_depth(z->format) && z->width >= 640) {
        D.gbuf = 1, D.armed = 0;
        return;
    }
    // the scene depth is D32F; a D16 target of the scene size also serves one composite pass before the transparents
    if (D.gbuf && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image && scene_depth(z->format) &&
        c->width == z->width && c->height == z->height && c->width >= 640 &&
        c->samples <= 1 && !c->is3d) {
        D.color = *c, D.depth = *z, D.armed = 1;
        return;
    }
    if (!D.armed) return;
    if (!D.ended) {
        if (!ex->pass_w || ex->pass_w * 4 > D.color.width) return;
        D.ended = 1;
        unsigned o[2], s[2];
        res_sizes(o, s);
        // DLAA in place; a scene smaller than the output waits for the game's scaling draw (vk_dlaa_scale_draw)
        if (D.color.width >= o[0]) D.done = 1, evaluate(ex, NULL, NULL, D.color.width, D.color.height);
    }
}

// The preset, read again when a setting changed (the menu); a different one recreates the features at the next DLAA run.
static void preset_check(void) {
    unsigned v = cfg_version();
    if (v == D.cfg_seen) return;
    D.cfg_seen = v;
    int p = preset_value();
    if (p == D.preset) return;
    if (D.evals) {
        if (p) rt_log("dlaa: DLSS preset %c in every mode; the features are created again", 'A' + p - 1);
        else rt_log("dlaa: DLSS presets per mode; the features are created again");
    }
    D.preset = p, D.preset_dirty = 1, D.reset = 1;
}

void vk_dlaa_frame(void) {
    preset_check();
    int on = N.available && cfg_bool("ACR_DLAA");
    if (on != D.on) {
        rt_log("dlaa: %s", on ? "on" : "off");
        D.on = on;
        // either way the next DLAA frame starts a new history from an unjittered frame
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.gbuf = D.ended = D.up = 0;
        D.hold = 0;
        return;
    }
    if (!dl_active()) return;
    if (!D.done) D.frames_missed++;
    if (D.jit_used && !D.done) D.hold = 60, D.holds++;   // jittered but not resolved: that frame shook
    else if (D.hold) D.hold--;
    if (!D.done) D.jit_ok = 0;
    D.armed = D.done = D.jit_used = D.gbuf = D.ended = D.up = 0;
    D.cam_addr = 0;
}

// ---------------------------------------------------------------- the scaling draw (DLSS modes)
// With a scene smaller than the output (a DLSS mode, or the game's own 5/6 render scale) Age of Calamity does its
// bloom, colour grading and tonemap at the scene size, and only then scales: measured (ACR_DUMP=900, Hyrule Field
// battle, performance: 960x540 scene, 1920x1080 output) the tonemap writes an 8-bit sRGB picture at the scene size, and
// one draw of a quad on the output target's corners samples it into that output-sized sRGB target (a D32S8 depth
// attached, never tested after); the HUD is drawn over the target in the next pass. The quad's UVs carry the game's
// dynamic resolution: the scene is drawn into the top-left f x f of its targets (f = 0.7 after a few seconds in that
// battle) and the UVs run 0..f. So DLSS runs at that draw, on the tonemapped picture (a feature without IsHDR) with
// the armed scene depth, its render subrect the scene's f share (one feature per share: evaluate), and its picture,
// the whole output, is blitted into the target in place of the game's draw. A quad of another shape (a
// zoom: UVs not from 0, f not the same on both axes, or a quad not on the target's corners) is left to the game, which
// then scales the jittered picture: that frame stops the jitter for a second (vk_dlaa_frame). TotK's recomp swaps
// DLSS's output in for the texture the game samples instead, keeping the game's draw; with UVs 0..f that would show
// DLSS's whole picture zoomed. FXAA on the 3D picture is vk_fxaa.c.
static int srgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

// The range of the first two components of attribute format `fmt` (NVN 0x22 float3 position, 0x16 float2 UV: the
// game's quad layout, with an rgba8 colour between) over the quad's four vertices, read from the vertex stream in
// guest memory. 1 when found and read.
static int quad_range(const VkExec *ex, uint32_t fmt, float lo[2], float hi[2]) {
    for (int i = 0; i < 16; i++) {
        uint32_t a = ex->pipe.vattr[i], off = (a >> 8) & 0x7ffff, st = a >> 27;
        if ((a & 0xff) != fmt) continue;
        const NVNbufferRange *r = &ex->vbuf[st];
        uint32_t stride = ex->pipe.vstream[st].stride;
        VkMemSpan ms;
        if (!r->address || !stride || r->size < 3ull * stride + off + 8 || !vk_mem_resolve(r->address, r->size, &ms))
            return 0;
        lo[0] = lo[1] = 1e9f, hi[0] = hi[1] = -1e9f;
        for (uint32_t v = 0; v < 4; v++) {
            float x[2];
            memcpy(x, (const uint8_t *)(uintptr_t)r->address + (uint64_t)v * stride + off, sizeof x);
            for (int k = 0; k < 2; k++) lo[k] = fminf(lo[k], x[k]), hi[k] = fmaxf(hi[k], x[k]);
        }
        return 1;
    }
    return 0;
}

int vk_dlaa_scale_draw(VkExec *ex) {
    if (!dl_active() || !D.armed || !D.ended || D.done) return 0;
    const VkExecTarget *c = &ex->color[0];
    unsigned o[2], s[2];
    res_sizes(o, s);
    if (D.color.width >= o[0] || ex->pass_ncolor != 1 || !c->image || !srgb8(c->format) || c->width != o[0] ||
        c->height != o[1] || c->samples > 1 || c->is3d)
        return 0;
    uint64_t h = ex->tex[1][0];   // the fragment stage's slot 0: the picture the draw scales
    if (!h) return 0;
    view_t tv;
    int has_view = 0;
    const VkTexImage *src = vk_texture_by_id(NVN_HANDLE_TEX(h), &tv, &has_view);
    if (!src || !src->image || !srgb8(src->format) || src->width != D.color.width || src->height != D.color.height ||
        src->layers != 1 || src->samples > 1 || src->depth > 1)
        return 0;
    D.done = 1;   // one try a frame: a draw after this one is not the scaling draw
    float plo[2] = {0}, phi[2] = {0}, ulo[2] = {0}, uhi[2] = {0};
    int pos = quad_range(ex, 0x22, plo, phi), uv = quad_range(ex, 0x16, ulo, uhi);
    // The quad spans the output in pixels, or in the UI's 1920x1080 layout units when the output has another size (the
    // KTGL's projection on the output target takes the layout size then: recomp/patches-v130.txt, set output).
    int px = fabsf(phi[0] - (float)c->width) < 0.5f && fabsf(phi[1] - (float)c->height) < 0.5f;
    int lu = fabsf(phi[0] - 1920.0f) < 0.5f && fabsf(phi[1] - 1080.0f) < 0.5f;
    int corners = pos && fabsf(plo[0]) < 0.5f && fabsf(plo[1]) < 0.5f && (px || lu);
    float f = uhi[0];
    int share = uv && fabsf(ulo[0]) < 1e-3f && fabsf(ulo[1]) < 1e-3f && fabsf(uhi[1] - f) < 2e-3f && f > 0.25f &&
                f < 1.0005f;
    if (!corners || !share) {
        if (D.other++ < 4 || D.other % 600 == 0)
            rt_log("dlaa: the game's scaling draw is not a plain scale (quad%s %.0f..%.0f x %.0f..%.0f, UVs%s "
                   "%.3f..%.3f x %.3f..%.3f): the game scales that frame (%llu such frames, %llu with DLSS)",
                   pos ? "" : " unread", plo[0], phi[0], plo[1], phi[1], uv ? "" : " unread", ulo[0], uhi[0], ulo[1],
                   uhi[1], (unsigned long long)D.other, (unsigned long long)D.evals);
        if (D.jit_used) D.hold = 60, D.holds++;   // the jittered scene reaches the screen: no jitter for a second
        return 0;
    }
    uint32_t sw = (uint32_t)lroundf(f * (float)src->width), sh = (uint32_t)lroundf(uhi[1] * (float)src->height);
    if (sw > src->width) sw = src->width;
    if (sh > src->height) sh = src->height;
    if (sw != D.sub_w || sh != D.sub_h) {
        if (D.sub_logs++ < 8 || D.sub_logs % 100 == 0)
            rt_log("dlaa: the game draws the scene at %ux%u of its %ux%u (dynamic resolution %.0f %%): the upscaler takes that",
                   sw, sh, src->width, src->height, 100.0 * f);
        D.sub_w = sw, D.sub_h = sh;
    }
    VkExecTarget t = {.image = src->image, .format = src->format, .view_type = VK_IMAGE_VIEW_TYPE_2D,
                      .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .layers = 1, .width = src->width, .height = src->height,
                      .samples = 1, .img = src};
    D.up = 1;
    evaluate(ex, &t, c, sw, sh);
    ex->dirty |= VK_EXEC_DIRTY_ALL;   // NGX bound its own; the game's next draw opens the pass again
    return !D.failed;
}

int vk_dlaa_ran(void) { return D.done || D.up; }
