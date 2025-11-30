#include "vm/swap.h"
#include <debug.h>
#include <stddef.h>

/* Initializes the swap disk. */
void
swap_init (void)
{
  /* TODO: Implement. */
}

/* Swaps out a frame to the swap disk. Returns swap slot index. */
size_t
swap_out (void *frame)
{
  /* TODO: Implement. */
  return 0;
}

/* Swaps in a page from the swap disk to the given frame. */
void
swap_in (size_t swap_slot, void *frame)
{
  /* TODO: Implement. */
}

/* Frees a swap slot. */
void
swap_free (size_t swap_slot)
{
  /* TODO: Implement. */
}

