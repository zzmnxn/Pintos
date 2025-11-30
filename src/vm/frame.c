#include "vm/frame.h"
#include <debug.h>
#include <stdio.h>
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

/* Removes a frame entry from the frame table without freeing the physical page.
   This is used during process exit to avoid double-free: the frame table entry
   is removed, but the physical memory will be freed later by pagedir_destroy().
   
   IMPORTANT: This function MUST NEVER call palloc_free_page() - it only removes
   the frame_entry from the table and frees the frame_entry struct itself. */
void
remove_frame_from_table (void *kpage)
{
  struct list_elem *e;
  struct frame_entry *fe = NULL;

  printf ("[DEBUG] remove_frame_from_table: kpage=%p\n", kpage);

  if (kpage == NULL)
    {
      printf ("[DEBUG] remove_frame_from_table: kpage is NULL, returning\n");
      return;
    }

  lock_acquire (&frame_lock);

  /* Find the frame_entry in the frame table. */
  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
    {
      fe = list_entry (e, struct frame_entry, list_elem);
      if (fe->frame == kpage)
        {
          /* Remove from frame table. */
          printf ("[DEBUG] remove_frame_from_table: Found frame_entry, removing from table\n");
          list_remove (e);
          break;
        }
    }

  lock_release (&frame_lock);

  if (fe == NULL)
    {
      printf ("[DEBUG] remove_frame_from_table: Frame not found in table - PANIC\n");
      PANIC ("Attempted to remove non-existent frame");
    }

  /* Free the frame_entry structure only. Physical page is NOT freed here.
     This is critical - the physical memory will be freed by pagedir_destroy(). */
  printf ("[DEBUG] remove_frame_from_table: Freeing frame_entry struct only (NOT physical page)\n");
  free (fe);
  printf ("[DEBUG] remove_frame_from_table: Done\n");
}

/* Frees a frame. */
void
free_frame (void *kpage)
{
  struct list_elem *e;
  struct frame_entry *fe = NULL;

  printf ("[DEBUG] free_frame: kpage=%p\n", kpage);

  if (kpage == NULL)
    {
      printf ("[DEBUG] free_frame: kpage is NULL, returning\n");
      return;
    }

  lock_acquire (&frame_lock);

  /* Find the frame_entry in the frame table. */
  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
    {
      fe = list_entry (e, struct frame_entry, list_elem);
      if (fe->frame == kpage)
        {
          /* Remove from frame table. */
          printf ("[DEBUG] free_frame: Found frame_entry, removing from table\n");
          list_remove (e);
          break;
        }
    }

  lock_release (&frame_lock);

  if (fe == NULL)
    {
      printf ("[DEBUG] free_frame: Frame not found in table - PANIC\n");
      PANIC ("Attempted to free non-existent frame");
    }

  /* Free the frame_entry structure. */
  printf ("[DEBUG] free_frame: Freeing frame_entry struct\n");
  free (fe);

  /* Free the physical page. */
  printf ("[DEBUG] free_frame: Freeing physical page kpage=%p\n", kpage);
  palloc_free_page (kpage);
  printf ("[DEBUG] free_frame: Done\n");
}

