// Xbox and PlayStation button prompts: the game draws the button icons of its prompts and menus from one texture atlas (G1T 3226a433
// in asset/ScreenLayout.rdb.bin: 1024x1024 BC3, one level) and the L and R tabs of its menus from another (G1T
// 8d1f8cb8, 512x256). tools/make_prompts.py redraws the Switch buttons of the player's own textures as Xbox ones and
// writes the result next to romfs-index.bin (input_prompts_folder names the file); romfs.c overlay_init loads the one
// // every file at start (up to PROMPTS_FILES), and the one the settings ask for is active (prompts_select, live):
// vk_texture.c upload puts each texture in place of the game's when the game uploads one whose bytes are the game's
// (same size and FNV-1a hash), and marks it, so a change of the setting uploads it again from the game's bytes. The game's files stay as they are, so a RomFS
// mod that changes one of these textures keeps its own (its hash differs).
//
// File: header <8sII (magic "ACRPRM2\0", texture count, 0), then per texture <IIQQ (width, height, FNV-1a 64 of the
// game's BC3 blocks, size) and the new blocks in rows (the layout of the G1T and of vk_texture.c's staging copy).
#include "prompts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

enum { MAX_TEX = 8 };
static const char MAGIC[8] = "ACRPRM2";
typedef struct { uint32_t w, h; uint64_t hash, size; uint8_t *data; } PromptTex;
static PromptTex g_files[PROMPTS_FILES][MAX_TEX];
static int g_count[PROMPTS_FILES];
static int g_active = -1;      // the file in use, -1: the game's own buttons
static uint32_t g_gen = 1;     // bumped when g_active changes: marked textures upload again

uint64_t prompts_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int prompts_load(int slot, const char *path) {
    if (slot < 0 || slot >= PROMPTS_FILES || g_count[slot]) return 0;
    PromptTex *g_tex = g_files[slot];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[16];
    uint32_t n = 0;
    int ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, MAGIC, 8);
    if (ok) memcpy(&n, h + 8, 4), ok = n >= 1 && n <= MAX_TEX;
    int got = 0;
    for (; ok && got < (int)n; got++) {
        uint8_t e[24];
        uint32_t w = 0, hh = 0;
        uint64_t hash = 0, size = 0;
        ok = fread(e, 1, sizeof e, f) == sizeof e;
        if (ok) {
            memcpy(&w, e, 4), memcpy(&hh, e + 4, 4), memcpy(&hash, e + 8, 8), memcpy(&size, e + 16, 8);
            ok = w && hh && w <= 16384 && hh <= 16384 && size == (uint64_t)((w + 3) / 4) * ((hh + 3) / 4) * 16;
        }
        uint8_t *d = ok ? malloc(size) : NULL;
        ok = d && fread(d, 1, size, f) == size;
        if (!ok) {
            free(d);
            break;
        }
        g_tex[got].w = w, g_tex[got].h = hh, g_tex[got].hash = hash, g_tex[got].size = size, g_tex[got].data = d;
    }
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        for (int i = 0; i < got; i++) free(g_tex[i].data), g_tex[i].data = NULL;
        return 0;
    }
    g_count[slot] = got;
    return 1;
}

void prompts_select(int slot) {
    if (slot >= PROMPTS_FILES || (slot >= 0 && !g_count[slot])) slot = -1;
    if (__atomic_exchange_n(&g_active, slot, __ATOMIC_ACQ_REL) != slot) __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELEASE);
}

uint32_t prompts_gen(void) { return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE); }

int prompts_swap(uint32_t w, uint32_t h, int bc3, uint8_t *blocks, size_t size) {
    if (!bc3) return 0;
    uint64_t hash = 0;
    int active = __atomic_load_n(&g_active, __ATOMIC_ACQUIRE);
    for (int f = 0; f < PROMPTS_FILES; f++)   // every file holds the game's fingerprints: one is enough to know it
        for (int i = 0; i < g_count[f]; i++) {
            const PromptTex *g = &g_files[f][i];
            if (g->w != w || g->h != h || g->size != size) continue;
            if (!hash) hash = prompts_hash(blocks, size);
            if (hash != g->hash) continue;
            for (int k = 0; active >= 0 && k < g_count[active]; k++) {
                const PromptTex *a = &g_files[active][k];
                if (a->hash == hash && a->size == size) memcpy(blocks, a->data, size);
            }
            rt_log("prompts: a texture of the game's buttons (%ux%u) uploaded with the %s buttons", w, h,
                   active >= 0 ? "new" : "game's own");
            return 1;
        }
    return 0;
}
