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
#include <string.h>
#include "filesys/file.h"
#include "userprog/syscall.h"

/* Global frame table. */
static struct list frame_table;

/* Lock for frame table synchronization. */
static struct lock frame_lock;

/* Clock hand for Clock Algorithm. */
static struct list_elem *clock_hand = NULL;

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
  struct frame_entry *fe;
  struct thread *owner;
  uint32_t *pd;
  struct vm_entry *vme;
  void *vaddr;
  bool dirty;
  size_t swap_slot = SWAP_SLOT_NONE;
  struct list_elem *start_hand;
  bool looped_once = false;
  struct frame_entry *victim_fe = NULL;
  struct vm_entry *victim_vme = NULL;
  enum vm_type vme_type;
  void *kpage;

  lock_acquire (&frame_lock);

  /* Check if frame_table is empty. */
  if (list_empty (&frame_table))
    {
      lock_release (&frame_lock);
      return NULL;
    }

  /* Initialize clock_hand if needed. */
  if (clock_hand == NULL || clock_hand == list_end (&frame_table))
    clock_hand = list_begin (&frame_table);

  /* Remember starting point to detect infinite loop. */
  start_hand = clock_hand;

  /* Clock Algorithm: find a victim frame. */
  while (true)
    {
      /* If we've gone through all frames, restart. */
      if (clock_hand == list_end (&frame_table))
        {
          clock_hand = list_begin (&frame_table);
          looped_once = true;
        }

      /* If we've looped once and returned to start, check for infinite loop. */
      if (looped_once && clock_hand == start_hand)
        {
          /* Check if there are any valid frames at all. */
          struct list_elem *e;
          bool found_valid_frame = false;
          
          for (e = list_begin (&frame_table); e != list_end (&frame_table);
               e = list_next (e))
            {
              fe = list_entry (e, struct frame_entry, list_elem);
              if (fe->owner != NULL && fe->vme != NULL)
                {
                  found_valid_frame = true;
                  break;
                }
            }
          
          if (!found_valid_frame)
            {
              lock_release (&frame_lock);
              return NULL;
            }
          
          /* All valid frames have accessed bit set. Continue to give second chance. */
          looped_once = false;
          start_hand = clock_hand;
        }

      fe = list_entry (clock_hand, struct frame_entry, list_elem);
      
      /* Check if owner is valid. */
      if (fe->owner == NULL)
        {
          clock_hand = list_next (clock_hand);
          continue;
        }
      
      owner = fe->owner;
      pd = owner->pagedir;
      if (pd == NULL)
        {
          clock_hand = list_next (clock_hand);
          continue;
        }

      /* If vme is not set, try to find it from owner's SPT. */
      if (fe->vme == NULL)
        {
          struct hash_iterator i;
          struct vm_entry *found_vme = NULL;
          
          /* Search through owner's SPT to find the vm_entry that maps to this kpage. */
          hash_first (&i, &owner->vm);
          while (hash_next (&i))
            {
              struct vm_entry *candidate_vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
              
              if (candidate_vme->is_loaded)
                {
                  void *mapped_kpage = pagedir_get_page (pd, candidate_vme->vaddr);
                  if (mapped_kpage == fe->frame)
                    {
                      found_vme = candidate_vme;
                      break;
                    }
                }
            }
          
          if (found_vme != NULL)
            {
              /* Found it! Update frame_entry for future use. */
              fe->vme = found_vme;
              vme = found_vme;
            }
          else
            {
              /* Could not find vme - skip this frame. */
              clock_hand = list_next (clock_hand);
              continue;
            }
        }
      else
        {
          vme = fe->vme;
        }
      
      vaddr = vme->vaddr;

      /* Check accessed bit. */
      if (pagedir_is_accessed (pd, vaddr))
        {
          /* Give second chance: clear accessed bit and continue. */
          pagedir_set_accessed (pd, vaddr, false);
          clock_hand = list_next (clock_hand);
          continue;
        }

      /* Found victim: accessed bit is clear. */
      break;
    }

  /* We have a victim frame. Save information needed for eviction. */
  kpage = fe->frame;
  victim_fe = fe;
  victim_vme = vme;
  vme_type = vme->type;

  /* Advance clock_hand before removing the victim. */
  struct list_elem *victim_elem = clock_hand;
  clock_hand = list_next (clock_hand);
  if (clock_hand == list_end (&frame_table))
    clock_hand = list_begin (&frame_table);

  /* Pin the vm_entry to prevent concurrent access during eviction. */
  victim_vme->pinned = true;

  /* Release lock before any I/O operations to avoid blocking other threads. */
  lock_release (&frame_lock);

  /* 1. Dirty Check & Write-back */
  dirty = pagedir_is_dirty (pd, vaddr);

  if (vme_type == VM_FILE)
    {
      /* Mmap 파일인 경우: Dirty하면 파일에 쓰고, 스왑은 안 함 */
      if (dirty && victim_vme->file != NULL)
        {
          bool lock_held = filesys_lock_held_by_current_thread ();
          if (!lock_held)
            lock_acquire (&filesys_lock);
          
          file_write_at (victim_vme->file, kpage, victim_vme->read_bytes, victim_vme->offset);
          
          if (!lock_held)
            lock_release (&filesys_lock);
        }
      victim_vme->swap_slot = SWAP_SLOT_NONE;
    }
  else if (vme_type == VM_ANON)
    {
      /* Anon 페이지(스택 등)인 경우: 무조건 스왑 아웃 */
      swap_slot = swap_out (kpage);
      victim_vme->swap_slot = swap_slot;
    }
  else if (vme_type == VM_BIN)
    {
      /* 실행 파일 코드/데이터: Dirty일 수 없음(수정 불가). 그냥 버림.
         만약 dirty하다면(코드 수정 등) VM_ANON으로 변환하여 스왑해야 하지만, 
         Pintos 기본 과제에선 VM_BIN은 Read-only로 가정해도 됨 */
      victim_vme->swap_slot = SWAP_SLOT_NONE;
    }

  /* 2. is_loaded false 설정 및 메모리 해제 */
  victim_vme->is_loaded = false;
  pagedir_clear_page (pd, vaddr);

  /* Remove from frame table. */
  lock_acquire (&frame_lock);
  list_remove (victim_elem);
  lock_release (&frame_lock);

  /* Free the frame_entry structure. */
  free (victim_fe);

  /* Clear page contents before reuse. */
  memset (kpage, 0, PGSIZE);

  /* Unpin the vm_entry after swap operations are complete. */
  victim_vme->pinned = false;

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
        return NULL;
      
      if (flags & PAL_ZERO)
        memset (kpage, 0, PGSIZE);
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
          
          /* If removed element is clock_hand, advance it to maintain validity. */
          if (e == clock_hand)
            {
              clock_hand = list_next (e);
              if (clock_hand == list_end (&frame_table))
                clock_hand = list_begin (&frame_table);
            }
          
          break;
        }
      fe = NULL;  /* Reset if not found in this iteration */
    }

  lock_release (&frame_lock);

  /* If frame not found, it may have been evicted already - this is normal. */
  if (fe == NULL)
    return;

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
          
          /* If removed element is clock_hand, advance it to maintain validity. */
          if (e == clock_hand)
            {
              clock_hand = list_next (e);
              if (clock_hand == list_end (&frame_table))
                clock_hand = list_begin (&frame_table);
            }
          
          break;
        }
      fe = NULL;  /* Reset if not found in this iteration */
    }

  lock_release (&frame_lock);

  if (fe == NULL)
    {
      return;
    }

  /* Free the frame_entry structure. */
  free (fe);

  /* Free the physical page. */
  palloc_free_page (kpage);
}

