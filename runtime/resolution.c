// Game resolution: the size the game composes its frame in (its output: UI and swapchain images) and the size of its
// 3D scene. Age of Calamity fixes both in nnMain: the KTGL device configuration {1920, 1080, ...} it hands to
// main+0x17c34f0, and 1440x810 for the scene (3/4 of the output per side), stored right after; the operation mode
// (docked or handheld) changes neither, so on the Switch's own 720p screen the system scales the 1080p frame down.
// The output and scene patch sets (recomp/patches-v130.txt) make nnMain read both pairs from the patch page, so the
// game itself renders at the size asked for: on the Steam Deck 1280x720, shown texel for texel on the 1280x800 screen
// in place of a 1.5x reduction of a 1080p frame, at less than half the pixels.
//
// Settings, read at start: AOC_OUTPUT_RES = <w>x<h> (default 1920x1080, the game's own; 960x540 to 1920x1080: the
// texture heaps are the game's, sized for 1920x1080, and larger targets were not measured), AOC_RENDER_SCALE = the
// scene's size relative to the output: "game" (default, 3/4, the game's 1440x810 at 1080p) or a percentage up to 100.
// The game's own final passes scale the scene to the output; the present scales the output to the window.
//
// Patch page: +32 the output size and +40 the scene size, each {w, h} in one 64-bit word (width in the low half), the
// form of the constants the patched instructions built.
//
// The heap, shadow-map, DLSS, aspect and field-of-view code below came with the runtime (TOTK's resolution.c); its
// patch sets do not exist for this game, so those settings log that and change nothing.
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
#define SCENE_MAX 4096       // per side, as MacNativeGames' AOC_RENDER_RES
#define SHADOW_BASE 960      // the sun shadow map's base size (its cascades: a D16 array of 4 layers)

static struct {
    int decided;
    unsigned ow, oh, sw, sh;   // output and scene
    int scene_set;             // the scene differs from the game's 5/6 of the output
    int on;                    // res_init switched the patch sets on
    unsigned shadow;           // fixed sun shadow map size (AOC_SHADOW_RES), 0: the game's own
    double dlss_k;             // AOC_DLSS quality/balanced/performance: the scene's share of the output, else 0
} R;

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("AOC_OUTPUT_RES");
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 960 && h >= 540 && w <= BASE_W && h <= BASE_H) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: AOC_OUTPUT_RES=%s is not <width>x<height> in 960x540..1920x1080; 1920x1080", o);
    }
    const char *s = cfg_value("AOC_RENDER_SCALE");
    double k = (double)SCENE_W / BASE_W;
    const char *m = cfg_value("AOC_DLSS");   // a DLSS mode other than dlaa sets the scene size
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
            rt_log("resolution: AOC_RENDER_SCALE=%s is not game or a percentage 25..100; game", s);
        }
    }
    double sw = R.ow * k, sh = R.oh * k;
    double fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    if (fit < 1.0) R.scene_set = 1;
    R.sw = ((unsigned)(sw * fit + 0.5) + 1) & ~1u;   // even sizes: the game halves the scene for its effect targets
    R.sh = ((unsigned)(sh * fit + 0.5) + 1) & ~1u;
    // Below 1920x1080 the KTGL's 2D projection and full-screen quads take the layout size for any target as large as
    // the output (recomp/patches-v130.txt, set output): a scene of the output's size would have its own passes caught by
    // that test and drawn at 2/3 (measured 2026-11-12 with 1280x720 at 100 %). 85 % is the largest scale clear of it.
    if ((R.ow != BASE_W || R.oh != BASE_H) && R.sw == R.ow && R.sh == R.oh) {
        R.sw = ((unsigned)(R.ow * 0.85 + 0.5) + 1) & ~1u;
        R.sh = ((unsigned)(R.oh * 0.85 + 0.5) + 1) & ~1u;
        rt_log("resolution: a scene as large as an output below 1920x1080 is not supported; %ux%u (85 %%)", R.sw, R.sh);
    }
    const char *sr = cfg_value("AOC_SHADOW_RES");
    if (strcmp(sr, "game")) {
        unsigned n = (unsigned)atoi(sr);
        if (n >= 256 && n <= 4096 && !(n & 7)) R.shadow = n;
        else rt_log("resolution: AOC_SHADOW_RES=%s is not game or a size 256..4096 (a multiple of 8); game", sr);
    }
}

// Texture heap for a sun shadow map above the base size: the four layers of 2 bytes, three maps in flight (the engine
// makes the array anew each frame).
static uint64_t shadow_kib(void) {
    return R.shadow > SHADOW_BASE ? ((uint64_t)R.shadow * R.shadow - SHADOW_BASE * SHADOW_BASE) * 8 * 3 / 1024 : 0;
}

static int bigger(void) {
    decide();
    return R.ow != BASE_W || R.oh != BASE_H || R.scene_set || shadow_kib();
}

uint64_t res_heap_extra_kib(void) {
    if (!bigger()) return 0;
    double grow = fmax(0.0, (double)R.ow * R.oh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.sw * R.sh / (SCENE_W * SCENE_H) - 1.0);
    uint64_t kib = grow > 0 ? (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB : 0;
    return kib + ((shadow_kib() + 1023) & ~1023ull);   // whole MiB
}

uint64_t res_heap_extra_bytes(void) { return ((res_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

// ---- sun shadow map size: each frame the engine sizes its shadow cascades to the area they cover, down to about 400 near
// the ground, which draws foliage shadows blocky, and each change of size moves the shadow texels, so leaf shadows jump.
// A fixed size (the shadow set, recomp/patches-v121.txt) keeps them still and sharper. Page +92.
static void shadow_init(uint8_t *page) {
    if (!R.shadow) {
        rt_log("resolution: sun shadow map sized by the game (AOC_SHADOW_RES=game)");
        return;
    }
    int set = rt_patch_set("shadow");
    if (set < 0) {
        rt_log("resolution: the generated C has no shadow patch set: the game sizes its shadow map");
        return;
    }
    int32_t n = (int32_t)R.shadow;
    memcpy(page + 92, &n, 4);
    rt_patch_switch(set, 1);
    rt_log("resolution: sun shadow map %ux%u (AOC_SHADOW_RES)", R.shadow, R.shadow);
}

// ---- start: the patch page and the sets
static uint64_t pack(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

// AOC_SHADOW_RANGE (percent, 50 to 400; 100 = the game's): the sun's shadow cascades end at 20, 40, 80 and 160 m.
// Other values multiply those distances through the cascade patch set (recomp/patches-v143.txt), so the sharp first
// cascade reaches further and the visible steps between cascades move away from the player, at the cost of shadow
// texels per metre (AOC_SHADOW_SCALE pays that back).
static void res_shadow_range(void) {
    int pct = cfg_int("AOC_SHADOW_RANGE"), set = rt_patch_set("cascade");
    if (pct == 100) return;
    if (set < 0) {
        rt_log("resolution: the generated C has no cascade patch set; shadow_range stays 100");
        return;
    }
    float k = (float)(pct < 50 ? 50 : pct > 400 ? 400 : pct) / 100.0f, d[4] = {20 * k, 40 * k, 80 * k, 160 * k};
    memcpy((uint8_t *)rt_patch_page() + 64, d, sizeof d);
    rt_patch_switch(set, 1);
    rt_log("resolution: shadow cascades end at %g, %g, %g and %g m", d[0], d[1], d[2], d[3]);
}

void res_init(void) {
    res_shadow_range();
    if (!bigger()) {
        rt_log("resolution: the game's own (output 1920x1080, scene 1440x810)");
        shadow_init(rt_patch_page());
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
    double k = RT_V143 ? (double)R.ow / BASE_W : 1.0;   // 1.4.3's own scaling of the scene with the output width
    int scene_on = R.scene_set || (!RT_V143 && (R.ow != BASE_W || R.oh != BASE_H));
    float sc[2] = {(float)(R.sw / k), (float)(R.sh / k)};
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
    // Another shape than 16:9 (more than 1% off): the camera takes the output's ratio (recomp/patches-v121.txt and
    // patches-v143.txt, set aspect), so a 21:9 output shows more of the world at the sides instead of a stretched 16:9 view. The HUD's layouts
    // for that shape come from romfs-patches-<w>x<h> (romfs.c overlay_init, tools/make_romfs_patches.py --aspect).
    double ratio = (double)R.ow / R.oh, game = (double)BASE_W / BASE_H;
    int aspect = rt_patch_set("aspect");
    if (!res_is_16x9(R.ow, R.oh)) {
        if (aspect < 0) {
            rt_log("resolution: output %ux%u is not 16:9 and this game version has no camera aspect patch; the view "
                   "is stretched", R.ow, R.oh);
        } else {
            float r = (float)ratio;
            memcpy(page + 56, &r, sizeof r);
            rt_patch_switch(aspect, 1);
            rt_log("resolution: camera aspect ratio %.4f (16:9 is %.4f)", ratio, game);
        }
    } else {
        rt_patch_switch(aspect, 0);   // the page holds no ratio
    }
    shadow_init(page);
    rt_log("resolution: output %ux%u, scene %ux%u%s; texture heap +%llu KiB", R.ow, R.oh, R.sw, R.sh,
           scene_on ? "" : " (the game's own)", (unsigned long long)res_heap_extra_kib());
}

// DLSS upscaling: textures are sampled for the output's resolution, not the scene's (NVIDIA's DLSS guide: a mip bias
// of log2(render / output)), so their detail survives the scaling. 0 without a DLSS mode.
float res_dlss_lod_bias(void) {
    decide();
    return R.dlss_k ? (float)log2(R.dlss_k) : 0.0f;
}

// 16:9 within 1% (1366x768 counts): the shape the game's own layouts and camera are made for.
int res_is_16x9(unsigned w, unsigned h) {
    double rel = ((double)w / h) / ((double)BASE_W / BASE_H);
    return rel >= 0.99 && rel <= 1.01;
}

// The game resolution asks for a romfs-patches-<w>x<h> overlay: it is not 16:9 and the game version has the camera
// aspect patch (set aspect); the UI layouts of another shape only fit a camera that follows the same shape.
int res_wants_wide_overlay(const unsigned out[2]) {
    return !res_is_16x9(out[0], out[1]) && rt_patch_set("aspect") >= 0;
}

// Field of view (AOC_FOV, percent of the game's own, 70..140, live): the camera's vertical view angle times the
// factor on page +60 (recomp/patches-v143.txt, set fov). The horizontal angle follows through the aspect ratio. 100
// switches the set off, so the camera code is the game's own. Runs on a settings change (gfx_present.c
// window_settings_live), the first time at the first present.
#define FOV_MIN 70
#define FOV_MAX 140
void res_fov_live(void) {
    static int set = -2, shown = 100;
    if (set == -2) set = rt_patch_set("fov");
    int pct = cfg_int("AOC_FOV");
    pct = pct < FOV_MIN ? FOV_MIN : pct > FOV_MAX ? FOV_MAX : pct;
    if (pct == shown) return;
    shown = pct;
    if (set < 0) {
        rt_log("resolution: this game version has no field-of-view patch; AOC_FOV=%d does nothing", pct);
        return;
    }
    float k = pct / 100.0f;
    memcpy((uint8_t *)rt_patch_page() + 60, &k, sizeof k);   // before the switch: a site that sees it on reads k
    rt_patch_switch(set, pct != 100);
    rt_log("resolution: field of view %d%% of the game's", pct);
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
