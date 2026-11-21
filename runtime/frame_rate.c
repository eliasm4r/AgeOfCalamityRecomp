// Frame rate: 30 (the game's own) or 60 fps, the world clock, and ACR_TRACE=perf speed lines. The world clock and the
// speed lines come from the TotK port (MacNativeGames games/totk/runtime/game_patches.c @ ebe621d, MIT, Copyright (c)
// 2026 MacNativeGames contributors).
//
// 60 fps: Age of Calamity keeps its frame rate in one block at main+0x269e150: {fps, vsyncs per update, fps, fps as a
// float}, {30, 2, 30, 30.0} as shipped. Its own setter (main+0x8a35a0 area) writes 60/n, n, 60/n, 60/n from a request
// (n << 4) | 1, and the game turns frame counts into seconds through it (frames / the float) and steps its world by
// the vsyncs per update. So the game runs at 60 with the block at {60, 1, 60, 60.0} and the window presenting every
// vsync instead of every second one. Measured (scripted 3 s run in the Hyrule Field battle, shots each second of world
// time): the same distance covered and the same dialogue timing at 30 and at 60 fps. The block is written at every
// present (the game's setter may write it back), and the present interval the game sets on its window (2) is replaced
// by the one the setting asks for (frame_step_interval). 40 or 45 fps cannot be had this way (whole vsyncs per
// update); 120, 144 and 240 can, with the real-time step below (fps_block).
//
// The world clock (frame_step_world_us): wall time from the first present. Recorded and scripted input (input.c) runs
// on it, so a replay presses at the same game time at any frame rate.
//
// ACR_TRACE=perf adds a line per 120 presents: fps and the game seconds stepped against wall seconds.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_SAMPLES 5
#define UPD_SAMPLES 32             // presents over which the updates per present are counted (above 60 fps)
#define STEP_RESET_S 0.25          // a gap this long (loading) restarts the average

static double step_now;   // the step the game used this frame (whole vsyncs: interval / 60)
// ACR_TRACE=perf: game seconds the updates stepped by the float vsyncs (fps_block): each update since the last present
// read the vsyncs written then
static double upd_game_s, upd_vs_last;
static uint32_t upd_seen;
static uint64_t last_ns;

#define FPS_BLOCK 0x700269e150ull   // main+0x269e150
// main+0x2780448: the game's update count, +1 per update of its update thread (main+0x166d980; found with
// runtime/stepscan.c). The game updates on that thread at the pace of its rate fields, apart from the presents.
#define GAME_UPDATES 0x7002780448ull

// ACR_FPS_LIMIT: 30, or 60 to 240 (one game update per frame, see SPEED_SCALE). "display" gives 60, other rates below
// 60 give 30.
#define FPS_MAX 240
static int fps_hz = -1;
static unsigned fps_seen;
static int fps_forced;   // frame_step_force_hz: a rate the step scan (stepscan.c) holds over the setting, 0 none

static int fps_parse(void) {
    if (fps_forced) return fps_forced;
    const char *e = cfg_get("ACR_FPS_LIMIT");
    if (!e || !*e) return 30;
    if (!strcmp(e, "display")) return 60;
    char *end;
    long n = strtol(e, &end, 10);
    if (*end || n <= 0) {
        rt_log("frame rate: ACR_FPS_LIMIT=%s is not a frame rate; 30", e);
        return 30;
    }
    return n >= FPS_MAX ? FPS_MAX : n >= 60 ? (int)n : 30;
}

static void fps_live(void) {
    unsigned v = cfg_version();
    if (fps_hz >= 0 && v == fps_seen) return;
    fps_seen = v;
    int hz = fps_parse();
    if (hz == fps_hz) return;
    fps_hz = hz;
    rt_log("frame rate: %d fps", hz);
}

void frame_step_init(void) { fps_live(); }

void frame_step_force_hz(int hz) {
    fps_forced = hz;
    fps_hz = -1;   // fps_live reads the rate again
    fps_live();
}

int frame_step_interval(void) {
    fps_live();
    return fps_hz >= 60 ? 1 : 2;
}

uint64_t frame_step_period_ns(void) {
    fps_live();
    return 1000000000ull / (uint64_t)fps_hz;
}

double frame_step_limit_hz(void) { return fps_hz < 0 ? 0 : fps_hz; }

// The block to the setting's rate, at every present. 30 and 60: the game's own whole steps, {30, 2} or {60, 1}. Above
// 60, as TotK's real-time step: the game steps by the frame's real time, in 60 Hz vsyncs (the last frame's, clamped).
// The float readers of the vsyncs read it as 16.16 fixed point from +0x158 (recomp/patches-v130.txt, set vsync); the
// integer readers get whole vsyncs from +0x154 that add up to the same time (1, 0, 1, 0... at 120 fps). The rate
// fields hold the setting's rate: the game paces itself by them (it holds 60 when they say 60).
static void fps_block(double dt) {
    static double acc;   // vsyncs stepped by the float readers that the integer ones have not had yet
    int n = frame_step_interval(), fps = n == 2 ? 30 : fps_hz;
    double vs = n;
    if (fps > 60) {
        // As TotK's frame_rate.c, the real time of the last presents, restarted after a gap (a load), clamped; but per
        // game update, not per present: the game updates at the pace of its rate fields (the setting), so below the
        // limit it makes more updates than presents (240 set, 120 shown: 230 updates a second), and a present's time
        // stepped by each of them ran the whole game about twice as fast (issue #1; measured 1.91 at 240). The step
        // is the time of the last UPD_SAMPLES presents over the updates made in it (over 5 it came out 2 % long: the
        // update count is 1, 2 or 3 a present). Each value written here is read by the updates until the next
        // present, so the integer vsyncs below still add up to the time on average.
        static double samples[UPD_SAMPLES];
        static uint32_t upd_samples[UPD_SAMPLES], upd_last;
        static unsigned count, next;
        uint32_t upd = *(volatile uint32_t *)(uintptr_t)GAME_UPDATES, k = upd - upd_last;
        upd_last = upd;
        if (dt <= 0 || dt > STEP_RESET_S || k > 16) {
            count = next = 0;
            dt = 1.0 / fps;
            k = 1;
        }
        samples[next] = dt, upd_samples[next] = k, next = (next + 1) % UPD_SAMPLES;
        if (count < UPD_SAMPLES) count++;
        // ACR_STEP_BY_PRESENT=1: the former step, the mean of the last 5 presents' time per update (diagnosis)
        static int by_present = -1;
        if (by_present < 0) by_present = cfg_get("ACR_STEP_BY_PRESENT") && !strcmp(cfg_get("ACR_STEP_BY_PRESENT"), "1");
        double sum = 0;
        uint32_t ups = 0;
        unsigned use = by_present && count > STEP_SAMPLES ? STEP_SAMPLES : count;
        for (unsigned i = 0; i < use; i++) {
            unsigned j = (next + UPD_SAMPLES - 1 - i) % UPD_SAMPLES;   // the newest first
            sum += samples[j], ups += upd_samples[j];
        }
        vs = sum * 60.0 / (by_present ? use : ups ? ups : use);
        if (vs < 60.0 / FPS_MAX) vs = 60.0 / FPS_MAX;
        if (vs > STEP_MAX_S * 60.0) vs = STEP_MAX_S * 60.0;
        acc += vs;
        n = (int)acc;
        acc -= n;
    } else {
        acc = 0;
    }
    uint32_t now_upd = *(volatile uint32_t *)(uintptr_t)GAME_UPDATES;
    if (now_upd - upd_seen < 64) upd_game_s += (now_upd - upd_seen) * upd_vs_last / 60.0;
    upd_seen = now_upd, upd_vs_last = vs;
    uint32_t *b = (uint32_t *)(uintptr_t)FPS_BLOCK;
    if (b[0] != (uint32_t)fps) {
        static int logs;
        if (logs++ < 8 || logs % 100 == 0) rt_log("frame rate: the game's block %u %u -> %d", b[0], b[1], fps);
    }
    // Both rates hold the setting's: the game paces itself by them. The float at the real rate locked a game at 15 fps
    // after a slow load (it paced to 15, so the rate stayed 15).
    float f = (float)fps;
    b[0] = (uint32_t)fps, b[1] = (uint32_t)n;
    static int set = -2;
    if (set == -2) set = rt_patch_set("vsync");
    b[2] = rt_patch_enabled(set) ? (uint32_t)(vs * 65536.0 + 0.5) : (uint32_t)fps;
    memcpy(&b[3], &f, sizeof f);
    // patch page +0: the 30 Hz ratio, the vsyncs of this update over the game's own 2 (set cursor: code that steps by a
    // fixed amount per update, recomp/patches-v130.txt)
    // patch page +4: the 30 Hz frames this update steps, the vsyncs over 2 carried from frame to frame (set event: the
    // cutscene clock counts 30 Hz frames): 1 at 30 fps, 0 and 1 in turn at 60
    static double acc30;
    uint8_t *page = rt_patch_page();
    if (page) {
        float ratio = (float)(vs / 2.0);
        memcpy(page, &ratio, sizeof ratio);
        acc30 += vs / 2.0;
        int32_t step30 = (int32_t)acc30;
        acc30 -= step30;
        memcpy(page + 4, &step30, sizeof step30);
    }
}

// ---- ACR_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static uint32_t win_upd0;
static double win_upd_game0;
static double win_game;

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        win_upd0 = *(volatile uint32_t *)(uintptr_t)GAME_UPDATES;
        win_upd_game0 = upd_game_s;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    double wall = (double)(now - win_t0) * 1e-9;
    uint32_t upd = *(volatile uint32_t *)(uintptr_t)GAME_UPDATES;
    rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu; "
           "%u game updates (%.1f a second), %.3f s of game time by them (speed %.3f)",
           (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
           (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3],
           upd - win_upd0, (upd - win_upd0) / wall, upd_game_s - win_upd_game0, (upd_game_s - win_upd_game0) / wall);
    win_upd0 = upd;
    win_upd_game0 = upd_game_s;
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;   // game time stepped since the first present (frame_step_ticks)
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started",
               (unsigned long long)presents,
               rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    step = dt > STEP_MAX_S ? STEP_MAX_S : dt;   // wall time
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

// ACR_FPS_AT=<world seconds>:<fps> (test runs): the frame rate changes to <fps> at that world time, so a scripted run
// can go through its menus at one rate and measure a scene at another.
static void fps_at(void) {
    static int state;   // 0 not read, 1 waiting, 2 done or off
    static double at;
    static int hz;
    if (state == 0) {
        const char *e = cfg_get("ACR_FPS_AT");
        state = e && sscanf(e, "%lf:%d", &at, &hz) == 2 && hz >= 30 ? 1 : 2;
    }
    if (state == 1 && frame_step_world_us() >= (int64_t)(at * 1e6)) {
        state = 2;
        rt_log("frame rate: ACR_FPS_AT: %d fps from %.1f s of world time", hz, at);
        frame_step_force_hz(hz);
    }
}

void frame_step_present(uint64_t display_ns, int interval) {
    stepscan_present();
    fps_at();
    double dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    fps_block(dt);
    if (interval > 0) step_now = interval == 1 && fps_hz > 60 ? (dt > 0 ? dt : 1.0 / fps_hz) : interval / 60.0;
    last_ns = display_ns;
    // The game clock of untimed script lines: what the game stepped this frame, whole vsyncs.
    double game = step_now;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, game);
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, game, interval);
}
