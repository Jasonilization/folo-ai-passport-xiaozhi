// Random-access reader for the packed WAD (doom1.wad.pack, embedded in flash).
// Format: "WADP" | u32 numlumps | u32 dir_off | dir[numlumps]{name[8],u32 off,
// u32 csize, u32 osize} | blob of per-lump raw-deflate streams.
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "miniz.h"

// Embedded by CMake EMBED_FILES (symbol names derive from the file name).
extern const uint8_t doom1_wad_pack_start[] asm("_binary_doom1_wad_pack_start");
extern const uint8_t doom1_wad_pack_end[] asm("_binary_doom1_wad_pack_end");

#define WADP_MAX_LUMPS 2048

typedef struct {
    char name[8];
    uint32_t off, csize, osize;
} pack_lump_t;

static pack_lump_t s_dir[WADP_MAX_LUMPS];
static int s_numlumps = 0;
static const uint8_t* s_blob = NULL;

// Small LRU cache of decompressed lumps (Doom reads the same lumps often).
#define WADP_CACHE_SLOTS 6
typedef struct {
    int lump;
    uint8_t* data;
    uint32_t size;
    uint32_t age;
} cache_slot_t;
static cache_slot_t s_cache[WADP_CACHE_SLOTS];
static uint32_t s_age = 0;

int wad_pack_init(void) {
    const uint8_t* p = doom1_wad_pack_start;
    if (memcmp(p, "WADP", 4) != 0) {
        return -1;
    }
    uint32_t num, dir_off;
    memcpy(&num, p + 4, 4);
    memcpy(&dir_off, p + 8, 4);
    if (num > WADP_MAX_LUMPS) {
        return -1;
    }
    s_numlumps = (int)num;
    s_blob = p + dir_off;
    const uint8_t* d = p + 12;
    for (uint32_t i = 0; i < num; i++) {
        memcpy(s_dir[i].name, d, 8);
        memcpy(&s_dir[i].off, d + 8, 4);
        memcpy(&s_dir[i].csize, d + 12, 4);
        memcpy(&s_dir[i].osize, d + 16, 4);
        d += 24;
    }
    for (int i = 0; i < WADP_CACHE_SLOTS; i++) {
        s_cache[i].lump = -1;
        s_cache[i].data = NULL;
    }
    return 0;
}

int wad_pack_numlumps(void) { return s_numlumps; }

// Copy lump metadata (name/size) for index i.
void wad_pack_lump_info(int i, char* name8, uint32_t* osize) {
    if (i < 0 || i >= s_numlumps) {
        if (name8) name8[0] = 0;
        if (osize) *osize = 0;
        return;
    }
    if (name8) memcpy(name8, s_dir[i].name, 8);
    if (osize) *osize = s_dir[i].osize;
}

// Decompress lump i into dest (must be >= osize). Returns osize or -1.
int wad_pack_read(int i, uint8_t* dest) {
    if (i < 0 || i >= s_numlumps) {
        return -1;
    }
    // cache hit?
    for (int s = 0; s < WADP_CACHE_SLOTS; s++) {
        if (s_cache[s].lump == i) {
            s_cache[s].age = ++s_age;
            memcpy(dest, s_cache[s].data, s_dir[i].osize);
            return (int)s_dir[i].osize;
        }
    }
    const uint8_t* src = s_blob + s_dir[i].off;
    mz_ulong out_len = s_dir[i].osize;
    int r = mz_uncompress(dest, &out_len, src, s_dir[i].csize);
    if (r != MZ_OK) {
        return -1;
    }
    // store in LRU (evict oldest)
    int slot = 0;
    for (int s = 1; s < WADP_CACHE_SLOTS; s++) {
        if (s_cache[s].age < s_cache[slot].age) slot = s;
    }
    if (s_cache[slot].data == NULL) {
        s_cache[slot].data = (uint8_t*)malloc(s_dir[i].osize);
        if (s_cache[slot].data == NULL) {
            return (int)out_len;  // decoded fine, just not cached
        }
    }
    memcpy(s_cache[slot].data, dest, out_len);
    s_cache[slot].lump = i;
    s_cache[slot].size = (uint32_t)out_len;
    s_cache[slot].age = ++s_age;
    return (int)out_len;
}
