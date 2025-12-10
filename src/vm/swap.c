#include "vm/swap.h"
#include <debug.h>
#include <stddef.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"

/* Number of sectors per page. */
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* Swap disk management. */
static struct bitmap *swap_map;      /* Tracks used swap slots. */
static struct block *swap_block;     /* Swap disk block device. */
static struct lock swap_lock;       /* Synchronization for swap operations. */

/* Initializes the swap disk. */
void
swap_init (void)
{
  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block == NULL)
    PANIC ("No swap block device found");
  
  /* Calculate number of swap slots: total sectors / sectors per page. */
  size_t swap_slots = block_size (swap_block) / SECTORS_PER_PAGE;
  
  /* Initialize bitmap to track swap slots. */
  swap_map = bitmap_create (swap_slots);
  if (swap_map == NULL)
    PANIC ("Failed to create swap bitmap");
  
  /* Initialize lock for synchronization. */
  lock_init (&swap_lock);
}

/* Swaps out a frame to the swap disk. Returns swap slot index. */
size_t
swap_out (void *frame)
{
  size_t slot;
  block_sector_t sector;
  int i;
  
  lock_acquire (&swap_lock);
  
  /* Find a free swap slot. */
  slot = bitmap_scan_and_flip (swap_map, 0, 1, false);
  if (slot == BITMAP_ERROR)
    {
      lock_release (&swap_lock);
      PANIC ("No free swap slots available");
    }
  
  /* Calculate starting sector for this swap slot. */
  sector = slot * SECTORS_PER_PAGE;
  
  /* Write page to disk: 1 page = 8 sectors. */
  for (i = 0; i < SECTORS_PER_PAGE; i++)
    {
      block_write (swap_block, sector + i, 
                   (uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
    }
  
  lock_release (&swap_lock);
  
  return slot;
}

/* Swaps in a page from the swap disk to the given frame. */
void
swap_in (size_t swap_slot, void *frame)
{
  block_sector_t sector;
  int i;
  
  ASSERT (swap_slot != SWAP_SLOT_NONE);

  lock_acquire (&swap_lock);
  
  /* Calculate starting sector for this swap slot. */
  sector = swap_slot * SECTORS_PER_PAGE;
  
  /* Read page from disk: 1 page = 8 sectors. */
  for (i = 0; i < SECTORS_PER_PAGE; i++)
    {
      block_read (swap_block, sector + i,
                  (uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
    }
  
  /* Free the swap slot. */
  bitmap_flip (swap_map, swap_slot);
  
  lock_release (&swap_lock);
}

/* Frees a swap slot. */
void
swap_free (size_t swap_slot)
{
  if (swap_slot == SWAP_SLOT_NONE)
    return;

  lock_acquire (&swap_lock);
  
  /* Free the swap slot in bitmap. */
  bitmap_flip (swap_map, swap_slot);
  
  lock_release (&swap_lock);
}

