#include "vm/frame.h"
#include <debug.h>
#include "lib/kernel/list.h"

/* Initializes the frame table. */
void
frame_init (void)
{
  /* TODO: Implement. */
}

/* Allocates a frame for the given vm_entry. */
void *
frame_alloc (struct vm_entry *vme)
{
  /* TODO: Implement. */
  return NULL;
}

/* Frees a frame. */
void
frame_free (void *frame)
{
  /* TODO: Implement. */
}

/* Finds a frame_entry for the given frame. */
struct frame_entry *
frame_find (void *frame)
{
  /* TODO: Implement. */
  return NULL;
}

