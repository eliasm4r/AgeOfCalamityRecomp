// Adapted from MacNativeGames games/totk/runtime/romfs.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Read-only game RomFS backed by the player's extracted image.
//
// Input: <romfs>/romfs.bin (rt_config()->romfs), whose six IVFC levels and SHA-256 tools/extract.py verified, and
// romfs-index.bin next to AgeOfCalamityRecomp (ACR_ROMFS_INDEX overrides), which tools/romfs_index.py wrote together with
// romfs_config.h. fs_init checks the index's SHA-256 and layout and the image size. The full 17 GB hash runs only with
// ACR_ROMFS_VERIFY=full; a proof file (.romfs-verified-v1 next to the index, or in ACR_ROMFS_PROOF_DIR) keyed by
// the file's volume, file id, size and times skips it on later starts, ACR_ROMFS_VERIFY=force ignores the proof.
//
// Reads are positional (platform.h plat_file_pread), so threads read the image in parallel without a file-pointer
// lock. ACR_ROMFS_MMAP=1 maps the image instead.
//
// Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
// path, the overlay's size and SHA-256 and the original's SHA-256 ("-" adds a file the RomFS lacks; it is numbered
// after the RomFS's own files, file_count + k in path order). The directories, in this order: ACR_ROMFS_PATCHES and
// ACR_ROMFS_MODS; unset or "0" = none. A missing path, a size or hash mismatch or a changed original stops the start.
// An overlay file is opened when the game opens it; its identity (volume, file id, size, last write) is kept from the
// start check, and an open that finds another file there stops the game.
//
// Player mods (mods.c): the files under each enabled mod's RomFS root are overlays too, checked by identity only (no
// hash, no manifest). A path several overlays give is served by one: a player mod higher in the order beats a lower
// one, any player mod beats the ACR_ROMFS_MODS manifest, which beats romfs-patches (overlay_contest). A mod path that
// matches a RomFS path only without case is served under the RomFS spelling.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "layout.h"
#include "mods.h"
#include "prompts.h"
#include "romfs_config.h"
#include "romfs_path.h"
#include "platform/platform.h"
#include "sha256.h"

#define NH 65536                     // RomFS handle slots
#define READ_CHUNK (64u << 20)       // largest single read
#define HASH_CHUNK (8u << 20)
#define ADDED UINT64_MAX             // an added overlay file's index until overlay_init numbers it

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");

typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } Identity;   // no padding
// prio: which overlay serves a path two of them give (the higher); romfs-patches 0, the ACR_ROMFS_MODS manifest 1, a
// player mod 2 + its distance from the bottom of the order. mod: its index in mods_at + 1, 0 for any other overlay.
typedef struct { uint64_t index, size; char *path, *name; Identity id; int prio, mod; } Overlay;

static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count, strings_offset;
static PlatFile *image;
// The RomFS header (0x50 bytes) and the extent of the four metadata tables (directory and file hash/meta) in the image.
// Astral Chain reads its RomFS through the SDK, which expects the fs service to have written that metadata into the
// cache buffer MountRom receives; ov_mount_rom builds it there.
static uint8_t romfs_header[0x50];
static uint64_t romfs_meta_off, romfs_meta_size;
static const uint8_t *image_map;
static char image_file[2048];
static char index_dir[2048];         // the directory of romfs-index.bin: the proof file's default place
static char mount_name[32];
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t handles[NH];         // file index + 1; 0 is free
static uint32_t handle_ovl[NH];      // overlay slot + 1 of an open handle, 0 = original
static PlatFile *handle_file[NH];    // the handle's open overlay file, when handle_ovl is set
static Overlay *overlays;            // sorted by RomFS index after overlay_init
static size_t noverlay, overlay_cap;
static char **added;                 // RomFS paths of the added files, sorted; added[k] is index file_count + k
static size_t nadded;
static int patches_loaded;           // a romfs-patches manifest gave a file (romfs_patches_loaded)

static void need(int c, const char *m) {
    if (!c) rt_fatal("RomFS: %s", m);
}

static Entry entry(uint64_t i) {
    Entry e;
    memcpy(&e, index_bytes + 24 + i * 24, 24);
    return e;
}
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

// ---- positional reads
// Bytes read at off (0 at or past the end of the file), -1 on an I/O error.
static int64_t read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    int64_t got = plat_file_pread(f, buf, n, off);
    if (got == -2)   // a read-only page in the buffer: a frozen region missed its thaw
        rt_log("RomFS: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static PlatFile *open_read(const char *path) { return plat_file_open(path); }

// Read-fault paging for the image at ROMFS_BASE (recomp/layout.py): the game parses the image directly at that
// fixed guest address, so the first guest read of a 4 KiB page commits it and fills it from romfs.bin.
// MapViewOfFileEx cannot place the view there (ROMFS_BASE is not 64 KiB-aligned), hence the fault path.
static int romfs_read_fault(uint64_t addr) {
    if (addr < ROMFS_BASE || addr >= ROMFS_BASE + ROMFS_SIZE) return 0;
    uint64_t page = addr & ~0xfffull, off = page - ROMFS_BASE;
    uint32_t n = (uint32_t)((ROMFS_SIZE - off < 0x1000) ? ROMFS_SIZE - off : 0x1000);
    if (!plat_mem_commit(page, 0x1000)) return 0;
    return read_at(image, (void *)page, n, off) == n;
}

static Identity identity_of(PlatFile *f) {
    PlatFileId i;
    need(plat_file_id(f, &i), "file information");
    return (Identity){(uint32_t)i.volume, 0, i.file_id, i.size, i.write_time, i.create_time};
}

// ---- SHA-256 over a file range, streaming
static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void sha_range(PlatFile *f, uint64_t offset, uint64_t size, char hex[65], int progress) {
    Sha256 h;
    sha256_init(&h);
    uint8_t *buf = malloc(HASH_CHUNK);
    need(buf != NULL, "hash buffer");
    double t0 = now_s();
    for (uint64_t done = 0; done < size;) {
        uint32_t want = size - done > HASH_CHUNK ? HASH_CHUNK : (uint32_t)(size - done);
        int64_t n = read_at(f, buf, want, offset + done);
        need(n > 0, "read of hash input");
        sha256_update(&h, buf, (size_t)n);
        done += (uint64_t)n;
        if (progress && (done % (2ull << 30) < (uint64_t)n || done == size))
            rt_log("RomFS: hashed %.1f of %.1f GiB (%.0f MiB/s)", done / 1073741824.0, size / 1073741824.0,
                   done / 1048576.0 / (now_s() - t0 + 1e-9));
    }
    uint8_t d[32];
    sha256_final(&h, d);
    free(buf);
    hex32(d, hex);
}

// ---- full image verification with a proof file
typedef struct { char magic[16], sha256[65]; uint8_t pad[7]; Identity id; } RomfsProof;

static void verify_image(const char *dir) {
    const char *mode = cfg_get("ACR_ROMFS_VERIFY");
    if (!mode || (strcmp(mode, "full") && strcmp(mode, "force"))) return;
    const char *pdir = cfg_get("ACR_ROMFS_PROOF_DIR");
    if (pdir && *pdir) dir = pdir;
    RomfsProof want = {0}, saved;
    memcpy(want.magic, "ACRFS-PROOF-1", sizeof "ACRFS-PROOF-1");
    memcpy(want.sha256, ROMFS_IMAGE_SHA256, sizeof ROMFS_IMAGE_SHA256 < 65 ? sizeof ROMFS_IMAGE_SHA256 : 65);
    want.id = identity_of(image);
    char path[2100];
    need(snprintf(path, sizeof path, "%s/.romfs-verified-v1", dir) < (int)sizeof path, "proof path");
    if (strcmp(mode, "force")) {
        PlatFile *p = open_read(path);
        if (p) {
            int ok = plat_file_pread(p, &saved, sizeof saved, 0) == sizeof saved;
            plat_file_close(p);
            if (ok && !memcmp(&saved, &want, sizeof want)) {
                rt_log("RomFS unchanged since its full SHA-256 check (%s)", path);
                return;
            }
        }
    }
    rt_log("RomFS: verifying the SHA-256 of %s (%llu bytes)", image_file, (unsigned long long)want.id.size);
    char hex[65];
    sha_range(image, 0, want.id.size, hex, 1);
    if (strcmp(hex, ROMFS_IMAGE_SHA256))
        rt_fatal("RomFS image %s has SHA-256 %s, the extraction recorded %s (extract it again)", image_file, hex,
                 ROMFS_IMAGE_SHA256);
    Identity after = identity_of(image);
    need(!memcmp(&after, &want.id, sizeof after), "image changed during verification");
    plat_file_write_atomic(path, &want, sizeof want);
    rt_log("RomFS: image SHA-256 ok");
}

// ---- index lookup
static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static size_t added_lower(const char *p) {
    size_t lo = 0, hi = nadded;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (strcmp(added[m], p) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// The index of RomFS path p (an added file: file_count + k), or UINT64_MAX.
static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t k = added_lower(p);
    return k < nadded && !strcmp(added[k], p) ? file_count + k : UINT64_MAX;
}

// 1 when a RomFS file or an added one lies under folder (a path ending in '/').
static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t k = added_lower(folder);
    return k < nadded && !strncmp(added[k], folder, n);
}

// The overlay slot + 1 that replaces RomFS file i, or 0.
static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (overlays[m].index < i) lo = m + 1;
        else hi = m;
    }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

// ---- overlays
// Loads and checks one overlay folder; a failed check stops the game. A folder without a manifest loads nothing.
// prio: the Overlay field (0 for the port's own folders, 1 for the ACR_ROMFS_MODS manifest).
static void overlay_load(const char *dir, int prio) {
#define BAD(...) rt_fatal(__VA_ARGS__)
    char path[2100];
    need(snprintf(path, sizeof path, "%s/manifest.tsv", dir) < (int)sizeof path, "overlay path");
    FILE *m = fopen(path, "rb");
    if (!m) {
        rt_log("RomFS overlay: none (%s)", path);
        return;
    }
    char line[8192];
    size_t first = noverlay;
    uint64_t hashed = 0;
    double t0 = now_s();
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[4096], sha[65], orig[65], hex[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 &&
             strlen(sha) == 64 && (strlen(orig) == 64 || !strcmp(orig, "-")), "overlay manifest line");
        need(romfs_path_valid_relative(name), "overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add)
            BAD("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its "
                "original's SHA-256", dir, name);
        if (known && add)
            BAD("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256",
                dir, name);
        Entry e = {0};
        if (known) {
            e = entry(i);
            sha_range(image, e.offset, e.size, hex, 0);
            if (strcmp(hex, orig)) BAD("RomFS overlay: the original of %s has SHA-256 %s, %s/manifest.tsv says %s",
                                       name, hex, dir, orig);
        } else {
            i = ADDED;
        }
        char file[8200];
        need(snprintf(file, sizeof file, "%s/%s", dir, name) < (int)sizeof file, "overlay file path");
        PlatFile *f = open_read(file);
        if (!f) BAD("RomFS overlay: %s missing", file);
        Identity id = identity_of(f);
        if (id.size != size)
            BAD("RomFS overlay: %s has %llu bytes, its manifest says %llu", file, (unsigned long long)id.size, size);
        sha_range(f, 0, size, hex, 0);
        plat_file_close(f);
        if (strcmp(hex, sha)) BAD("RomFS overlay: %s has SHA-256 %s, its manifest says %s", file, hex, sha);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *copy = strdup(file), *rel = strdup(name);
        need(copy && rel, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, rel, id, prio, 0};
        if (!prio) patches_loaded = 1;
        if (noverlay - first <= 16)
            rt_log("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    fclose(m);
    if (noverlay - first > 16) rt_log("RomFS overlay: %zu files from %s (%zu not listed)", noverlay - first, dir,
                                      noverlay - first - 16);
    if (noverlay > first) rt_log("RomFS overlay: %s checked, %zu files, %.1f MiB hashed in %.2f s", dir,
                                 noverlay - first, hashed / 1048576.0, now_s() - t0);
#undef BAD
}

// ---- player mods (mods.c): every file under an enabled mod's RomFS root, by identity (no hash: a texture pack is
// gigabytes and the player changes it at will; romfs_open still stops on a file that changed after the start)
static uint32_t *folded;   // RomFS indices sorted by name ignoring ASCII case; built on the first mod path that misses

static int folded_order(const void *a, const void *b) {
    return strcasecmp(entry_name(entry(*(const uint32_t *)a)), entry_name(entry(*(const uint32_t *)b)));
}

// The RomFS index of name, or of the RomFS path equal to it but for case (a mod made on Windows, where an emulator's
// folder lookup ignores case); UINT64_MAX for a path the RomFS lacks.
static uint64_t index_any_case(const char *name) {
    uint64_t i = lower(name);
    if (i < file_count && !strcmp(entry_name(entry(i)), name)) return i;
    if (!folded) {
        folded = malloc(file_count * sizeof *folded);
        need(folded != NULL, "case-folded index");
        for (uint64_t k = 0; k < file_count; k++) folded[k] = (uint32_t)k;
        qsort(folded, file_count, sizeof *folded, folded_order);
    }
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcasecmp(entry_name(entry(folded[m])), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo < file_count && !strcasecmp(entry_name(entry(folded[lo])), name) ? folded[lo] : UINT64_MAX;
}

static void overlay_push(Overlay o) {
    if (noverlay == overlay_cap) {
        overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
        overlays = realloc(overlays, overlay_cap * sizeof *overlays);
        need(overlays != NULL, "overlay table");
    }
    overlays[noverlay++] = o;
}

// 1 when path names a file of one game version only (Product.<n> in a name: the resource size table, the actor
// database and other version-bound files); the RomFS lacking it means it is another version's.
static int other_version(const char *path) {
    for (const char *p = strstr(path, ".Product."); p; p = strstr(p + 1, ".Product.")) {
        const char *d = p + strlen(".Product.");
        if (*d >= '0' && *d <= '9') return 1;
    }
    return 0;
}

typedef struct { int k, prio; const char *dir, *rel; } ModWalk;

static void mod_walk(int k, int prio, const char *dir, const char *rel);

// One entry of a mod's RomFS folder (mod_walk).
static int mod_walk_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    const ModWalk *w = ctx;
    ModInfo *m = mods_at(w->k);
    char file[4200], path[4200];
    if (snprintf(file, sizeof file, "%s/%s", w->dir, leaf) >= (int)sizeof file ||
        snprintf(path, sizeof path, "%s%s%s", w->rel, *w->rel ? "/" : "", leaf) >= (int)sizeof path) {
        rt_log("mods: %s: path too long under %s; left out", m->name, w->dir);
        return 1;
    }
    if (kind == PLAT_FS_DIR) {
        mod_walk(w->k, w->prio, file, path);
        return 1;
    }
    if (kind != PLAT_FS_FILE) return 1;
    if (!romfs_path_valid_relative(path)) {
        rt_log("mods: %s: %s is not a RomFS path; left out", m->name, path);
        return 1;
    }
    uint64_t i = index_any_case(path);
    if (i == UINT64_MAX && other_version(path)) {
        m->other_version++;
        return 1;
    }
    PlatFile *f = open_read(file);
    if (!f) {
        rt_log("mods: %s: cannot open %s; left out", m->name, file);
        return 1;
    }
    Identity id = identity_of(f);
    plat_file_close(f);
    char *copy = strdup(file), *rp = strdup(i == UINT64_MAX ? path : entry_name(entry(i)));
    need(copy && rp, "overlay path copy");
    overlay_push((Overlay){i == UINT64_MAX ? ADDED : i, id.size, copy, rp, id, w->prio, w->k + 1});
    return 1;
}

// The files under dir (a mod's RomFS root; rel: the RomFS path of dir, "" at the root) as overlays of mod k.
static void mod_walk(int k, int prio, const char *dir, const char *rel) {
    ModWalk w = {k, prio, dir, rel};
    plat_fs_list(dir, mod_walk_cb, &w);
}

// What overlays compete on: the RomFS index, an added file's path; the higher prio first.
static int contest_order(const void *a, const void *b) {
    const Overlay *x = a, *y = b;
    if (x->index != y->index) return (x->index > y->index) - (x->index < y->index);
    int c = x->index == ADDED ? strcmp(x->name, y->name) : 0;
    return c ? c : y->prio - x->prio;
}

static const char *owner_name(const Overlay *o) {
    return o->mod ? mods_at(o->mod - 1)->name : o->prio ? "romfs_mods" : "the port's patches";
}

// One overlay per path: the highest prio serves it; the others are dropped and counted on their mod.
static void overlay_contest(void) {
    if (!noverlay) return;
    qsort(overlays, noverlay, sizeof *overlays, contest_order);
    size_t kept = 0;
    for (size_t k = 0; k < noverlay; k++) {
        Overlay *o = &overlays[k], *w = kept ? &overlays[kept - 1] : NULL;
        if (w && w->index == o->index && (o->index != ADDED || !strcmp(w->name, o->name))) {
            if (o->mod) {
                ModInfo *m = mods_at(o->mod - 1);
                m->overridden++;
                if (!m->overridden_by[0]) snprintf(m->overridden_by, sizeof m->overridden_by, "%s", owner_name(w));
            } else if (w->mod) {
                rt_log("RomFS overlay: %s from mod %s replaces the one of %s", o->name, owner_name(w), owner_name(o));
            }
            free(o->path);
            free(o->name);
            continue;
        }
        if (o->mod) {
            if (o->index == ADDED) mods_at(o->mod - 1)->added++;
            else mods_at(o->mod - 1)->replaced++;
        }
        overlays[kept++] = *o;
    }
    noverlay = kept;
}

static void mods_overlay_init(void) {
    mods_scan();
    int n = mods_count();
    for (int k = 0; k < n; k++) {
        const ModInfo *m = mods_at(k);
        // a mod with ExeFS patches waits for the rebuild that applies them: half of a mod is worse than none
        if (m->enabled && (m->kind & MOD_ROMFS) && m->problem == MOD_OK) mod_walk(k, 2 + (n - k), m->romfs, "");
    }
}

static void mods_overlay_log(void) {
    for (int k = 0; k < mods_count(); k++) {
        const ModInfo *m = mods_at(k);
        if (!m->enabled || !(m->kind & MOD_ROMFS) || m->problem != MOD_OK) continue;
        rt_log("mods: %s: %u files served (%u replaced, %u added), %u overridden%s%s, %u for another game version",
               m->name, m->replaced + m->added, m->replaced, m->added, m->overridden, m->overridden ? " by " : "",
               m->overridden_by, m->other_version);
    }
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    return (x > y) - (x < y);
}

static int added_order(const void *a, const void *b) {
    return strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name);
}

// Number the added files in path order after the RomFS's own (file_count + k); overlay_contest left one per path.
static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "added files");
    for (size_t k = 0; k < nadded; k++) {
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    rt_log("RomFS overlay: %zu files added to the RomFS", nadded);
}

// The Xbox prompts' files (input_prompts_folder) and whether each was next to romfs-index.bin at start; the one the
// settings asked for at start, the one overlay_init loaded (NULL: the game's own prompts) and why the first did not
// load (ROMFS_PROMPTS_*). The menu compares them with the settings in effect (ui/menu.cpp prompts_note).
static struct { const char *name; int ready; } prompts_files[4];
static const char *prompts_wanted, *prompts_loaded;
static int prompts_off;

const char *romfs_prompts(void) { return prompts_loaded; }

int romfs_prompts_ready(const char *folder) {
    for (int i = 0; i < 4; i++)
        if (folder && prompts_files[i].name && !strcmp(prompts_files[i].name, folder)) return prompts_files[i].ready != 0;
    return 0;
}

int romfs_prompts_off(const char *folder) {
    return folder && prompts_wanted && !strcmp(folder, prompts_wanted) ? prompts_off : ROMFS_PROMPTS_OK;
}

// The prompts' file the settings ask for now; selects it in prompts.c (live: the game's button textures upload again,
// vk_texture.c). Called at start and at every present; it only looks again when a setting changed.
void romfs_prompts_tick(void) {
    static unsigned seen = ~0u;
    static PlatMutex mu = PLAT_MUTEX_INIT;   // the menu thread and the present
    unsigned v = cfg_version();
    if (__atomic_load_n(&seen, __ATOMIC_ACQUIRE) == v) return;
    plat_mutex_lock(&mu);
    if (seen != v) {
        const char *layout = cfg_value("ACR_PAD_LAYOUT");
        const char *want = input_prompts_folder(input_prompts_pick(cfg_value("ACR_PROMPTS"), layout), layout);
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (want && prompts_files[i].name && !strcmp(want, prompts_files[i].name)) slot = i;
        prompts_wanted = want;
        prompts_off = !want ? ROMFS_PROMPTS_OK
                      : slot < 0 || !prompts_files[slot].ready ? ROMFS_PROMPTS_MISSING
                      : prompts_files[slot].ready < 0 ? ROMFS_PROMPTS_FAILED : ROMFS_PROMPTS_OK;
        const char *now = want && prompts_off == ROMFS_PROMPTS_OK ? want : NULL;
        if (now != prompts_loaded) {
            rt_log("prompts: %s", now ? now : "the game's own buttons");
            if (want && !now)
                rt_log("prompts: prompts = %s, but %s is %s (run the setup again); Switch prompts stay",
                       cfg_value("ACR_PROMPTS"), want, prompts_off == ROMFS_PROMPTS_FAILED ? "damaged" : "missing");
        }
        prompts_loaded = now;
        prompts_select(now ? slot : -1);
        __atomic_store_n(&seen, v, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&mu);
}

static void overlay_init(void) {
    // The Xbox and PlayStation prompts' files (input_prompts_folder, tools/make_prompts.py), all loaded: prompts.c puts
    // the one the settings ask for in place of the game's button atlas when the game uploads it, and a change of the
    // setting uploads the atlas again (romfs_prompts_tick). ready: 1 loaded, -1 damaged, 0 missing.
    char path[2100];
    static const char *const layouts[2] = {"nintendo", "xbox"};   // one of each file
    for (int i = 0; i < 4; i++) {
        prompts_files[i].name = input_prompts_folder(i < 2 ? PROMPTS_XBOX : PROMPTS_PLAYSTATION, layouts[i & 1]);
        int there = snprintf(path, sizeof path, "%s/%s", index_dir, prompts_files[i].name) < (int)sizeof path &&
                    plat_file_exists(path);
        prompts_files[i].ready = !there ? 0 : prompts_load(i, path) ? 1 : -1;
        if (prompts_files[i].ready < 0) rt_log("prompts: %s is damaged (run the setup again)", path);
    }
    romfs_prompts_tick();
    // ACR_ROMFS_PATCHES unset: romfs-patches next to romfs-index.bin if it has a manifest.
    const char *patches = cfg_get("ACR_ROMFS_PATCHES"), *mods = cfg_get("ACR_ROMFS_MODS");
    char plain[2100];
    if (!patches && snprintf(plain, sizeof plain, "%s/romfs-patches/manifest.tsv", index_dir) < (int)sizeof plain &&
        plat_file_exists(plain)) {
        plain[strlen(plain) - strlen("/manifest.tsv")] = 0;
        patches = plain;
    }
    if (patches && *patches && strcmp(patches, "0")) overlay_load(patches, 0);
    if (mods && *mods && strcmp(mods, "0")) overlay_load(mods, 1);
    mods_overlay_init();
    overlay_contest();
    mods_overlay_log();
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    for (size_t k = 1; k < noverlay; k++)   // overlay_contest left one per file
        if (overlays[k].index == overlays[k - 1].index)
            rt_fatal("RomFS overlay: %s and %s replace the same file %s", overlays[k - 1].path, overlays[k].path,
                     overlays[k].name);
}

// ---- start
// romfs-index.bin next to AgeOfCalamityRecomp, or ACR_ROMFS_INDEX.
static void index_path(char *out, size_t cap) {
    const char *e = cfg_get("ACR_ROMFS_INDEX");
    if (e && *e) {
        need(snprintf(out, cap, "%s", e) < (int)cap, "index path");
        return;
    }
    need(plat_exe_path(out, cap), "executable path");
    char *sep = plat_last_sep(out);
    size_t k = sep ? (size_t)(sep - out) + 1 : 0;
    need(k + 16 < cap, "executable path");
    strcpy(out + k, "romfs-index.bin");
}

static void load_index(void) {
    char path[2100];
    index_path(path, sizeof path);
    snprintf(index_dir, sizeof index_dir, "%s", path);
    char *sep = plat_last_sep(index_dir);
    if (sep) *sep = 0;
    else strcpy(index_dir, ".");
    uint64_t sz = 0;
    index_bytes = plat_file_read(path, &sz);
    if (!index_bytes) rt_fatal("RomFS index %s missing (tools/build_game.py writes it next to AgeOfCalamityRecomp)", path);
    need(sz >= 24 && sz < (128 << 20), "index size");
    index_size = (size_t)sz;
    uint8_t d[32];
    char hex[65];
    rt_sha256(index_bytes, index_size, d);
    hex32(d, hex);
    if (strcmp(hex, ROMFS_INDEX_SHA256))
        rt_fatal("RomFS index %s has SHA-256 %s, this build expects %s (rebuild with tools/build_game.py)", path, hex,
                 ROMFS_INDEX_SHA256);
    need(!memcmp(index_bytes, "ACRFS1", 6), "index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count == ROMFS_FILE_COUNT && file_count < 1000000 && strings_offset == 24 + file_count * 24 &&
         strings_offset < index_size, "index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < file_count; i++) {
        Entry e = entry(i);
        need(e.offset <= ROMFS_IMAGE_SIZE && e.size <= ROMFS_IMAGE_SIZE - e.offset, "file bounds");
        need(e.name < index_size - strings_offset && e.len > 1 && e.len <= index_size - strings_offset - e.name,
             "name bounds");
        const char *name = entry_name(e);
        need(name[e.len - 1] == 0 && strlen(name) == e.len - 1 && (!previous || strcmp(previous, name) < 0),
             "sorted paths");
        previous = name;
    }
    rt_log("RomFS index %s: SHA-256 ok, %llu files, %zu bytes", path, (unsigned long long)file_count, index_size);
}

static void init_once(void *param) {
    (void)param;
#ifdef ROMFS_MISSING
    rt_fatal("this build has no RomFS: tools/build_game.py found no index.json in its --romfs directory");
#else
    double t0 = now_s();
    load_index();
    const char *dir = rt_config()->romfs;
    need(snprintf(image_file, sizeof image_file, "%s/romfs.bin", dir) < (int)sizeof image_file, "image path");
    image = open_read(image_file);
    if (!image) rt_fatal("RomFS image %s missing; put romfs.bin into the romfs folder next to AgeOfCalamityRecomp or set ACR_ROMFS",
                         image_file);
    PlatFileId sz;
    need(plat_file_id(image, &sz), "image size");
    if (sz.size != ROMFS_IMAGE_SIZE)
        rt_fatal("RomFS image %s has %llu bytes, the index was built for %llu", image_file,
                 (unsigned long long)sz.size, (unsigned long long)ROMFS_IMAGE_SIZE);
    rt_log("RomFS image %s: size ok (%llu bytes)", image_file, (unsigned long long)ROMFS_IMAGE_SIZE);
    // Parse the RomFS header: the four metadata tables (directory and file hash and meta) sit together at the end of the
    // image; the fs service copies them into the caller's cache buffer on MountRom.
    if (read_at(image, romfs_header, sizeof romfs_header, 0) == (int64_t)sizeof romfs_header &&
        ((uint32_t *)romfs_header)[0] == 0x50u) {
        uint64_t *h = (uint64_t *)romfs_header;
        uint64_t dir_hash_off = h[1], dir_hash_size = h[2], dir_meta_off = h[3], dir_meta_size = h[4];
        uint64_t file_hash_off = h[5], file_hash_size = h[6], file_meta_off = h[7], file_meta_size = h[8];
        uint64_t lo = dir_hash_off, hi = file_meta_off + file_meta_size;
        if (dir_hash_off <= dir_meta_off && dir_meta_off <= file_hash_off && file_hash_off <= file_meta_off &&
            hi <= ROMFS_IMAGE_SIZE && lo < hi) {
            romfs_meta_off = lo;
            romfs_meta_size = hi - lo;
            rt_log("RomFS metadata: %#llx bytes at %#llx (dir hash %#llx+%#llx, dir meta %#llx+%#llx, file hash "
                   "%#llx+%#llx, file meta %#llx+%#llx, data %#llx)",
                   (unsigned long long)romfs_meta_size, (unsigned long long)romfs_meta_off,
                   (unsigned long long)dir_hash_off, (unsigned long long)dir_hash_size,
                   (unsigned long long)dir_meta_off, (unsigned long long)dir_meta_size,
                   (unsigned long long)file_hash_off, (unsigned long long)file_hash_size,
                   (unsigned long long)file_meta_off, (unsigned long long)file_meta_size, (unsigned long long)h[9]);
        } else {
            rt_log("RomFS metadata: header offsets out of bounds; MountRom hands out no cache");
        }
    } else {
        rt_log("RomFS metadata: unexpected header; MountRom hands out no cache");
    }
    // The game reads the RomFS image at the fixed guest address ROMFS_BASE (recomp/layout.py): it parses the image
    // directly, no nn::fs calls. The range lies inside the module window reservation (PAGE_NOACCESS), so pages are
    // committed and filled from the image on the first guest read fault (romfs_read_fault). Only when the layout has
    // such a region: Age of Calamity mounts its RomFS through nn::fs::MountRom and reads files, so ROMFS_SIZE is 0.
#if ROMFS_SIZE
    plat_mem_read_fault_hook(romfs_read_fault);
#else
    (void)romfs_read_fault;
#endif
    verify_image(index_dir);
    const char *mm = cfg_get("ACR_ROMFS_MMAP");
    if (mm && strcmp(mm, "0")) {
        const void *v = plat_file_map(image);
        if (v) {
            image_map = v;
            rt_log("RomFS: mapped read path enabled");
        } else {
            rt_log("RomFS: mapping failed; using positional reads");
        }
    }
    overlay_init();
    rt_log("RomFS ready: %llu files, %zu overlays, %zu added, %.2f s", (unsigned long long)file_count, noverlay,
           nadded, now_s() - t0);
#endif
    return;
}

void fs_init(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, init_once, NULL);
}

// ---- the RomFS interface (fs.h)
void fs_trace(const char *what, const char *path) {
    static double t0;
    diag_open(what, path);
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return file_count; }

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    if (offset) *offset = e.offset;
    if (size) *size = e.size;
    return entry_name(e);
}

const char *romfs_image_path(void) { return image_file; }

size_t romfs_overlay_count(void) { return noverlay; }

int romfs_patches_loaded(void) { return patches_loaded; }

const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    if (k >= noverlay) return NULL;
    *file = overlays[k].path;
    *size = overlays[k].size;
    *is_added = overlays[k].index >= file_count;
    return overlays[k].name;
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') ||
        strchr(name, ':'))
        return FS_R_INVALID;
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(name, mount_name)) rt_fatal("second RomFS mount %s (%s is mounted)", name, mount_name);
    strcpy(mount_name, name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> RomFS", name);
    return 0;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    uint32_t ov = overlay_slot(i);
    PlatFile *f = NULL;
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        f = open_read(o->path);
        Identity id;
        if (f) id = identity_of(f);
        if (!f || memcmp(&id, &o->id, sizeof id))
            rt_fatal("RomFS overlay %s is missing or changed since the start check (restart the game)", o->path);
    }
    plat_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            handle_ovl[j] = ov;
            handle_file[j] = f;
            *out = ROMFS_HANDLE_BASE + j;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

// The handle's index row (an added file's is empty: its bytes are all in its overlay), and where its bytes come from:
// the overlay file, or the original's range of the image.
static int handle_source(uint64_t h, PlatFile **f, uint64_t *base, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock_shared(&lock);
    uint64_t i = handles[j];
    uint32_t ov = handle_ovl[j];
    PlatFile *of = handle_file[j];
    plat_mutex_unlock_shared(&lock);
    if (!i) return 0;
    if (ov) {
        *f = of;
        *base = 0;
        *size = overlays[ov - 1].size;
    } else {
        Entry e = entry(i - 1);
        *f = image;
        *base = e.offset;
        *size = e.size;
    }
    return 1;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    uint64_t base, fsize;
    if (!out || offset < 0 || !handle_source(h, &f, &base, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0;
    if (f == image && image_map) {
        memcpy(buffer, image_map + base + (uint64_t)offset, (size_t)n);
        done = n;
    }
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = read_at(f, (uint8_t *)buffer + done, chunk, base + (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)(base + offset + done));
        done += (uint64_t)r;
    }
    diag_read(done);
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!out || !handle_source(h, &f, &base, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!handle_source(h, &f, &base, &size))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *of = handle_ovl[j] ? handle_file[j] : NULL;
    handles[j] = 0;
    handle_ovl[j] = 0;
    handle_file[j] = NULL;
    plat_mutex_unlock(&lock);
    if (of) plat_file_close(of);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (folder_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// Direct children of the RomFS directory p ("" is the root): add(name, kind, size) for each, in index order. Only the
// index files are listed; files a mod added under the directory do not appear (Astral Chain enumerates the base RomFS
// at boot; mods replace or add whole archives by path, not entries).
void romfs_dir_list(const char *path, int (*add)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    fs_init();
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) {
        // The mount root ("Contents:/" or "Contents:///") has no relative path of its own; list the top level.
        size_t n = strlen(mount_name);
        const char *q = path && !strncmp(path, mount_name, n) && path[n] == ':' ? path + n + 1 : NULL;
        if (!q || *q != '/') return;
        while (*q == '/') q++;
        if (*q) return;
        p = "";
    }
    if (!add) return;
    size_t dn = strlen(p);
    while (dn && p[dn - 1] == '/') dn--;   // "Contents:/st8/" and "Contents:/st8" name the same directory
    char last[769];   // one child name: nn::fs caps names at 768 bytes
    int have = 0;
    for (uint64_t i = 0; i < file_count; i++) {
        const char *name = entry_name(entry(i));
        if (dn && (strncmp(name, p, dn) || name[dn] != '/')) continue;   // not under the directory
        const char *child = name + (dn ? dn + 1 : 0);
        const char *slash = strchr(child, '/');
        size_t cn = slash ? (size_t)(slash - child) : strlen(child);
        if (cn >= sizeof last) continue;   // over 768 bytes: nn::fs cannot name it
        if (have && !strncmp(last, child, cn) && !last[cn]) continue;    // same child, already listed
        memcpy(last, child, cn);
        last[cn] = 0;
        have = 1;
        const Entry e = entry(i);
        if (!add(last, slash ? PLAT_FS_DIR : PLAT_FS_FILE, slash ? 0 : e.size, ctx)) return;
    }
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
//
// The fs service writes the RomFS metadata (the four directory and file tables, with offsets rebased onto the cache)
// into the cache buffer, and the SDK's RomFsFileSystem parses files from it. Nothing on the host does that, so the
// mount hands the tables over here; without them the SDK reports an empty RomFS and the game's file table stays empty
// (its loader then never finds a single resource).
static void ov_mount_rom(Cpu *c) {
    const char *name = arg_p(c, 0);
    void *cache = arg_p(c, 1);
    uint64_t size = c->x[2];
    if (cache && romfs_meta_size) {
        uint64_t *h = (uint64_t *)romfs_header;   // image copy; the cache copy gets rebased offsets
        uint64_t dir_hash_size = h[2], dir_meta_size = h[4], file_hash_size = h[6];
        uint64_t dir_hash_off = sizeof romfs_header;
        uint64_t dir_meta_off = dir_hash_off + dir_hash_size;
        uint64_t file_hash_off = dir_meta_off + dir_meta_size;
        uint64_t file_meta_off = file_hash_off + file_hash_size;
        uint64_t need_bytes = file_meta_off + h[8];
        if (size < need_bytes) {
            rt_logf(" -> cache too small (%#llx < %#llx); no metadata copied", (unsigned long long)size,
                    (unsigned long long)need_bytes);
        } else {
            uint8_t *copy = malloc((size_t)need_bytes);
            int ok = copy != 0;
            if (ok) {
                memcpy(copy, romfs_header, sizeof romfs_header);
                uint64_t *dh = (uint64_t *)copy;
                dh[1] = dir_hash_off;
                dh[3] = dir_meta_off;
                dh[5] = file_hash_off;
                dh[7] = file_meta_off;
                ok = read_at(image, copy + dir_hash_off, (uint32_t)dir_hash_size, h[1]) == (int64_t)dir_hash_size &&
                     read_at(image, copy + dir_meta_off, (uint32_t)dir_meta_size, h[3]) == (int64_t)dir_meta_size &&
                     read_at(image, copy + file_hash_off, (uint32_t)file_hash_size, h[5]) == (int64_t)file_hash_size &&
                     read_at(image, copy + file_meta_off, (uint32_t)h[8], h[7]) == (int64_t)h[8];
            }
            if (ok) {
                mem_thaw((uintptr_t)cache, need_bytes);
                memcpy(cache, copy, (size_t)need_bytes);
                free(copy);
            } else {
                free(copy);
                rt_logf("ov_mount_rom: metadata build failed");
            }
        }
    }
    int rc = romfs_mount(name);
    ret_x(c, rc);
}
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): the buffer MountRom needs for the metadata tables
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (!out) {
        ret_x(c, FS_R_INVALID);
        return;
    }
    uint64_t bytes = romfs_meta_size ? (sizeof romfs_header + romfs_meta_size + 0xFFFu) & ~0xFFFull : 0;
    *out = bytes;
    ret_x(c, 0);
}
// void nn::fs::InitializeForApplication(): SDK-internal; opens the index instead of the fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
