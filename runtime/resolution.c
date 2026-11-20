// Game resolution: the size the game composes its frame in (its output: UI and swapchain images) and the size of its
// 3D scene. Age of Calamity fixes both in nnMain: the KTGL device configuration {1920, 1080, ...} it hands to
// main+0x17c34f0, and 1440x810 for the scene (3/4 of the output per side), stored right after; the operation mode
// (docked or handheld) changes neither, so on the Switch's own 720p screen the system scales the 1080p frame down.
// The output and scene patch sets (recomp/patches-v130.txt) make nnMain read both pairs from the patch page, so the
// game itself renders at the size asked for: on the Steam Deck 1280x720, shown texel for texel on the 1280x800 screen
// in place of a 1.5x reduction of a 1080p frame, at less than half the pixels.
//
// Settings, read at start: ACR_OUTPUT_RES = <w>x<h> (default 1920x1080, the game's own; 960x540 to 1920x1080: the
// texture heaps are the game's, sized for 1920x1080, and larger targets were not measured), ACR_RENDER_SCALE = the
// scene's size relative to the output: "game" (default, 3/4, the game's 1440x810 at 1080p) or a percentage up to 100.
// Above 100 (supersampling) the first cutscene of the story never finishes loading (125 and 140 %, 2026-11-20): the
// game's GPU texture pool is a fixed 1510 MiB (heap configuration main+0x8aecf8) and the larger targets fill it; growing
// that pool alone makes nnMain give up at start (its storage comes from another fixed game heap). Capped at 100 until
// that heap is grown. Before that cutscene was found, 145 % was the
// largest that reached the battle (2026-11-20, the same route from the title screen): from 148 % on, a resource the
// largest that reached the battle (2026-11-20, the same route from the title screen): from 148 % on, a resource the
// game loads as the second intro movie starts gets no memory (main+0x18d3040: its linear allocator is full and the
// fallback allocator returns null; main+0x191cd7c reads it), whatever the size of the application pool (4 GiB tried):
// one of the game's own fixed heaps holds something sized from the scene. 140 % keeps a margin (the recorded first
// battle at 1080p held 59.8 fps with DLAA, GPU 7 ms on an RTX 4070). At 200 the game crashed on leaving the title
// screen (a null write in main+0x1151520).
// The game's own final passes scale the scene to the output; the present scales the output to the window.
//
// Patch page: +32 the output size and +40 the scene size, each {w, h} in one 64-bit word (width in the low half), the
// form of the constants the patched instructions built.
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
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 960 && h >= 540 && w <= BASE_W && h <= BASE_H) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: ACR_OUTPUT_RES=%s is not <width>x<height> in 960x540..1920x1080; 1920x1080", o);
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
        if (end != s && !*end && pct >= 25 && pct <= 100) {
            k = pct / 100.0;
            R.scene_set = 1;
        } else {
            rt_log("resolution: ACR_RENDER_SCALE=%s is not game or a percentage 25..100; game", s);
        }
    }
    double sw = R.ow * k, sh = R.oh * k;
    double fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    if (fit < 1.0) R.scene_set = 1;
    R.sw = ((unsigned)(sw * fit + 0.5) + 1) & ~1u;   // even sizes: the game halves the scene for its effect targets
    R.sh = ((unsigned)(sh * fit + 0.5) + 1) & ~1u;
    // Below 1920x1080 the KTGL's 2D projection and full-screen quads take the layout size for any target as large as
    // the output (recomp/patches-v130.txt, set output): a scene of the output's size would have its own passes caught by
    // that test and drawn at 2/3 (measured 2026-11-12 with 1280x720 at 100 %). 85 % is the largest scale clear of it;
    // a larger scene (supersampling) is held there too, as it was never measured below 1920x1080.
    if ((R.ow != BASE_W || R.oh != BASE_H) && R.sw >= R.ow && R.sh >= R.oh) {
        R.sw = ((unsigned)(R.ow * 0.85 + 0.5) + 1) & ~1u;
        R.sh = ((unsigned)(R.oh * 0.85 + 0.5) + 1) & ~1u;
        rt_log("resolution: a scene as large as an output below 1920x1080 is not supported; %ux%u (85 %%)", R.sw, R.sh);
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
    rt_patch_switch(output, R.ow != BASE_W || R.oh != BASE_H);
    rt_patch_switch(scene, scene_on);
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
