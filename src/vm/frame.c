#include "vm/frame.h"
#include <debug.h>
#include "lib/kernel/list.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* Global frame table. */
static struct list frame_table;

/* Lock for frame table synchronization. */
static struct lock frame_lock;

/* Initializes the frame table. */
void
frame_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_lock);
}

/* Allocates a frame and returns its kernel virtual address.
   Returns NULL on failure. */
void *
allocate_frame (enum palloc_flags flags)
{
  void *kpage;
  struct frame_entry *fe;

  /* Allocate a physical page. */
  kpage = palloc_get_page (flags);
  if (kpage == NULL)
    PANIC ("Frame allocation failed");

  /* Allocate frame_entry structure. */
  fe = malloc (sizeof (struct frame_entry));
  if (fe == NULL)
    {
      /* If malloc fails, free the allocated page and panic. */
      palloc_free_page (kpage);
      PANIC ("Frame entry allocation failed");
    }

  /* Initialize frame_entry. */
  fe->frame = kpage;
  fe->owner = thread_current ();
  fe->vme = NULL;

  /* Add to frame table. */
  lock_acquire (&frame_lock);
  list_push_back (&frame_table, &fe->list_elem);
  lock_release (&frame_lock);

  return kpage;
}

/* Frees a frame. */
void
free_frame (void *kpage)
{
  struct list_elem *e;
  struct frame_entry *fe = NULL;

  if (kpage == NULL)
    return;

  lock_acquire (&frame_lock);

  /* Find the frame_entry in the frame table. */
  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
    {
      fe = list_entry (e, struct frame_entry, list_elem);
      if (fe->frame == kpage)
        {
          /* Remove from frame table. */
          list_remove (e);
          break;
        }
    }

  lock_release (&frame_lock);

  if (fe == NULL)
    PANIC ("Attempted to free non-existent frame");

  /* Free the frame_entry structure. */
  free (fe);

  /* Free the physical page. */
  palloc_free_page (kpage);
}

