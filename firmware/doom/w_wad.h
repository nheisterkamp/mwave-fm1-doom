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
//	WAD I/O functions, over a DPK image in flash (tools/wadpack.py)
//	instead of WAD files.
//


#ifndef __W_WAD__
#define __W_WAD__

#include "doomtype.h"
#include "d_mode.h"

//
// TYPES
//

// One directory entry of the DPK image, read in place from flash.
// "position" is an offset into the image (which may be split in two parts).

typedef struct
{
    char	name[8];
    uint32_t	position;
    int		size;		// unpacked size
    uint32_t	csize;		// stored size
    uint32_t	codec;		// DPK_RAW, DPK_DEFLATE, DPK_ADPCM
} lumpinfo_t;

#define DPK_RAW     0
#define DPK_DEFLATE 1
#define DPK_ADPCM   2

typedef struct
{
    char	magic[4];	// "FDPK"
    uint32_t	version;
    uint32_t	numlumps;
    uint32_t	dir_offset;
    uint32_t	total_size;
    uint32_t	end_a;		// image offset where part b begins
    uint32_t	end_b;		// image offset where part c begins
    uint32_t	crc_a;		// crc32 of part a after this header
    uint32_t	crc_b;		// crc32 of part b
    uint32_t	crc_c;		// crc32 of part c
} dpk_header_t;

extern const lumpinfo_t *lumpinfo;
extern unsigned int numlumps;

// The image, in up to three parts that live apart: part a holds the header
// and the directory, parts b and c (NULL when empty) the bytes from end_a
// and from end_b on.  Returns 0, or why the image is unusable.
int     W_InitPack(const void *part_a, const void *part_b, const void *part_c);

int	W_CheckNumForName (char* name);
int	W_GetNumForName (char* name);

int	W_LumpLength (unsigned int lump);
void    W_ReadLump (unsigned int lump, void *dest);

// Raw lumps are returned in place (read only); packed lumps are unpacked
// into the zone with the tag, as before.
void*	W_CacheLumpNum (int lump, int tag);
void*	W_CacheLumpName (char* name, int tag);

// A lump to read once, right away (the map loader): in place if raw, else
// unpacked into the screen buffer, which is free while a level loads. Valid
// until the next call; nothing to release (W_ReleaseLumpNum does nothing).
void   *W_LumpScratch(unsigned int lump);

// The stored bytes of a lump in place (for DPK_ADPCM sounds)
const byte *W_LumpData(unsigned int lump);

void    W_GenerateHashTable(void);

extern unsigned int W_LumpNameHash(const char *s);

void    W_ReleaseLumpNum(int lump);
void    W_ReleaseLumpName(char *name);

#endif
