/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2001 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Handles WAD file header, directory, lump I/O.
 *
 *-----------------------------------------------------------------------------
 */

// use config.h if autoconf made one -- josh
#include <ctype.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <string.h>
#include <fcntl.h>
#include "doomtype.h"
#include "doom_iwad.h"
// ESP32: WAD comes from the embedded packed file, not a memory blob.
extern int wad_pack_init(void);
extern int wad_pack_numlumps(void);
extern void wad_pack_lump_info(int i, char* name8, uint32_t* osize);
extern int wad_pack_read(int i, uint8_t* dest);
#define WADP_SCRATCH_SLOTS 8
static uint32_t s_scratch_size[WADP_SCRATCH_SLOTS];

#include "w_wad.h"
#include "lprintf.h"

//
// GLOBALS
//
void ExtractFileBase (const char *path, char *dest)
{
    const char *src = path + strlen(path) - 1;
    int length;

    // back up until a \ or the start
    while (src != path && src[-1] != ':' // killough 3/22/98: allow c:filename
           && *(src-1) != '\\'
           && *(src-1) != '/')
    {
        src--;
    }

    // copy up to eight characters
    memset(dest,0,8);
    length = 0;

    while ((*src) && (*src != '.') && (++length<9))
    {
        *dest++ = toupper(*src);
        src++;
    }
    /* cph - length check removed, just truncate at 8 chars.
   * If there are 8 or more chars, we'll copy 8, and no zero termination
   */
}

//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// All files are optional, but at least one file must be
//  found (PWAD, if all required lumps are present).
// Files with a .wad extension are wadlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
//
// Reload hack removed by Lee Killough
// CPhipps - source is an enum
//
// proff - changed using pointer to wadfile_info_t
static void W_AddFile()
{
    if (wad_pack_init() != 0)
        I_Error("W_AddFile: packed WAD missing/corrupt");
}

//Return -1 if not found.
//Pure, so it must not write anything: with an out pointer GCC tail called it
//with the address of a dead local, which overwrote a saved register.

static int PUREFUNC FindLumpByName(const char* name)
{
    int_64_t nameint = 0;
    strncpy((char*)&nameint, name, 8);

    int num = wad_pack_numlumps();
    for (int i = num - 1; i >= 0; i--)
    {
        char n8[8];
        wad_pack_lump_info(i, n8, NULL);
        int_64_t nameint2 = 0;
        memcpy(&nameint2, n8, 8);
        if (nameint == nameint2)
            return i;
    }

    return -1;
}

static void PUREFUNC FindLumpByNum(int num, char* name8, uint32_t* osize)
{
    wad_pack_lump_info(num, name8, osize);
}

//
// W_CheckNumForName
// Returns -1 if name not found.
//
// Rewritten by Lee Killough to use hash table for performance. Significantly
// cuts down on time -- increases Doom performance over 300%. This is the
// single most important optimization of the original Doom sources, because
// lump name lookup is used so often, and the original Doom used a sequential
// search. For large wads with > 1000 lumps this meant an average of over
// 500 were probed during every search. Now the average is under 2 probes per
// search. There is no significant benefit to packing the names into longwords
// with this new hashing algorithm, because the work to do the packing is
// just as much work as simply doing the string comparisons with the new
// algorithm, which minimizes the expected number of comparisons to under 2.
//
// killough 4/17/98: add namespace parameter to prevent collisions
// between different resources such as flats, sprites, colormaps
//

int PUREFUNC W_CheckNumForName(const char *name)
{
    return FindLumpByName(name);
}

// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int PUREFUNC W_GetNumForName(const char* name)     // killough -- const added
{
    int i = W_CheckNumForName (name);

    if (i == -1)
        I_Error("W_GetNumForName: %.8s not found", name);

    return i;
}

const char* PUREFUNC W_GetNameForNum(int lump)
{
    // Return a pointer to a static copy of the lump name.
    static char s_name[9];
    uint32_t osize = 0;
    FindLumpByNum(lump, s_name, &osize);
    s_name[8] = 0;
    if (lump < 0 || lump >= wad_pack_numlumps())
        return NULL;
    return s_name;
}



// W_Init
// Loads each of the files in the wadfiles array.
// All files are optional, but at least one file
//  must be found.
// Files with a .wad extension are idlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
// Lump names can appear multiple times.
// The name searcher looks backwards, so a later file
//  does override all earlier ones.
//
// CPhipps - modified to use the new wadfiles array
//

void W_Init(void)
{
    // CPhipps - start with nothing

    W_AddFile();
}

//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//

int PUREFUNC W_LumpLength(int lump)
{
    uint32_t osize = 0;
    char n8[8];
    FindLumpByNum(lump, n8, &osize);
    if (osize || lump >= 0)
    {
        return (int)osize;
    }

    I_Error ("W_LumpLength: %d >= numlumps",lump);

    return 0;
}

// Decompressed lump storage: a small set of static buffers; the LRU lives in
// wad_pack.c, so W_GetLumpPtr returns a pointer into the pack cache.
static const void* PUREFUNC W_GetLumpPtr(int lump)
{
    // wad_pack_read needs a destination; use the cache slot buffer directly by
    // decoding into a temp then letting the cache own it. To keep it simple we
    // decode into a per-call static round-robin buffer.
    static uint8_t* s_scratch[WADP_SCRATCH_SLOTS];
    static int s_scratch_idx = 0;
    if (lump < 0 || lump >= wad_pack_numlumps())
        return NULL;
    uint32_t osize = 0;
    char n8[8];
    FindLumpByNum(lump, n8, &osize);
    if (osize == 0)
        return NULL;
    int slot = s_scratch_idx++ % WADP_SCRATCH_SLOTS;
    if (s_scratch[slot] == NULL || s_scratch_size[slot] < osize) {
        free(s_scratch[slot]);
        s_scratch[slot] = (uint8_t*)malloc(osize);
        s_scratch_size[slot] = osize;
    }
    if (s_scratch[slot] == NULL)
        return NULL;
    if (wad_pack_read(lump, s_scratch[slot]) < 0)
        return NULL;
    return s_scratch[slot];
}

const void* PUREFUNC W_CacheLumpNum(int lump)
{
    return W_GetLumpPtr(lump);
}
