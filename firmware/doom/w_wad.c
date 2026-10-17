//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2026 FM-1 Doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Lump directory and lump access over a DPK image (tools/wadpack.py).
//	The image sits in memory-mapped flash: a raw lump is handed out in
//	place, a deflated one is unpacked into the zone and cached there
//	under the caller's tag, as the original did with lumps read from disk.
//

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"

#include "i_system.h"
#include "i_video.h"
#include "inflate.h"
#include "z_zone.h"

#include "w_wad.h"

const lumpinfo_t *lumpinfo;
unsigned int numlumps = 0;

static const byte *part_a, *part_b, *part_c;
static uint32_t end_a, end_b;

// Unpacked copies in the zone (their user pointers), NULL when not cached
static void **lumpcache;

// Hash chains: the newest lump of a name first, as a later WAD overrode
static short *lumphash, *lumpnext;
#define HASH_SIZE 256

// A patch of no size, for a missing menu or intermission graphic
static const short empty_patch[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

unsigned int W_LumpNameHash(const char *s)
{
    // This is the djb2 string hash function, modded to work on strings
    // that have a maximum length of 8.

    unsigned int result = 5381;
    unsigned int i;

    for (i=0; i < 8 && s[i] != '\0'; ++i)
    {
        result = ((result << 5) ^ result ) ^ toupper((int)s[i]);
    }

    return result;
}

static uint32_t crc32_of(const byte *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int k;

    for (i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#ifdef Z_STATS
// Host tests: the packed lumps each frame uses (W_FrameStats), and how often
// one had to be unpacked again
static unsigned char used_this_frame[4096];
static int frame_bytes, frame_misses;
static int unpack_count[4096];
static long unpack_bytes[4096];
int w_stat_max_frame_bytes, w_stat_misses, w_stat_frames, w_stat_max_frame_misses;

static void W_NoteUse(int lump, int miss)
{
    if (lump < 4096 && !used_this_frame[lump])
    {
        used_this_frame[lump] = 1;
        frame_bytes += lumpinfo[lump].size;
    }
    if (miss)
    {
        frame_misses++;
        if (lump < 4096)
        {
            unpack_count[lump]++;
            unpack_bytes[lump] += lumpinfo[lump].size;
        }
    }
}

void W_DumpUnpacks(int top)
{
    int i, k;

    for (k = 0; k < top; k++)
    {
        int best = -1;

        for (i = 0; i < (int) numlumps && i < 4096; i++)
            if (unpack_bytes[i] && (best < 0 || unpack_bytes[i] > unpack_bytes[best]))
                best = i;
        if (best < 0)
            break;
        fprintf(stderr, "  %-8.8s %6d B x %5d = %8ld KB\n", lumpinfo[best].name, lumpinfo[best].size,
                unpack_count[best], unpack_bytes[best] / 1024);
        unpack_bytes[best] = 0;
    }
}

void W_FrameStats(void)
{
    if (frame_bytes > w_stat_max_frame_bytes)
        w_stat_max_frame_bytes = frame_bytes;
    if (frame_misses > w_stat_max_frame_misses)
        w_stat_max_frame_misses = frame_misses;
    w_stat_misses += frame_misses;
    w_stat_frames++;
    frame_bytes = 0;
    frame_misses = 0;
    memset(used_this_frame, 0, sizeof(used_this_frame));
}
#endif

const byte *W_LumpData(unsigned int lump)
{
    uint32_t pos = lumpinfo[lump].position;

    if (pos < end_a)
        return part_a + pos;
    if (pos < end_b)
        return part_b + (pos - end_a);
    return part_c + (pos - end_b);
}

int W_InitPack(const void *a, const void *b, const void *c)
{
    const dpk_header_t *h = a;

    if (memcmp(h->magic, "FDPK", 4) != 0 || h->version != 3)
        return -1;
    if (h->end_a > h->end_b || h->end_b > h->total_size
     || (h->end_a < h->end_b && b == NULL) || (h->end_b < h->total_size && c == NULL))
        return -2;
    part_a = a;
    part_b = b;
    part_c = c;
    end_a = h->end_a;
    end_b = h->end_b;
    if (crc32_of(part_a + sizeof(*h), end_a - sizeof(*h)) != h->crc_a)
        return -3;
    if (crc32_of(part_b, end_b - end_a) != h->crc_b)
        return -4;
    if (crc32_of(part_c, h->total_size - end_b) != h->crc_c)
        return -5;
    lumpinfo = (const lumpinfo_t *) (part_a + h->dir_offset);
    numlumps = h->numlumps;
    return 0;
}

void W_GenerateHashTable(void)
{
    unsigned int i;

    lumpcache = Z_Malloc(numlumps * sizeof(*lumpcache), PU_STATIC, NULL);
    memset(lumpcache, 0, numlumps * sizeof(*lumpcache));
    lumphash = Z_Malloc(HASH_SIZE * sizeof(*lumphash), PU_STATIC, NULL);
    lumpnext = Z_Malloc(numlumps * sizeof(*lumpnext), PU_STATIC, NULL);
    for (i = 0; i < HASH_SIZE; i++)
        lumphash[i] = -1;
    for (i = 0; i < numlumps; i++)
    {
        unsigned int h = W_LumpNameHash(lumpinfo[i].name) % HASH_SIZE;

        lumpnext[i] = lumphash[h];
        lumphash[h] = (short) i;
    }
}

//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (char* name)
{
    int i;

    for (i = lumphash[W_LumpNameHash(name) % HASH_SIZE]; i >= 0; i = lumpnext[i])
    {
        if (!strncasecmp(lumpinfo[i].name, name, 8))
            return i;
    }

    return -1;
}

//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (char* name)
{
    int	i;

    i = W_CheckNumForName (name);

    if (i < 0)
    {
        I_Error ("W_GetNumForName: %.8s not found!", name);
    }

    return i;
}

//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (unsigned int lump)
{
    if (lump >= numlumps)
    {
        I_Error ("W_LumpLength: %i >= numlumps", lump);
    }

    return lumpinfo[lump].size;
}

//
// W_ReadLump
// Loads the lump into the given buffer,
//  which must be >= W_LumpLength().
//
void W_ReadLump(unsigned int lump, void *dest)
{
    const lumpinfo_t *l;

    if (lump >= numlumps)
    {
        I_Error ("W_ReadLump: %i >= numlumps", lump);
    }

    l = &lumpinfo[lump];
    if (l->codec == DPK_DEFLATE)
    {
        if (inflate_raw(dest, l->size, W_LumpData(lump), l->csize) != (long) l->size)
            I_Error("W_ReadLump: %.8s does not unpack", l->name);
    }
    else
    {
        memcpy(dest, W_LumpData(lump), l->size);
    }
}

//
// W_CacheLumpNum
//
// A raw lump is returned in place, in flash: the caller only reads it.
// A packed one is unpacked into the zone and tagged; the pointer to it
// is valid until W_ReleaseLumpNum() (or forever with PU_STATIC).
//
void *W_CacheLumpNum(int lumpnum, int tag)
{
    const lumpinfo_t *l;
    void *result;

    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_CacheLumpNum: %i >= numlumps", lumpnum);
    }

    l = &lumpinfo[lumpnum];
    if (l->codec != DPK_DEFLATE)
    {
        return (void *) W_LumpData(lumpnum);
    }
#ifdef Z_STATS
    W_NoteUse(lumpnum, lumpcache[lumpnum] == NULL);
#endif

    if (lumpcache[lumpnum] != NULL)
    {
        // Already cached, so just switch the zone tag.

        result = lumpcache[lumpnum];
        Z_ChangeTag(result, tag);
    }
    else
    {
        // Not yet loaded, so load it now

        Z_Malloc(W_LumpLength(lumpnum), tag, &lumpcache[lumpnum]);
	W_ReadLump (lumpnum, lumpcache[lumpnum]);
        result = lumpcache[lumpnum];
    }

    return result;
}

void *W_LumpScratch(unsigned int lump)
{
    const lumpinfo_t *l;
    byte *buf;

    if (lump >= numlumps)
    {
        I_Error ("W_LumpScratch: %i >= numlumps", lump);
    }
    l = &lumpinfo[lump];
    if (l->codec != DPK_DEFLATE)
    {
        return (void *) W_LumpData(lump);
    }
    buf = I_ScratchBuffer(l->size);
    if (buf == NULL)
    {
        I_Error ("W_LumpScratch: %.8s is too big", l->name);
    }
    W_ReadLump(lump, buf);
    return buf;
}

//
// W_CacheLumpName
// A missing name gives an empty patch (a menu or intermission graphic
// that the image leaves out draws as nothing).
//
void *W_CacheLumpName(char *name, int tag)
{
    int lump = W_CheckNumForName(name);

    if (lump < 0)
    {
        return (void *) empty_patch;
    }
    return W_CacheLumpNum(lump, tag);
}

//
// Release a lump back to the cache, so that it can be reused later
// without having to read from disk again, or alternatively, discarded
// if we run out of memory.
//

void W_ReleaseLumpNum(int lumpnum)
{
    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_ReleaseLumpNum: %i >= numlumps", lumpnum);
    }

    if (lumpinfo[lumpnum].codec == DPK_DEFLATE && lumpcache[lumpnum] != NULL)
    {
        Z_ChangeTag(lumpcache[lumpnum], PU_CACHE);
    }
}

void W_ReleaseLumpName(char *name)
{
    int lump = W_CheckNumForName(name);

    if (lump >= 0)
    {
        W_ReleaseLumpNum(lump);
    }
}
