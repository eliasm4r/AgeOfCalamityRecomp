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
#define STEP_RESET_S 0.25          // a gap this long (loading) restarts the average

static double step_now;   // the step the game used this frame (whole vsyncs: interval / 60)
static uint64_t last_ns;

#define FPS_BLOCK 0x700269e150ull   // main+0x269e150

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
        // TotK's frame_rate.c: the mean of the last 5 display intervals, restarted after a gap (a load), clamped
        static double samples[STEP_SAMPLES];
        static unsigned count, next;
        if (dt <= 0 || dt > STEP_RESET_S) {
            count = next = 0;
            dt = 1.0 / fps;
        }
        samples[next] = dt, next = (next + 1) % STEP_SAMPLES;
        if (count < STEP_SAMPLES) count++;
        double sum = 0;
        for (unsigned k = 0; k < count; k++) sum += samples[k];
        vs = sum / count * 60.0;
        if (vs < 60.0 / FPS_MAX) vs = 60.0 / FPS_MAX;
        if (vs > STEP_MAX_S * 60.0) vs = STEP_MAX_S * 60.0;
        acc += vs;
        n = (int)acc;
        acc -= n;
    } else {
        acc = 0;
    }
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
}

// ---- ACR_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static double win_game;

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    double wall = (double)(now - win_t0) * 1e-9;
    rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu",
           (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
           (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3]);
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

void frame_step_present(uint64_t display_ns, int interval) {
    stepscan_present();
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
