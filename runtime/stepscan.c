// Step scan (diagnosis): finds the guest words that advance by the same amount per game frame at 30 and at 60 fps.
// At 60 fps the game makes twice as many updates per second; a counter that follows the frame rate block (frame_rate.c)
// advances half as much per frame at 60 as at 30, one that steps a fixed amount per frame (cutscenes and the map ran
// twice as fast at 60) advances as much at both rates. Those are the ones to trace back to the code that steps them
// (the TotK port fixed its map cursor the same way: patches-v121.txt set cursor, the step times the 30 Hz ratio).
//
// While it runs the left stick is held at 75 % to the right (the map cursor's step is one of the ones to find).
// Trigger: F9 while the game window has focus, or ACR_STEPSCAN=<world seconds> once (scripted runs). The scan sets the
// frame rate to 30 (frame_step_force_hz), waits SETTLE presents, then walks the scanned memory in slices of SLICE_CH
// 64 KiB pieces, one slice a present: slice t is copied at present t, compared at t + K (words that changed by a
// plausible step are kept) and checked at t + 2K (kept if they changed by the same step again). Then the same at 60,
// and the setting comes back.
// Scanned memory: the writable guest memory that is in no NVN memory pool (vk_mem_class_of; pools hold textures and
// GPU buffers, read slowly from the CPU, and are most of the 3.5 GiB) and that the game wrote at least once (the
// write-watch bits, read without resetting them: the Vulkan back end owns the resets). A whole-memory scan made presents
// of seconds in the user's test (2026-11-21).
// Then the write trap: the pages of the FIXED words (up to 32) turn read-only for TRAP_PRESENTS presents
// (platform.h plat_trap_set) and every write to one of those words is logged once per writing instruction with the host
// address: `nm AgeOfCalamityRecomp.exe` names the enclosing recompiled function f_<guest address>.
// Then the stick track: the words that changed during the passes are read at fixed points while the stick is held right
// for TRACK_SEG presents, then left as long, at 30 and at 60 fps. A word the stick drives (the map cursor) moves one
// way, then back by as much; its move per frame at 60 over the one at 30 is 1 when it steps a fixed amount per frame
// (2x speed at 60), 0.5 when it follows the rate. A smoothed or accelerating cursor fails the passes' constant-step test
// (the user's test 7: the cursor was 2x by eye and the passes did not find it); this one does not need a constant step.
// The trap then watches the stick-driven words (those with ratio near 1 first). File: stick-<present>.txt.
// ACR_STEPSCAN_RATES=a,b compares two other rates (60,120: what runs fast above 60).
// ACR_STEPSCAN_TARGETS=<hex>,<hex>... skips the passes and traps those words at once (writes, or reads with
// ACR_STEPSCAN_READS=1): a second run of the same script names the code that steps the words the first one found.
// Results: <log dir>/stepscan-<present>.txt (every word found at both rates: its step per frame at 30 and 60 and the
// ratio) and the first FIXED words in the log. Ratio 1.0 = fixed step per frame (suspect), 0.5 = follows the rate.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "vk.h"
#include "platform/platform.h"

#define SETTLE 45              // presents after a rate change before the first slice
#define K 10                   // presents between a slice's copy, compare and check
#define CHUNK 0x10000ull       // the scan's unit: 64 KiB
#define SLICE_CH 128u          // pieces per present (8 MiB)
#define SLOW_NS 300000ull      // a piece slower than this to copy (about 200 MiB/s): it and the next 31 are left out
#define MAX_CAND (8u << 20)
#define MAX_RES (2u << 20)

typedef struct { uint64_t addr; uint32_t a, b; } Cand;
typedef struct { uint64_t addr; uint32_t kind; float step; uint32_t value; } Res;   // kind 1 int, 2 float

static uint64_t *chunks;        // guest addresses of the scanned pieces, ascending
static uint32_t nchunks, nslices;
static uint8_t **copies;        // per slice: its copy, freed after the compare
static uint8_t **okmap;         // per slice: per piece 1 when copied
static Cand *cand;              // appended in slice order by the compares, consumed in that order by the checks
static uint32_t ncand, cand_head, *cand_end;   // cand_end[s]: end of slice s's candidates
static Res *res[2];
static uint32_t nres[2];
static uint64_t slow_bytes, skip_until;

#define TRAP_PRESENTS 150
#define TRAP_HITS 256
static uint64_t targets[64];
static int ntargets, trap_left, trap_reads;
static int stick_on;   // the scan holds the left stick (ACR_STEPSCAN_STICK=0: no)   // trap_reads: the trap watches reads (the stick words)
static uint64_t hit_t[TRAP_HITS], hit_ip[TRAP_HITS];
static volatile uint32_t hit_n[TRAP_HITS];
static volatile int32_t nhits, hits_logged, hit_lock;
static volatile uint64_t other_writes;

static int state;               // 0 idle, 1 settling, 2 walking, 3 stick track

#define TRACK_SEG 40            // presents per stick direction
#define TRACK_MAX (1u << 20)
static uint64_t *trk;           // tracked addresses, ascending
static uint32_t ntrk;
static uint32_t *trk_v[2][6];   // per rate: values at right +10, +20, +30, left +10, +20, +30
static int trk_rate;
static uint64_t trk_t0;
static int rate_i;              // 0: the first rate, 1: the second
// ACR_STEPSCAN_RATES=<a>,<b>: the two rates compared (default 30,60; 60,120 finds what runs fast above 60). A word
// that follows the rate steps a/b as much per frame at b; a fixed step per frame keeps the ratio 1.
static int rate_hz[2] = {30, 60};
static double follow_ratio(void) { return (double)rate_hz[0] / rate_hz[1]; }
static void rates_read(void) {
    const char *e = cfg_get("ACR_STEPSCAN_RATES");
    int a, b;
    if (e && sscanf(e, "%d,%d", &a, &b) == 2 && a >= 30 && b > a && b <= 240) rate_hz[0] = a, rate_hz[1] = b;
}
static uint64_t presents, wait_until, step_t;

static int region_ok(const RtMemoryInfo *mi) {
    // writable, readable guest memory: heap, module data, shared; not I/O, not free, nothing huge
    return mi->state != 0 && mi->state != 1 && (mi->perm & 3) == 3 && mi->size < (8ull << 30);
}

// A piece is scanned when no NVN pool covers its first or last byte, it is readable, and the game wrote a page of it
// (or the write watch does not cover it: module data). The written pages come from one write-watch read per region
// (a read per piece made the list take 13 s in the user's test).
static int collect(void) {
    uint32_t cap = 1u << 16;
    chunks = malloc(sizeof *chunks * cap);
    nchunks = 0;
    uint64_t total = 0, pool = 0, unwritten = 0, t_pool = 0, t_ww = 0;
    uint64_t *wp = NULL, wcap = 0;
    for (uint64_t a = 0; chunks && a < RT_ADDRESS_LIMIT;) {
        RtMemoryInfo mi;
        mem_query(a, &mi);
        uint64_t end = mi.base + mi.size;
        if (end <= a) break;
        if (region_ok(&mi)) {
            total += mi.size;
            uint64_t t = kern_now_ns(), need = mi.size >> 12, nw = UINT64_MAX;
            if (need > wcap) {
                free(wp);
                wp = malloc(sizeof *wp * (wcap = need));
                if (!wp) wcap = 0;
            }
            if (wp) nw = mem_written_peek(mi.base, mi.size, wp, wcap);   // ascending; UINT64_MAX: not watched
            t_ww += kern_now_ns() - t;
            uint64_t k = 0;
            for (uint64_t o = 0; o + CHUNK <= mi.size; o += CHUNK) {
                uint64_t g = mi.base + o;
                if (nw != UINT64_MAX) {   // a written page in [g, g + CHUNK)?
                    while (k < nw && wp[k] < g) k++;
                    if (k >= nw || wp[k] >= g + CHUNK) {
                        unwritten += CHUNK;
                        continue;
                    }
                }
                t = kern_now_ns();
                int in_pool = vk_mem_class_of(g) != VK_MCLS_NONE || vk_mem_class_of(g + CHUNK - 1) != VK_MCLS_NONE;
                t_pool += kern_now_ns() - t;
                if (in_pool) {
                    pool += CHUNK;
                    continue;
                }
                if (nchunks == cap) {
                    uint64_t *m = realloc(chunks, sizeof *chunks * (cap *= 2));
                    if (!m) {
                        free(wp);
                        return 0;
                    }
                    chunks = m;
                }
                chunks[nchunks++] = g;
            }
        }
        a = end;
    }
    free(wp);
    if (!chunks) return 0;
    nslices = (nchunks + SLICE_CH - 1) / SLICE_CH;
    cand = malloc(sizeof *cand * MAX_CAND);
    if (!cand) return 0;
    rt_log("stepscan: %.0f MiB scanned of %.0f MiB writable (%.0f MiB never written, %.0f MiB in GPU pools left out), "
           "%u slices; list: write watch %.0f ms, pool lookups %.0f ms",
           nchunks * (double)CHUNK / 1048576.0, total / 1048576.0, unwritten / 1048576.0, pool / 1048576.0, nslices,
           t_ww / 1e6, t_pool / 1e6);
    return 1;
}

static int alloc_pass(void) {
    copies = calloc(nslices + 1, sizeof *copies);
    okmap = calloc(nslices + 1, sizeof *okmap);
    cand_end = calloc(nslices + 1, sizeof *cand_end);
    ncand = cand_head = 0;
    slow_bytes = skip_until = 0;
    return copies && okmap && cand_end;
}

static void free_pass(void) {
    for (uint32_t s = 0; copies && s < nslices; s++) free(copies[s]), free(okmap[s]);
    free(copies), copies = NULL;
    free(okmap), okmap = NULL;
    free(cand_end), cand_end = NULL;
    ncand = cand_head = 0;
}

static void free_all(void) {
    free_pass();
    free(chunks), chunks = NULL;
    nchunks = nslices = 0;
    free(cand), cand = NULL;
    for (int k = 0; k < 2; k++) free(res[k]), res[k] = NULL, nres[k] = 0;
}

static uint32_t slice_lo(uint32_t s) { return s * SLICE_CH; }
static uint32_t slice_hi(uint32_t s) { return s * SLICE_CH + SLICE_CH < nchunks ? s * SLICE_CH + SLICE_CH : nchunks; }

static void copy_slice(uint32_t s) {
    uint32_t lo = slice_lo(s), hi = slice_hi(s);
    uint8_t *buf = malloc((size_t)(hi - lo) * CHUNK), *ok = calloc(hi - lo, 1);
    if (!buf || !ok) {
        free(buf), free(ok);
        return;
    }
    for (uint32_t i = lo; i < hi; i++) {
        uint64_t g = chunks[i];
        if (g < skip_until) continue;
        uint64_t t = kern_now_ns();
        int got = plat_mem_read_safe(g, buf + (size_t)(i - lo) * CHUNK, CHUNK);   // unreadable: fails, no fault
        if (!got) continue;
        if (kern_now_ns() - t > SLOW_NS) {
            skip_until = g + 32 * CHUNK;
            slow_bytes += 32 * CHUNK;
            continue;
        }
        ok[i - lo] = 1;
    }
    copies[s] = buf, okmap[s] = ok;
}

static int plausible_int(uint32_t a, uint32_t b) {
    int32_t d = (int32_t)(b - a);
    return d != 0 && d >= -4096 && d <= 4096;
}
static int plausible_float(uint32_t a, uint32_t b) {
    float fa, fb;
    memcpy(&fa, &a, 4), memcpy(&fb, &b, 4);
    if (!isfinite(fa) || !isfinite(fb) || fabsf(fa) > 1e7f || fabsf(fb) > 1e7f) return 0;
    float d = fb - fa;
    return d != 0 && fabsf(d) < 1e4f && fabsf(d) > 1e-7f;
}

// The raw stick as the game keeps it: an integer (or 16-bit half) that went from +x to -x.
static int stick_flip(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (x == -y && x != 0 && abs(x) >= 1000 && abs(x) <= 32768) return 1;
    int16_t lo = (int16_t)a, hi = (int16_t)(a >> 16), lo2 = (int16_t)b, hi2 = (int16_t)(b >> 16);
    return (lo != 0 && lo == -lo2 && abs(lo) >= 1000) || (hi != 0 && hi == -hi2 && abs(hi) >= 1000);
}

static void compare_slice(uint32_t s) {
    uint32_t lo = slice_lo(s), hi = slice_hi(s);
    static uint32_t now[CHUNK / 4];
    if (copies[s]) {
        for (uint32_t i = lo; i < hi; i++) {
            uint64_t g = chunks[i];
            if (!okmap[s][i - lo] || !plat_mem_read_safe(g, now, CHUNK)) continue;
            const uint32_t *was = (const uint32_t *)(copies[s] + (size_t)(i - lo) * CHUNK);
            for (uint64_t w = 0; w < CHUNK / 4 && ncand < MAX_CAND; w++) {
                uint32_t a = was[w], b = now[w];
                if (a != b && (plausible_int(a, b) || plausible_float(a, b) || stick_flip(a, b)))
                    cand[ncand++] = (Cand){g + 4 * w, a, b};
            }
        }
        free(copies[s]), copies[s] = NULL;
        free(okmap[s]), okmap[s] = NULL;
    }
    cand_end[s] = ncand;
}

static void check_slice(uint32_t s) {
    Res *out = res[rate_i];
    for (; cand_head < cand_end[s]; cand_head++) {
        const Cand *c = &cand[cand_head];
        uint32_t v;
        if (nres[rate_i] >= MAX_RES || !plat_mem_read_safe(c->addr, &v, 4)) continue;
        if (plausible_int(c->a, c->b) && (int32_t)(v - c->b) == (int32_t)(c->b - c->a)) {
            out[nres[rate_i]++] = (Res){c->addr, 1, (float)(int32_t)(c->b - c->a) / K, v};
            continue;
        }
        float fa, fb, fv;
        memcpy(&fa, &c->a, 4), memcpy(&fb, &c->b, 4), memcpy(&fv, &v, 4);
        if (plausible_float(c->a, c->b) && isfinite(fv)) {
            float d1 = fb - fa, d2 = fv - fb;
            if (d2 != 0 && fabsf(d2 - d1) <= 0.01f * fabsf(d1)) out[nres[rate_i]++] = (Res){c->addr, 2, d1 / K, v};
        }
    }
}

static int by_addr(const void *x, const void *y) {
    uint64_t a = ((const Res *)x)->addr, b = ((const Res *)y)->addr;
    return a < b ? -1 : a > b;
}

static void report(void) {
    char path[PLAT_PATH_MAX * 3 + 64];
    const char *lp = rt_log_path();
    snprintf(path, PLAT_PATH_MAX * 3, "%s", lp ? lp : "logs/x");
    char *s = plat_last_sep(path);
    if (s) *s = 0;
    size_t len = strlen(path);
    snprintf(path + len, sizeof path - len, "/stepscan-%llu.txt", (unsigned long long)presents);
    FILE *f = fopen(path, "w");
    if (f) fprintf(f, "# stepscan: guest words stepping by a constant amount per frame at %d and at %d fps\n"
                      "# address kind step_per_frame_%d step_per_frame_%d ratio value_now\n"
                      "# ratio ~1.0: same step per frame at both rates (fixed step, too fast at %d); ~%.2f: follows the rate\n",
                   rate_hz[0], rate_hz[1], rate_hz[0], rate_hz[1], rate_hz[1], follow_ratio());
    for (int k = 0; k < 2; k++) qsort(res[k], nres[k], sizeof *res[k], by_addr);
    uint32_t i = 0, j = 0, fixed = 0, real = 0, other = 0, logged = 0;
    uint64_t others[32], fixeds[64], fixfs[64];
    int nother = 0, nfixed = 0, nfixf = 0;
    while (i < nres[0] && j < nres[1]) {
        const Res *a = &res[0][i], *b = &res[1][j];
        if (a->addr < b->addr) { i++; continue; }
        if (b->addr < a->addr) { j++; continue; }
        i++, j++;
        if (a->kind != b->kind || a->step == 0) continue;
        double ratio = b->step / a->step;
        int cls = fabs(ratio - 1.0) < 0.1 ? 1 : fabs(ratio - follow_ratio()) < 0.1 * follow_ratio() ? 2 : 0;
        fixed += cls == 1, real += cls == 2, other += cls == 0;
        char val[32];
        if (a->kind == 1) {
            snprintf(val, sizeof val, "%d", (int32_t)b->value);
        } else {
            float fv;
            memcpy(&fv, &b->value, 4);
            snprintf(val, sizeof val, "%g", fv);
        }
        if (f) fprintf(f, "%#llx %s %g %g %.3f %s%s\n", (unsigned long long)a->addr, a->kind == 1 ? "int" : "float",
                       a->step, b->step, ratio, val, cls == 1 ? "  FIXED" : "");
        // trapped: the words that step neither like a fixed step nor with the rate first (the map cursor was one: 2x
        // by eye, 0.3 by the scan, as it turns at the edges), then the fixed ones
        if (cls == 0 && nother < (int)(sizeof others / sizeof *others)) others[nother++] = a->addr;
        if (cls == 1 && a->kind == 1 && nfixed < (int)(sizeof fixeds / sizeof *fixeds)) fixeds[nfixed++] = a->addr;
        if (cls == 1 && a->kind == 2 && nfixf < (int)(sizeof fixfs / sizeof *fixfs)) fixfs[nfixf++] = a->addr;
        if (cls == 1 && logged < 60) {
            logged++;
            rt_log("stepscan: FIXED %#llx %s %g per frame at %d and %g at %d (now %s)", (unsigned long long)a->addr,
                   a->kind == 1 ? "int" : "float", a->step, rate_hz[0], b->step, rate_hz[1], val);
        }
    }
    if (f) fclose(f);
    // Without the stick (ACR_STEPSCAN_STICK=0: a cutscene) the fixed float words go first: a clock in seconds that
    // steps 1/30 per update whatever the rate. With it, the other words first, then the fixed ints and floats.
    // ACR_STEPSCAN_FIRST=int: the fixed ints alone (frame counters).
    ntargets = 0;
    const int cap = (int)(sizeof targets / sizeof *targets);
    const char *first = cfg_get("ACR_STEPSCAN_FIRST");
    if (first && !strcmp(first, "int")) {
        for (int k = 0; k < nfixed && ntargets < cap; k++) targets[ntargets++] = fixeds[k];
    } else {
        if (!stick_on)
            for (int k = 0; k < nfixf && ntargets < cap; k++) targets[ntargets++] = fixfs[k];
        for (int k = 0; k < nother && ntargets < cap; k++) targets[ntargets++] = others[k];
        for (int k = 0; k < nfixed && ntargets < cap; k++) targets[ntargets++] = fixeds[k];
        if (stick_on)
            for (int k = 0; k < nfixf && ntargets < cap; k++) targets[ntargets++] = fixfs[k];
    }
    rt_log("stepscan: done: %u words fixed per frame (ratio 1), %u follow the rate (%.2f), %u other; list in %s", fixed,
           real, follow_ratio(), other, f ? path : "(file not written)");
}


static void stick_off(void);

// The write trap's callback, in the exception handler of the writing thread: no locks but its own spin, no log.
// Returns the slot of a new (word, instruction) pair: trap_bt then records its call chain.
#define BT_MAX 20
static uint64_t hit_bt[TRAP_HITS][BT_MAX];
static int hit_nbt[TRAP_HITS];

static int trap_hit(uint64_t addr, uint64_t ip) {
    int t = -1;
    for (int i = 0; i < ntargets; i++)
        if (addr - targets[i] < 4 || (trap_reads && targets[i] - addr < 16)) t = i;
    if (t < 0) {
        __atomic_fetch_add(&other_writes, 1, __ATOMIC_RELAXED);
        return -1;
    }
    while (__atomic_exchange_n(&hit_lock, 1, __ATOMIC_ACQUIRE)) {}
    int n = nhits, i = 0, slot = -1;
    while (i < n && !(hit_t[i] == targets[t] && hit_ip[i] == ip)) i++;
    if (i == n && n < TRAP_HITS) {
        hit_t[n] = targets[t], hit_ip[n] = ip, hit_n[n] = 0, hit_nbt[n] = -1, slot = n;
        __atomic_store_n(&nhits, n + 1, __ATOMIC_RELEASE);
    }
    if (i < TRAP_HITS) hit_n[i]++;
    __atomic_store_n(&hit_lock, 0, __ATOMIC_RELEASE);
    return slot;
}

static void trap_bt(int slot, const uint64_t *frames, int n) {
    if (n > BT_MAX) n = BT_MAX;
    memcpy(hit_bt[slot], frames, sizeof *frames * n);
    __atomic_store_n(&hit_nbt[slot], n, __ATOMIC_RELEASE);
}

static void trap_start(void) {
    uint64_t pages[32];
    int n = 0;
    for (int i = 0; i < ntargets && n < 32; i++) {
        uint64_t pg = targets[i] & ~0xfffull;
        int have = 0;
        for (int k = 0; k < n; k++) have |= pages[k] == pg;
        if (!have) pages[n++] = pg;
    }
    nhits = hits_logged = 0;
    other_writes = 0;
    plat_trap_access(trap_reads);
    int got = n ? plat_trap_set(pages, n, trap_hit, trap_bt) : 0;
    rt_log("stepscan: %s trap on %d pages for %d presents (executable at %#llx)", trap_reads ? "read" : "write", got,
           TRAP_PRESENTS,
           (unsigned long long)plat_exe_base());
    trap_left = got ? TRAP_PRESENTS : 0;
    if (!trap_left) stick_off();
}

static void trap_tick(void) {
    int n = __atomic_load_n(&nhits, __ATOMIC_ACQUIRE);
    for (; hits_logged < n; hits_logged++)
        rt_log("stepscan: write to %#llx from host %#llx (exe+%#llx)", (unsigned long long)hit_t[hits_logged],
               (unsigned long long)hit_ip[hits_logged], (unsigned long long)(hit_ip[hits_logged] - plat_exe_base()));
    if (--trap_left > 0) {
        plat_trap_arm();
        return;
    }
    plat_trap_clear();
    plat_trap_access(0);
    stick_off();
    uint64_t base = plat_exe_base();
    for (int i = 0; i < n; i++) {
        char line[BT_MAX * 14 + 8];
        size_t o = 0;
        line[0] = 0;
        int nb = __atomic_load_n(&hit_nbt[i], __ATOMIC_ACQUIRE);
        for (int k = 0; k < nb && o + 14 < sizeof line; k++)
            o += snprintf(line + o, sizeof line - o, " %llx", (unsigned long long)(hit_bt[i][k] - base));
        rt_log("stepscan: trap summary: %#llx %s %u times from exe+%#llx; called from%s", (unsigned long long)hit_t[i],
               trap_reads ? "read" : "written",
               hit_n[i], (unsigned long long)(hit_ip[i] - base), nb > 0 ? line : " (no chain)");
    }
    rt_log("stepscan: trap over: %d accesses found, %llu to other words of those pages", n,
           (unsigned long long)other_writes);
}

// The left stick at STICK_TILT (75 %; 25 % did not move the map cursor) to the right for both passes (ACR_STEPSCAN_STICK=0: untouched): on the map the cursor then
// moves at a constant speed, the step to find.
#define STICK_TILT 24576
static int stick_dir = 1, stick_ph;

// Right then left, STICK_FLIP presents each way: the cursor keeps moving instead of resting on the map's edge, and the
// game's copy of the stick (a float near +-0.75) differs between every copy and compare, so the track follows it.
#define STICK_FLIP 10   // = K: a slice's copy and compare see the stick held opposite ways
static void stick_tick(void) {
    if (!stick_on) return;
    if (++stick_ph % STICK_FLIP == 0) stick_dir = -stick_dir;
    input_force_lstick(1, stick_dir * STICK_TILT, 0);
}

static int start_rate(int i) {
    rate_i = i;
    const char *st = cfg_get("ACR_STEPSCAN_STICK");
    stick_on = !(st && !strcmp(st, "0"));
    frame_step_force_hz(rate_hz[i]);
    state = 1;
    wait_until = presents + SETTLE;
    step_t = 0;
    res[i] = malloc(sizeof *res[i] * MAX_RES);
    nres[i] = 0;
    return res[i] && alloc_pass();
}

static void stick_off(void) {
    stick_on = 0;
    input_force_lstick(0, 0, 0);
}

static void track_free(void) {
    free(trk), trk = NULL, ntrk = 0;
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 6; k++) free(trk_v[r][k]), trk_v[r][k] = NULL;
}

static void finish(void) {
    track_free();
    free_all();
    frame_step_force_hz(0);
    state = 0;
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b;
}

// Reads every tracked word: one read per run of words within 64 KiB (an unreadable run reads as 0).
static void track_snap(uint32_t *out) {
    static uint8_t buf[CHUNK + 8];
    for (uint32_t i = 0; i < ntrk;) {
        uint64_t lo = trk[i];
        uint32_t j = i;
        while (j + 1 < ntrk && trk[j + 1] + 4 - lo <= CHUNK) j++;
        uint64_t n = trk[j] + 4 - lo;
        int ok = plat_mem_read_safe(lo, buf, n);
        for (uint32_t k = i; k <= j; k++) {
            if (ok) memcpy(&out[k], buf + (trk[k] - lo), 4);
            else out[k] = 0;
        }
        i = j + 1;
    }
}

static void track_rate(int r) {
    trk_rate = r;
    frame_step_force_hz(rate_hz[r]);
    stick_on = 0;
    input_force_lstick(1, 0, 0);   // still while the rate settles
    trk_t0 = presents + SETTLE;
}

static void track_start(void) {
    free_pass();
    free(cand), cand = NULL;
    for (int k = 0; k < 2; k++) free(res[k]), res[k] = NULL, nres[k] = 0;
    if (!trk || !ntrk) {
        rt_log("stepscan: stick track: no words to follow");
        stick_off(), finish();
        return;
    }
    qsort(trk, ntrk, sizeof *trk, cmp_u64);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ntrk; i++)
        if (!n || trk[i] != trk[n - 1]) trk[n++] = trk[i];
    ntrk = n;
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 6; k++)
            if (!(trk_v[r][k] = malloc(sizeof(uint32_t) * ntrk))) {
                rt_log("stepscan: out of memory; stopped");
                stick_off(), finish();
                return;
            }
    rt_log("stepscan: stick track of %u words: right %d presents then left, at %d then %d fps", ntrk, TRACK_SEG, rate_hz[0],
           rate_hz[1]);
    state = 3;
    track_rate(0);
}

static int track_kind(const uint32_t *v) {   // 2 float, 1 int, 0 neither
    float f[6];
    memcpy(f, v, sizeof f);
    int fl = 1;
    for (int k = 0; k < 6; k++) fl &= isfinite(f[k]) && fabsf(f[k]) < 1e7f;
    if (fl && plausible_float(v[0], v[2])) return 2;
    if (plausible_int(v[0], v[2])) return 1;
    return 0;
}

// Per present move while right and while left; 0 unless the word moved steadily one way (both 10-present halves of
// the segment within 25 % of each other: buffers that only change, as audio and GPU commands, fail it) and back by
// about as much.
static int steady(double d1, double d2) {
    if (d1 == 0 || d2 == 0 || (d1 > 0) != (d2 > 0)) return 0;
    double m = fabs(d1) > fabs(d2) ? fabs(d1) : fabs(d2);
    return fabs(d1 - d2) <= 0.25 * m;
}

static double track_move(const uint32_t *v, int kind) {
    double d[4];
    if (kind == 2) {
        float f[6];
        memcpy(f, v, sizeof f);
        d[0] = f[1] - f[0], d[1] = f[2] - f[1], d[2] = f[4] - f[3], d[3] = f[5] - f[4];
    } else {
        d[0] = (int32_t)(v[1] - v[0]), d[1] = (int32_t)(v[2] - v[1]);
        d[2] = (int32_t)(v[4] - v[3]), d[3] = (int32_t)(v[5] - v[4]);
    }
    if (!steady(d[0], d[1]) || !steady(d[2], d[3])) return 0;
    double a = (d[0] + d[1]) / 20.0, b = (d[2] + d[3]) / 20.0;
    if ((a > 0) == (b > 0)) return 0;
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    if (fabs(a + b) > 0.3 * m) return 0;
    return (fabs(a) + fabs(b)) / 2;
}

static void track_report(void) {
    char path[PLAT_PATH_MAX * 3 + 64];
    const char *lp = rt_log_path();
    snprintf(path, PLAT_PATH_MAX * 3, "%s", lp ? lp : "logs/x");
    char *sep = plat_last_sep(path);
    if (sep) *sep = 0;
    size_t len = strlen(path);
    snprintf(path + len, sizeof path - len, "/stick-%llu.txt", (unsigned long long)presents);
    FILE *f = fopen(path, "w");
    if (f) fprintf(f, "# stick track: words that moved with the stick (right, then back left)\n"
                      "# address kind move_per_frame_%d move_per_frame_%d ratio value_%d value_%d\n"
                      "# ratio ~1: fixed step per frame (too fast at %d); ~%.2f: follows the rate\n",
                   rate_hz[0], rate_hz[1], rate_hz[0], rate_hz[1], rate_hz[1], follow_ratio());
    static uint64_t fixed[4096], rest[4096];
    static double fmove[4096];
    static int fint[4096];
    int nf = 0, nr = 0, logged = 0;
    uint32_t total = 0, nfix = 0, nreal = 0;
    for (uint32_t i = 0; i < ntrk; i++) {
        uint32_t v0[6], v1[6];
        for (int k = 0; k < 6; k++) v0[k] = trk_v[0][k][i], v1[k] = trk_v[1][k][i];
        int kind = track_kind(v0);
        if (!kind || kind != track_kind(v1)) continue;
        double m0 = track_move(v0, kind), m1 = track_move(v1, kind);
        if (m0 == 0 || m1 == 0) continue;
        double ratio = m1 / m0;
        total++;
        int fx = ratio > 0.75 && ratio < 1.35;
        nfix += fx, nreal += fabs(ratio - follow_ratio()) < 0.3 * follow_ratio();
        char a[32], b[32];
        if (kind == 2) {
            float x, y;
            memcpy(&x, &v0[2], 4), memcpy(&y, &v1[2], 4);
            snprintf(a, sizeof a, "%g", x), snprintf(b, sizeof b, "%g", y);
        } else {
            snprintf(a, sizeof a, "%d", (int32_t)v0[2]), snprintf(b, sizeof b, "%d", (int32_t)v1[2]);
        }
        if (f) fprintf(f, "%#llx %s %g %g %.3f %s %s%s\n", (unsigned long long)trk[i], kind == 2 ? "float" : "int", m0, m1,
                       ratio, a, b, fx ? "  FIXED" : "");
        if (fx && nf < 4096) fint[nf] = kind == 1, fmove[nf] = m0, fixed[nf++] = trk[i];
        else if (!fx && nr < 4096) rest[nr++] = trk[i];
        if (fx && logged++ < 40)
            rt_log("stepscan: stick FIXED %#llx %s moves %g per frame at %d and %g at %d (now %s)",
                   (unsigned long long)trk[i], kind == 2 ? "float" : "int", m0, rate_hz[0], m1, rate_hz[1], b);
    }
    if (f) fclose(f);
    rt_log("stepscan: stick track: %u words move with the stick, %u fixed per frame (too fast at the second rate), %u follow the rate; "
           "list in %s", total, nfix, nreal, f ? path : "(file not written)");
    // The game's copy of the stick: a float held at +a while the stick is right and -a while left (a near 0.75), at both
    // rates. Its readers are the code that moves the cursor and scrolls the map: the trap watches reads of it.
    int nstick = 0;
    for (uint32_t i = 0; i < ntrk && nstick < 32; i++) {
        int ok = 1;
        float a0 = 0;
        for (int r = 0; r < 2 && ok; r++) {
            float f[6];
            int32_t n[6];
            for (int k = 0; k < 6; k++) memcpy(&f[k], &trk_v[r][k][i], 4), n[k] = (int32_t)trk_v[r][k][i];
            int fl = 1;
            for (int k = 0; k < 6; k++) fl &= isfinite(f[k]);
            float a = f[0];
            int okf = fl && fabsf(a) > 0.2f && fabsf(a) < 1.05f;
            for (int k = 1; k < 3; k++) okf &= fabsf(f[k] - a) < 0.01f;
            for (int k = 3; k < 6; k++) okf &= fabsf(f[k] + a) < 0.02f;
            // an integer at +x then -x, or a 16-bit half doing so
            int oki = 0;
            for (int sh = 0; sh < 3 && !oki; sh++) {
                int32_t v[6];
                for (int k = 0; k < 6; k++) v[k] = sh == 0 ? n[k] : sh == 1 ? (int16_t)n[k] : (int16_t)(n[k] >> 16);
                int o = abs(v[0]) >= 1000 && abs(v[0]) <= 32768;
                for (int k = 1; k < 3; k++) o &= v[k] == v[0];
                for (int k = 3; k < 6; k++) o &= v[k] == -v[0];
                oki = o;
                if (o) a = (float)v[0];
            }
            // the game's pad record (main+0x8c0f90 builds it): each stick axis as an integer centred on 127
            int okc = n[0] != n[3] && abs(n[0] - 127) >= 20 && abs(n[0] + n[3] - 254) <= 2 && n[0] >= 0 && n[0] <= 255;
            for (int k = 1; k < 3; k++) okc &= n[k] == n[0] && n[3 + k] == n[3];
            if (okc && !okf && !oki) a = (float)n[0];
            ok &= okf || oki || okc;
            if (r == 0) a0 = a;
            else ok &= fabsf(a - a0) < 0.01f * (fabsf(a0) > 1 ? fabsf(a0) : 1);
        }
        if (!ok) continue;
        uint64_t w = trk[i];
        int dup = 0;
        for (int q = 0; q < nstick; q++) dup |= (targets[q] >> 12) == (w >> 12) && 0;
        if (dup) continue;
        targets[nstick++] = w;
        rt_log("stepscan: stick value at %#llx: %g right, %g left", (unsigned long long)w, a0, -a0);
    }
    if (nstick) {
        ntargets = nstick;
        trap_reads = 1;
        return;
    }
    trap_reads = 0;
    // The fixed words that move the most: integers moving a pixel or more per frame first (screen positions: the
    // cursor's rectangle moved 8.9 per frame in test 9), then floats by their move; 64 words on 32 pages at most.
    ntargets = 0;
    int npg = 0;
    uint64_t pgs[32];
    for (int pass = 0; pass < 2; pass++) {
        for (;;) {
            int best = -1;
            for (int k = 0; k < nf; k++)
                if (fixed[k] && fint[k] == !pass && (!pass ? fmove[k] >= 1 : fmove[k] > 1e-4) &&
                    (best < 0 || fmove[k] > fmove[best]))
                    best = k;
            if (best < 0 || ntargets >= 64) break;
            uint64_t w = fixed[best], pg = w >> 12;
            fixed[best] = 0;
            int have = 0;
            for (int q = 0; q < npg; q++) have |= pgs[q] == pg;
            if (!have && npg == 32) continue;
            if (!have) pgs[npg++] = pg;
            targets[ntargets++] = w;
        }
    }
}

static void track_tick(void) {
    if (presents < trk_t0) return;
    uint64_t t = presents - trk_t0;
    input_force_lstick(1, t < TRACK_SEG ? STICK_TILT : -STICK_TILT, 0);
    int r = trk_rate;
    for (int k = 0; k < 3; k++) {
        if (t == 10 + 10 * (uint64_t)k) track_snap(trk_v[r][k]);
        if (t == TRACK_SEG + 10 + 10 * (uint64_t)k) track_snap(trk_v[r][3 + k]);
    }
    if (t < 2 * TRACK_SEG) return;
    if (r == 0) {
        track_rate(1);
        return;
    }
    track_report();
    track_free();
    free_all();
    frame_step_force_hz(0);
    state = 0;
    stick_on = 1;   // the trap watches with the stick swinging
    trap_start();
}

// At every present, on the presenting thread (frame_rate.c frame_step_present).
void stepscan_present(void) {
    static int key_was, at_done, slow_logs;
    static double at = -2;
    presents++;
    if (at == -2) {
        const char *e = cfg_get("ACR_STEPSCAN");
        at = e && *e ? atof(e) : -1;
    }
    int key = input_stepscan_key();
    int go = key && !key_was;
    key_was = key;
    if (at >= 0 && !at_done && frame_step_world_us() >= (int64_t)(at * 1e6)) go = 1, at_done = 1;
    stick_tick();
    if (state == 0 && trap_left > 0) {
        trap_tick();
        return;
    }
    if (state == 0) {
        if (!go) return;
        // ACR_STEPSCAN_TARGETS=<addr>,<addr>... (hex guest addresses, from an earlier scan of the same scripted run): no
        // passes, the trap goes straight on them (writes; ACR_STEPSCAN_READS=1: reads), with the stick left alone.
        const char *tg = cfg_get("ACR_STEPSCAN_TARGETS");
        if (tg && *tg) {
            ntargets = 0;
            for (const char *q = tg; *q && ntargets < (int)(sizeof targets / sizeof *targets);) {
                char *end;
                unsigned long long a = strtoull(q, &end, 16);
                if (end == q) break;
                targets[ntargets++] = a;
                q = *end == ',' ? end + 1 : end;
            }
            const char *rd = cfg_get("ACR_STEPSCAN_READS");
            trap_reads = rd && atoi(rd);
            rt_log("stepscan: trap on %d given words at present %llu", ntargets, (unsigned long long)presents);
            trap_start();
            return;
        }
        ntargets = 0;
        track_free();
        trk = malloc(sizeof *trk * TRACK_MAX);
        uint64_t t0 = kern_now_ns();
        rates_read();
        rt_log("stepscan: start at present %llu: %d fps then %d fps", (unsigned long long)presents, rate_hz[0], rate_hz[1]);
        if (!collect() || !start_rate(0)) {
            rt_log("stepscan: out of memory; stopped");
            stick_off(), finish();
            return;
        }
        rt_log("stepscan: memory list made in %.0f ms", (kern_now_ns() - t0) / 1e6);
        return;
    }
    if (state == 3) {
        track_tick();
        return;
    }
    if (state == 1) {
        if (presents < wait_until) return;
        state = 2;
    }
    uint64_t t = step_t++;
    uint64_t t0 = kern_now_ns(), c0 = cand_head;
    if (t < nslices) copy_slice((uint32_t)t);
    uint64_t t1 = kern_now_ns();
    if (t >= K && t - K < nslices) compare_slice((uint32_t)(t - K));
    uint64_t t2 = kern_now_ns();
    if (t >= 2 * K && t - 2 * K < nslices) check_slice((uint32_t)(t - 2 * K));
    uint64_t t3 = kern_now_ns();
    if (t3 - t0 > 40000000ull && slow_logs++ < 40)   // what a slow present spent its time on
        rt_log("stepscan: present took %.0f ms: copy %.0f, compare %.0f, check %.0f (%llu candidates)", (t3 - t0) / 1e6,
               (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6, (unsigned long long)(cand_head - c0));
    if (t < 2 * K + nslices - 1) return;
    rt_log("stepscan: %d fps: %u words stepped the same amount twice (%u changed once; %.0f MiB left out as slow)",
           rate_hz[rate_i], nres[rate_i], ncand, slow_bytes / 1048576.0);
    for (uint32_t k = 0; trk && k < ncand && ntrk < TRACK_MAX; k++) trk[ntrk++] = cand[k].addr;
    free_pass();
    if (rate_i == 0) {
        if (start_rate(1)) return;
        rt_log("stepscan: out of memory; stopped");
        stick_off(), finish();
        return;
    }
    report();
    if (stick_on) {
        track_start();
        return;
    }
    // no stick: no stick track; the trap goes on the words the passes found
    free_pass();
    free(cand), cand = NULL;
    for (int k = 0; k < 2; k++) free(res[k]), res[k] = NULL, nres[k] = 0;
    track_free();
    free_all();
    frame_step_force_hz(0);
    state = 0;
    trap_start();
}
