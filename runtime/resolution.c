// Game resolution: the size the game composes its frame in (its output: UI and swapchain images) and the size of its
// 3D scene. Age of Calamity fixes both in nnMain: the KTGL device configuration {1920, 1080, ...} it hands to
// main+0x17c34f0, and 1440x810 for the scene (3/4 of the output per side), stored right after; the operation mode
// (docked or handheld) changes neither, so on the Switch's own 720p screen the system scales the 1080p frame down.
// The output and scene patch sets (recomp/patches-v130.txt) make nnMain read both pairs from the patch page, so the
// game itself renders at the size asked for: on the Steam Deck 1280x720, shown texel for texel on the 1280x800 screen
// in place of a 1.5x reduction of a 1080p frame, at less than half the pixels.
//
// Settings, read at start: ACR_OUTPUT_RES = <w>x<h> (default 1920x1080, the game's own; 960x540 to 3840x2160: above
// 1920x1080 the window's images take the output's size too, set output, and the HUD is drawn at that size from its
// 1920x1080 layout), ACR_RENDER_SCALE = the scene's size relative to the output: "game" (default, 3/4, the game's
// 1440x810 at 1080p) or a percentage up to 200, the scene keeping at most 3840x2160 pixels (SCENE_PIXELS).
// Above 100 the scene is larger than the output (supersampling), and the game's final passes scale it down. The game's
// GPU texture pool is a fixed 1510 MiB in a GPU heap of fixed size (set texpool below): at 125 and 140 % the larger
// targets filled it and a story cutscene of the first battle waited for memory forever (0.1.1 capped the scale at 100),
// so both grow with the scene. The same pool stopped 148 % and more as the second intro movie started (main+0x18d3040
// got no memory for a texture; the first battle at 150 % takes up to 1568 MiB of it). The heap's 215 MiB pool of GPU
// buffers stopped 175 % (3360x1890) on loading a save (the movie player main+0x151daf4 got a null buffer and passed it
// to main+0x17bf930; 200 % stopped 18 s into the first battle on a null write): it holds 167 MiB at the game's scene
// and 15.6 MiB more per million scene pixels (278 MiB at 3840x2160; measured 2026-11-21 by the end of its furthest
// buffer or texture), so it grows by 16 MiB per million pixels over the game's 1440x810, which keeps its own 48 MiB
// margin. With both pools grown, 175 and 200 % play a new game's first battle and load a save, and so does a
// 3840x2160 output at 100 % (2026-11-21; 200 % ran at 47 fps on an RTX 4070 with a 60 fps limit).
// A scene larger than 3840x2160 was never measured: it is brought back to that many pixels (SCENE_PIXELS). The
// recorded first battle at 140 % held 59.8 fps with DLAA, GPU 7 ms on an RTX 4070; 200 % draws twice the pixels of 140.
// The game's own final passes scale the scene to the output; the present scales the output to the window. A scene
// larger than the output switches on the hold set too: the game copies its held picture (the loading after "New game")
// into a target of the scene's size with a rectangle of the output's, which left the rest of that target as the memory
// was, an unlit picture of the last scene at 150 and 200 %.
//
// Patch page: +32 the output size and +40 the scene size, each {w, h} in one 64-bit word (width in the low half), the
// form of the constants the patched instructions built; +112 the texture pool's size, +116 the GPU heap's and +128 the
// buffer pool's (32 bits).
//
// The heap and DLSS code below came with the runtime (TOTK's resolution.c).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

#define BASE_W 1920
#define BASE_H 1080
#define SCENE_W 1440
#define SCENE_H 810
#define DTA_KIB 113664u      // DynamicTextureAllocatorMem, shipped
#define SCENE_MAX 4096       // per side, as MacNativeGames' ACR_RENDER_RES
#define SCENE_PIXELS (3840.0 * 2160.0)   // the largest scene measured (header)
#define OUT_MAX_W 3840
#define OUT_MAX_H 2160

static struct {
    int decided;
    unsigned ow, oh, sw, sh;   // output and scene
    int scene_set;             // the scene differs from the game's 5/6 of the output
    int on;                    // res_init switched the patch sets on
    double dlss_k;             // ACR_DLSS quality/balanced/performance: the scene's share of the output, else 0
} R;

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("ACR_OUTPUT_RES");
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 960 && h >= 540 && w <= OUT_MAX_W && h <= OUT_MAX_H) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: ACR_OUTPUT_RES=%s is not <width>x<height> in 960x540..3840x2160; 1920x1080", o);
    }
    const char *s = cfg_value("ACR_RENDER_SCALE");
    double k = (double)SCENE_W / BASE_W;
    const char *m = cfg_value("ACR_DLSS");   // a DLSS mode other than dlaa sets the scene size
    static const struct { const char *name; double k; } dlss[] = {
        {"quality", 2.0 / 3.0}, {"balanced", 0.58}, {"performance", 0.5}};
    for (int i = 0; i < 3; i++)
        if (!strcmp(m, dlss[i].name)) R.dlss_k = dlss[i].k;
    if (R.dlss_k) {
        k = R.dlss_k;
        R.scene_set = 1;
        rt_log("resolution: DLSS %s: the scene at %.0f %% of the output (render_scale ignored)", m, k * 100);
    } else if (strcmp(s, "game")) {
        char *end;
        double pct = strtod(s, &end);
        if (end != s && !*end && pct >= 25 && pct <= 200) {
            k = pct / 100.0;
            R.scene_set = 1;
        } else {
            rt_log("resolution: ACR_RENDER_SCALE=%s is not game or a percentage 25..200; game", s);
        }
    }
    double sw = R.ow * k, sh = R.oh * k;
    double fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    if (fit < 1.0) R.scene_set = 1;
    R.sw = ((unsigned)(sw * fit + 0.5) + 1) & ~1u;   // even sizes: the game halves the scene for its effect targets
    R.sh = ((unsigned)(sh * fit + 0.5) + 1) & ~1u;
    if ((double)R.sw * R.sh > SCENE_PIXELS) {   // the same shape at the largest size that plays
        double f = sqrt(SCENE_PIXELS / ((double)R.sw * R.sh));
        R.sw = (unsigned)(R.sw * f) & ~1u;
        R.sh = (unsigned)(R.sh * f) & ~1u;
        R.scene_set = 1;
        rt_log("resolution: a scene larger than 3840x2160 was never measured; %ux%u", R.sw, R.sh);
    }
    // Below 1920x1080 the KTGL's 2D projection and full-screen quads take the layout size for any target as large as
    // the output (recomp/patches-v130.txt, set output): a scene of the output's size would have its own passes caught by
    // that test and drawn at 2/3 (measured 2026-11-12 with 1280x720 at 100 %). 85 % is the largest scale clear of it;
    // a larger scene (supersampling) is held there too, as it was never measured below 1920x1080.
    if ((R.ow < BASE_W || R.oh < BASE_H) && R.sw >= R.ow && R.sh >= R.oh) {
        R.sw = ((unsigned)(R.ow * 0.85 + 0.5) + 1) & ~1u;
        R.sh = ((unsigned)(R.oh * 0.85 + 0.5) + 1) & ~1u;
        rt_log("resolution: a scene as large as an output below 1920x1080 is not supported; %ux%u (85 %%)", R.sw, R.sh);
    }
    // Above 1920x1080 the test is the same: a target whose width (or height) equals the output's takes the layout's
    // 1920 (or 1080) for that axis. A scene of exactly the output's size (100 %) played battles whole, but the passes of
    // in-engine cutscenes drew in its top left 1920x1080 twice over: at 2560x1440 the screen showed the top left
    // 1440x810 of the picture stretched (0.5625 of each side, matched shot for shot against 1920x1080, 2026-11-21).
    // Such a scene is drawn 2 pixels smaller on that axis (2558x1438 at 2560x1440), which no test takes for the output;
    // the game's scaling draw, or DLSS with DLAA on, brings it to the output (2556x1438: the cutscene whole).
    if (R.ow > BASE_W || R.oh > BASE_H) {
        int w = R.sw == R.ow, h = R.sh == R.oh;
        if (w) R.sw -= 2;
        if (h) R.sh -= 2;
        if (w || h) rt_log("resolution: a scene of the output's size draws at %ux%u (the output test, above)", R.sw, R.sh);
    }
}

static int bigger(void) {
    decide();
    return R.ow != BASE_W || R.oh != BASE_H || R.scene_set;
}

uint64_t res_heap_extra_kib(void) {
    if (!bigger()) return 0;
    double grow = fmax(0.0, (double)R.ow * R.oh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.sw * R.sh / (SCENE_W * SCENE_H) - 1.0);
    return grow > 0 ? (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB : 0;   // whole MiB
}

uint64_t res_heap_extra_bytes(void) { return ((res_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

// The game's texture pool and the GPU heap that holds it (recomp/patches-v130.txt, set texpool): 1510 MiB in a heap of
// exactly the three pools. A scene larger than the game's grows both by the targets' growth (res_heap_extra_bytes),
// the growth mem_init gives the application pool: the rest of the game's heap keeps its own size. (Counted twice in the
// application pool, 175 % asked for more than the 4 GiB heap and the SDK stopped at SetHeapSize.)
#define TEXPOOL_SIZE 0x5e600000u
#define GPU_HEAP_SIZE 0x72204008u
static int texpool_off(void) {
    const char *e = cfg_get("ACR_PATCH_TEXPOOL");   // =0: the game's own sizes
    return e && !strcmp(e, "0");
}
uint64_t res_texpool_extra_bytes(void) { return texpool_off() ? 0 : res_heap_extra_bytes(); }

// The pool of GPU buffers (+128): 215 MiB, grown by 16 MiB per million scene pixels over the game's 1440x810 (header),
// in 2 MiB steps. mem_init adds it to the application pool and res_init to the heap.
#define BUFFER_POOL_SIZE 0xd700000u
uint64_t res_buffer_pool_extra_bytes(void) {
    if (!bigger() || texpool_off()) return 0;
    double px = (double)R.sw * R.sh - (double)SCENE_W * SCENE_H;
    return px > 0 ? ((uint64_t)ceil(16.0 * px / 1e6 / 2.0) * 2) << 20 : 0;
}

// ---- start: the patch page and the sets
static uint64_t pack(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

void res_init(void) {
    if (!bigger()) {
        rt_log("resolution: the game's own (output 1920x1080, scene 1440x810)");
        return;
    }
    int output = rt_patch_set("output"), scene = rt_patch_set("scene");
    if (output < 0 || scene < 0) {
        rt_log("resolution: the generated C has no output and scene patch sets; the game keeps its own resolution");
        return;
    }
    // Age of Calamity has no HeapDef table to grow: the TotK runtime this file came from needed one and kept the game's
    // own resolution without it, which left every scene above 3/4 at 1440x810. The KTGL takes its targets from the
    // application pool, which mem_init grows by res_heap_extra_bytes.
    uint8_t *page = rt_patch_page();
    int scene_on = R.scene_set || R.ow != BASE_W || R.oh != BASE_H;
    float sc[2] = {(float)R.sw, (float)R.sh};
    uint64_t out = pack(R.ow, R.oh), scn = pack(R.sw, R.sh);
    memcpy(page + 16, sc, sizeof sc);
    memcpy(page + 32, &out, 8);
    memcpy(page + 40, &scn, 8);
    memcpy(page + 48, &out, 8);
    // +104: the UI's layout size, 1920x1080: the KTGL's 2D projection and full-screen quad take it in place of the size
    // of a target as large as the output (recomp/patches-v130.txt, the output sites in main+0x17def58 and on)
    uint64_t layout = pack(BASE_W, BASE_H);
    memcpy(page + 104, &layout, 8);
    // +120: the window's images, the output's size when it is larger than the game's 1920x1080, else that
    uint64_t win = R.ow > BASE_W && R.oh > BASE_H ? out : layout;
    memcpy(page + 120, &win, 8);
    rt_patch_switch(output, R.ow != BASE_W || R.oh != BASE_H);
    rt_patch_switch(scene, scene_on);
    // The held picture covers a target larger than the output (recomp/patches-v130.txt, set hold; ACR_PATCH_HOLD=0
    // turns it off)
    const char *h = cfg_get("ACR_PATCH_HOLD");
    if (!(h && !strcmp(h, "0"))) rt_patch_switch(rt_patch_set("hold"), R.sw > R.ow || R.sh > R.oh);
    int texpool = rt_patch_set("texpool");
    uint64_t grow = res_texpool_extra_bytes(), bgrow = res_buffer_pool_extra_bytes();
    if (texpool >= 0 && (grow || bgrow)) {
        uint32_t sizes[2] = {TEXPOOL_SIZE + (uint32_t)grow, GPU_HEAP_SIZE + (uint32_t)(grow + bgrow)};
        uint32_t buffers = BUFFER_POOL_SIZE + (uint32_t)bgrow;
        memcpy(page + 112, sizes, sizeof sizes);
        memcpy(page + 128, &buffers, sizeof buffers);
        rt_patch_switch(texpool, 1);
        rt_log("resolution: the game's texture pool grows by %llu MiB to %u MiB, its buffer pool by %llu MiB to %u MiB",
               (unsigned long long)(grow >> 20), sizes[0] >> 20, (unsigned long long)(bgrow >> 20), buffers >> 20);
    } else if (grow || bgrow) {
        rt_log("resolution: the generated C has no texpool patch set; the texture pool keeps its 1510 MiB");
    }
    R.on = 1;
    rt_log("resolution: output %ux%u, scene %ux%u%s; texture heap +%llu KiB", R.ow, R.oh, R.sw, R.sh,
           scene_on ? "" : " (the game's own)", (unsigned long long)res_heap_extra_kib());
}

// DLSS upscaling: textures are sampled for the output's resolution, not the scene's (NVIDIA's DLSS guide: a mip bias
// of log2(render / output)), so their detail survives the scaling. 0 without a DLSS mode.
float res_dlss_lod_bias(void) {
    decide();
    return R.dlss_k ? (float)log2(R.dlss_k) : 0.0f;
}

void res_wanted(unsigned out[2]) {
    decide();
    out[0] = R.ow, out[1] = R.oh;
}

void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    scene[0] = R.on ? R.sw : SCENE_W, scene[1] = R.on ? R.sh : SCENE_H;
}
