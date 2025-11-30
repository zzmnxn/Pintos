#include "vm/frame.h"
#include <debug.h>
#include <stdio.h>
#include "lib/kernel/list.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"

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

/* Helper function to find frame_entry for a given kpage. */
static struct frame_entry *
get_frame_entry (void *kpage)
{
  struct list_elem *e;
  struct frame_entry *fe = NULL;

  lock_acquire (&frame_lock);

  /* Find the frame_entry in the frame table. */
  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
    {
      fe = list_entry (e, struct frame_entry, list_elem);
      if (fe->frame == kpage)
        break;
      fe = NULL;
    }

  lock_release (&frame_lock);
  return fe;
}

/* Helper function to find vm_entry for a given frame.
   If frame_entry->vme is set, return it. Otherwise, search through
   the owner thread's SPT to find the vm_entry that maps to this kpage. */
static struct vm_entry *
find_vm_entry_for_frame (void *kpage)
{
  struct frame_entry *fe;
  struct thread *owner;
  struct hash_iterator i;
  struct vm_entry *vme;

  /* Get frame_entry. */
  fe = get_frame_entry (kpage);
  if (fe == NULL)
    return NULL;

  /* If vme is already set, return it. */
  if (fe->vme != NULL)
    return fe->vme;

  /* Get owner thread. */
  owner = fe->owner;
  if (owner == NULL || owner->pagedir == NULL)
    return NULL;

  /* Search through owner's SPT. */
  hash_first (&i, &owner->vm);
  while (hash_next (&i))
    {
      vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
      
      if (vme->is_loaded)
        {
          void *mapped_kpage = pagedir_get_page (owner->pagedir, vme->vaddr);
          if (mapped_kpage == kpage)
            {
              /* Found it! Update frame_entry for future use. */
              fe->vme = vme;
              return vme;
            }
        }
    }

  return NULL;
}

/* Evicts a frame using Clock Algorithm and returns the freed kpage. */
static void *
evict_frame (void)
{
  static struct list_elem *clock_hand = NULL;
  struct frame_entry *fe;
  struct thread *owner;
  uint32_t *pd;
  struct vm_entry *vme;
  void *vaddr;
  bool dirty;
  size_t swap_slot;

  lock_acquire (&frame_lock);

  /* Initialize clock_hand if needed. */
  if (clock_hand == NULL || clock_hand == list_end (&frame_table))
    clock_hand = list_begin (&frame_table);

  /* Clock Algorithm: find a victim frame. */
  while (true)
    {
      /* If we've gone through all frames, restart. */
      if (clock_hand == list_end (&frame_table))
        clock_hand = list_begin (&frame_table);

      fe = list_entry (clock_hand, struct frame_entry, list_elem);
      owner = fe->owner;
      pd = owner->pagedir;

      /* Find vm_entry for this frame. */
      lock_release (&frame_lock);
      vme = find_vm_entry_for_frame (fe->frame);
      lock_acquire (&frame_lock);

      if (vme == NULL)
        {
          /* No vm_entry found - skip this frame. */
          clock_hand = list_next (clock_hand);
          continue;
        }

      vaddr = vme->vaddr;

      /* Check accessed bit. */
      if (pagedir_is_accessed (pd, vaddr))
        {
          /* Give second chance: clear accessed bit and continue. */
          lock_release (&frame_lock);
          pagedir_set_accessed (pd, vaddr, false);
          lock_acquire (&frame_lock);
          clock_hand = list_next (clock_hand);
          continue;
        }

      /* Found victim: accessed bit is clear. */
      break;
    }

  /* We have a victim frame. Save necessary information before releasing lock. */
  void *kpage = fe->frame;
  struct list_elem *victim_elem = clock_hand;
  
  /* Advance clock_hand before removing the victim. */
  clock_hand = list_next (clock_hand);
  if (clock_hand == list_end (&frame_table))
    clock_hand = list_begin (&frame_table);

  /* Release lock before swap operations and pagedir operations. */
  lock_release (&frame_lock);

  /* Check dirty bit. */
  dirty = pagedir_is_dirty (pd, vaddr);

  /* Handle eviction based on page type. */
  if (vme->type == VM_BIN && !dirty)
    {
      /* VM_BIN and not dirty: just discard (can reload from file). */
      /* No swap needed. */
    }
  else if (vme->type == VM_FILE && dirty)
    {
      /* VM_FILE and dirty: structure for future write-back.
         For now, treat as VM_ANON and swap out. */
      swap_slot = swap_out (kpage);
      vme->swap_slot = swap_slot;
      vme->type = VM_ANON;
    }
  else if (vme->type == VM_ANON || (vme->type == VM_BIN && dirty))
    {
      /* VM_ANON or dirty VM_BIN: swap out. */
      swap_slot = swap_out (kpage);
      vme->swap_slot = swap_slot;
      if (vme->type == VM_BIN)
        vme->type = VM_ANON;
    }

  /* Mark page as not loaded. */
  vme->is_loaded = false;

  /* Clear page mapping. */
  pagedir_clear_page (pd, vaddr);

  /* Remove from frame table and free frame_entry. */
  lock_acquire (&frame_lock);
  list_remove (victim_elem);
  lock_release (&frame_lock);

  free (fe);

  /* Free the physical page. */
  palloc_free_page (kpage);

  return kpage;
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
    {
      /* Memory is full - evict a frame. */
      kpage = evict_frame ();
      if (kpage == NULL)
        PANIC ("Eviction failed");
    }

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
    PANIC ("Attempted to remove non-existent frame");

  /* Free the frame_entry structure only. Physical page is NOT freed here.
     This is critical - the physical memory will be freed by pagedir_destroy(). */
  free (fe);
}

/* Sets the vm_entry for a frame. */
void
set_frame_vme (void *kpage, struct vm_entry *vme)
{
  struct frame_entry *fe = get_frame_entry (kpage);
  if (fe != NULL)
    fe->vme = vme;
}

/* Frees a frame. */
void
free_frame (void *kpage)
{
  struct list_elem *e;
  struct frame_entry *fe = NULL;


  if (kpage == NULL)
    {
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
          list_remove (e);
          break;
        }
    }

  lock_release (&frame_lock);

  if (fe == NULL)
    {
      PANIC ("Attempted to free non-existent frame");
    }

  /* Free the frame_entry structure. */
  free (fe);

  /* Free the physical page. */
  palloc_free_page (kpage);
}

