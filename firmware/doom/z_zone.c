//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
//	Zone Memory Allocation. Neat.
//


#include "z_zone.h"
#include "i_system.h"
#include "doomtype.h"


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
// 
 
#define MEM_ALIGN sizeof(void *)
#define ZONEID	0x1d4a11

typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;
    int			tag;	// PU_FREE if this is free
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
} memblock_t;


typedef struct
{
    // total bytes malloced, including header
    int		size;

    // start / end cap for linked list
    memblock_t	blocklist;
    
    memblock_t*	rover;
    
} memzone_t;



memzone_t*	mainzone;



//
// Z_ClearZone
//
void Z_ClearZone (memzone_t* zone)
{
    memblock_t*		block;
	
    // set the entire zone to one free block
    zone->blocklist.next =
	zone->blocklist.prev =
	block = (memblock_t *)( (byte *)zone + sizeof(memzone_t) );
    
    zone->blocklist.user = (void *)zone;
    zone->blocklist.tag = PU_STATIC;
    zone->rover = block;
	
    block->prev = block->next = &zone->blocklist;
    
    // a free block.
    block->tag = PU_FREE;

    block->size = zone->size - sizeof(memzone_t);
}



//
// Z_Init
//
void Z_Init (void)
{
    memblock_t*	block;
    int		size;

    mainzone = (memzone_t *)I_ZoneBase (&size);
    mainzone->size = size;

    // set the entire zone to one free block
    mainzone->blocklist.next =
	mainzone->blocklist.prev =
	block = (memblock_t *)( (byte *)mainzone + sizeof(memzone_t) );

    mainzone->blocklist.user = (void *)mainzone;
    mainzone->blocklist.tag = PU_STATIC;
    mainzone->rover = block;
	
    block->prev = block->next = &mainzone->blocklist;

    // free block
    block->tag = PU_FREE;
    
    block->size = mainzone->size - sizeof(memzone_t);
}


#ifdef Z_STATS
// The most memory ever held by blocks that cannot be purged (host tests)
static int z_peak;

static void Z_UpdatePeak(void)
{
    memblock_t *block;
    int used = 0;

    for (block = mainzone->blocklist.next; block != &mainzone->blocklist; block = block->next)
        if (block->tag != PU_FREE && block->tag < PU_PURGELEVEL)
            used += block->size;
    if (used > z_peak)
        z_peak = used;
}

int Z_PeakUsed(void)
{
    return z_peak;
}

// Where each live block came from (host tests)
#define SITES 16384
static struct { void *p; const char *file; int line; } sites[SITES];

void *Z_MallocAt(int size, int tag, void *user, const char *file, int line)
{
    void *p = (Z_Malloc)(size, tag, user);     // (the function, not the macro)
    int i;

    for (i = 0; i < SITES; i++)
        if (sites[i].p == NULL || sites[i].p == p)
        {
            sites[i].p = p;
            sites[i].file = file;
            sites[i].line = line;
            break;
        }
    return p;
}

void Z_DumpSites(int min_size)
{
    memblock_t *block;
    static const char *const tags[] = {"?", "static", "sound", "music", "free", "level", "levspec",
                                       "purgelevel", "cache"};

    for (block = mainzone->blocklist.next; block != &mainzone->blocklist; block = block->next)
    {
        void *p = (byte *) block + sizeof(memblock_t);
        const char *file = "?";
        int line = 0, i;

        if ((block->tag == PU_FREE && min_size) || block->size < min_size)
            continue;
        for (i = 0; i < SITES; i++)
            if (sites[i].p == p)
            {
                file = sites[i].file;
                line = sites[i].line;
                break;
            }
        fprintf(stderr, "  %7d %-8s %s:%d\n", block->size, tags[block->tag < 9 ? block->tag : 0], file, line);
    }
}

// Bytes held per tag (host tests: what the zone holds at a moment)
void Z_TagUsage(int *bytes, int ntags)
{
    memblock_t *block;
    int i;

    for (i = 0; i < ntags; i++)
        bytes[i] = 0;
    for (block = mainzone->blocklist.next; block != &mainzone->blocklist; block = block->next)
        if (block->tag >= 0 && block->tag < ntags)
            bytes[block->tag] += block->size;
}
#endif

//
// Small level objects: thousands are spawned and freed in a level (puffs,
// blood, shots) and, among the level's big blocks, they cut the free space
// into pieces too small for a texture. They get slots instead, in chunks of
// the zone; all of them go with the level (Z_FreeTags).
//
#define CHUNK_SLOTS 32
#define MAX_CHUNKS 32

static byte *slot_chunks[MAX_CHUNKS];
static int num_chunks;
static void *slot_free;                 // free slots, linked through their first word

static boolean Z_SlotOwned(void *p)
{
    int i;

    for (i = 0; i < num_chunks; i++)
        if ((byte *) p >= slot_chunks[i] && (byte *) p < slot_chunks[i] + CHUNK_SLOTS * Z_SLOT_SIZE)
            return true;
    return false;
}

static void *Z_SlotAlloc(void);

// Chunks for n more objects now, while the level's blocks are going in one
// after another (P_SetupLevel): a chunk made later, during play, would land
// in the middle of the free space
void Z_SlotReserve(int n)
{
    void *p, *list = NULL;
    int i;

    for (i = 0; i < n; i++)
    {
        p = Z_SlotAlloc();
        if (p == NULL)
            break;
        *(void **) p = list;
        list = p;
    }
    while (list != NULL)
    {
        p = list;
        list = *(void **) p;
        *(void **) p = slot_free;
        slot_free = p;
    }
}

static void *Z_SlotAlloc(void)
{
    void *p;


    if (slot_free == NULL)
    {
        byte *chunk;
        int i;

        if (num_chunks == MAX_CHUNKS)
            return NULL;
        chunk = (Z_Malloc)(CHUNK_SLOTS * Z_SLOT_SIZE, PU_LEVEL, &slot_chunks[num_chunks]);
        num_chunks++;
        for (i = CHUNK_SLOTS - 1; i >= 0; i--)
        {
            *(void **) (chunk + i * Z_SLOT_SIZE) = slot_free;
            slot_free = chunk + i * Z_SLOT_SIZE;
        }
    }
    p = slot_free;
    slot_free = *(void **) p;
    return p;
}

//
// Z_Free
//
static void Z_FreeBlock (void* ptr);

void Z_Free (void* ptr)
{
    if (Z_SlotOwned(ptr))
    {
        *(void **) ptr = slot_free;
        slot_free = ptr;
        return;
    }
    Z_FreeBlock(ptr);
}

// a zone block (the slots' chunks too: Z_FreeTags, the purge in Z_Malloc)
static void Z_FreeBlock (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;
	
    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
	I_Error ("Z_Free: freed a pointer without ZONEID");
		
    if (block->tag != PU_FREE && block->user != NULL)
    {
    	// clear the user's mark
	    *block->user = 0;
    }

    // mark as free
    block->tag = PU_FREE;
    block->user = NULL;
    block->id = 0;
	
    other = block->prev;

    if (other->tag == PU_FREE)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == mainzone->rover)
            mainzone->rover = other;

        block = other;
    }
	
    other = block->next;
    if (other->tag == PU_FREE)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == mainzone->rover)
            mainzone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64


#ifdef Z_STATS
#undef Z_Malloc
#endif

void*
Z_Malloc
( int		size,
  int		tag,
  void*		user )
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;
    void *result;
    boolean retried = false;

    if (size <= (int) Z_SLOT_SIZE && user == NULL && (tag == PU_LEVEL || tag == PU_LEVSPEC))
    {
        result = Z_SlotAlloc();
        if (result != NULL)
            return result;
    }

    size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    
    // scan through the block list,
    // looking for the first free block
    // of sufficient size,
    // throwing out any purgable blocks along the way.

    // account for size of block header
    size += sizeof(memblock_t);
    
    // if there is a free block behind the rover,
    //  back up over them
    base = mainzone->rover;
    
    if (base->prev->tag == PU_FREE)
        base = base->prev;
	
    rover = base;
    start = base->prev;
	
    do
    {
        if (rover == start)
        {
            // scanned all the way around the list. The scan stops at the
            // block before the one it began at without purging it, so a
            // fit can be missed: once more after purging all the cache,
            // from the start of the zone.
            if (!retried)
            {
                retried = true;
                Z_FreeTags(PU_PURGELEVEL, PU_CACHE);
                base = mainzone->blocklist.next;
                rover = base;
                start = base->prev;
                continue;
            }
            I_Error ("Z_Malloc: failed on allocation of %i bytes", size);
        }
	
        if (rover->tag != PU_FREE)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                // hit a block that can't be purged,
                // so move base past it
                base = rover = rover->next;
            }
            else
            {
                // free the rover block (adding the size to base)

                // the rover can be the base block
                base = base->prev;
                Z_FreeBlock ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
        {
            rover = rover->next;
        }

    } while (base->tag != PU_FREE || base->size < size);

    
    // found a block big enough
    extra = base->size - size;
    
    if (extra >  MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;
	
        newblock->tag = PU_FREE;
        newblock->user = NULL;	
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }
	
	if (user == NULL && tag >= PU_PURGELEVEL)
	    I_Error ("Z_Malloc: an owner is required for purgable blocks");

    base->user = user;
    base->tag = tag;

    result  = (void *) ((byte *)base + sizeof(memblock_t));

    if (base->user)
    {
        *base->user = result;
    }

    // next allocation will start looking here
    mainzone->rover = base->next;	
	
    base->id = ZONEID;

#ifdef Z_STATS
    Z_UpdatePeak();
#endif
    return result;
}



//
// Z_FreeTags
//
void
Z_FreeTags
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
    memblock_t*	next;
	
    for (block = mainzone->blocklist.next ;
	 block != &mainzone->blocklist ;
	 block = next)
    {
	// get link before freeing
	next = block->next;

	// free block?
	if (block->tag == PU_FREE)
	    continue;
	
	if (block->tag >= lowtag && block->tag <= hightag)
	    Z_FreeBlock ( (byte *)block+sizeof(memblock_t));
    }

    // the slots' chunks are level blocks: gone with the level
    if (lowtag <= PU_LEVEL && hightag >= PU_LEVSPEC)
    {
        num_chunks = 0;
        slot_free = NULL;
    }
}



//
// Z_DumpHeap
// Note: TFileDumpHeap( stdout ) ?
//
void
Z_DumpHeap
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
	
    printf ("zone size: %i  location: %p\n",
	    mainzone->size,mainzone);
    
    printf ("tag range: %i to %i\n",
	    lowtag, hightag);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->tag >= lowtag && block->tag <= hightag)
	    printf ("block:%p    size:%7i    user:%p    tag:%3i\n",
		    block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    printf ("ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    printf ("ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    printf ("ERROR: two consecutive free blocks\n");
    }
}


//
// Z_FileDumpHeap
//
void Z_FileDumpHeap (FILE* f)
{
    memblock_t*	block;
	
    fprintf (f,"zone size: %i  location: %p\n",mainzone->size,mainzone);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	fprintf (f,"block:%p    size:%7i    user:%p    tag:%3i\n",
		 block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    fprintf (f,"ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    fprintf (f,"ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    fprintf (f,"ERROR: two consecutive free blocks\n");
    }
}



//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
    memblock_t*	block;
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}




//
// Z_ChangeTag
//
void Z_ChangeTag2(void *ptr, int tag, char *file, int line)
{
    memblock_t*	block;
	
    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
        I_Error("%s:%i: Z_ChangeTag: block without a ZONEID!",
                file, line);

    if (tag >= PU_PURGELEVEL && block->user == NULL)
        I_Error("%s:%i: Z_ChangeTag: an owner is required "
                "for purgable blocks", file, line);

    block->tag = tag;
}

void Z_ChangeUser(void *ptr, void **user)
{
    memblock_t*	block;

    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
        I_Error("Z_ChangeUser: Tried to change user for invalid block!");
    }

    block->user = user;
    *user = ptr;
}



//
// Z_FreeMemory
//
int Z_FreeMemory (void)
{
    memblock_t*		block;
    int			free;
	
    free = 0;
    
    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist;
         block = block->next)
    {
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
            free += block->size;
    }

    return free;
}

unsigned int Z_ZoneSize(void)
{
    return mainzone->size;
}

