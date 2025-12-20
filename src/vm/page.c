#include "vm/page.h"
#include <debug.h>
#include <stdio.h> 
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/synch.h"
#include <string.h>
#include "lib/kernel/list.h"
#include "userprog/syscall.h"

/* Hash function for vm_entry: hashes by virtual address (page-aligned). */
static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  const struct vm_entry *vme = hash_entry (e, struct vm_entry, hash_elem);
  return hash_int ((int) pg_no (vme->vaddr));
}

/* Comparison function for vm_entry: compares by virtual address. */
static bool
vm_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  const struct vm_entry *vme_a = hash_entry (a, struct vm_entry, hash_elem);
  const struct vm_entry *vme_b = hash_entry (b, struct vm_entry, hash_elem);
  return vme_a->vaddr < vme_b->vaddr;
}


static void
vm_entry_destructor (struct hash_elem *e, void *aux)
{
  struct vm_entry *vme = hash_entry (e, struct vm_entry, hash_elem);
  uint32_t *pd = (uint32_t *) aux;  /* pagedir passed via h->aux from hash_clear */
  void *kpage;

  /* If the page is loaded and pagedir is provided, we need to remove the frame entry. */
  if (vme->is_loaded && pd != NULL)
    {
      /* Get the physical frame associated with this virtual address. */
      kpage = pagedir_get_page (pd, vme->vaddr);

      if (kpage != NULL)
        {
       
          remove_frame_from_table (kpage);
        }
    }
  
  /* Free the vm_entry structure itself. */
  free (vme);
}

/* Initializes the supplemental page table. */
void
vm_init (struct hash *vm)
{
  hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

/* Destroys the supplemental page table. */
void
vm_destroy (struct hash *vm, uint32_t *pagedir)
{
  void *saved_aux;

  if (vm == NULL)
    return;

  /* Save the original aux value. */
  saved_aux = vm->aux;

  /* Temporarily set aux to pagedir so destructor can access it via h->aux. */
  vm->aux = (void *) pagedir;

  /* Clear the hash table, passing each element to the destructor.
     hash_clear will pass h->aux (which is now pagedir) to the destructor. */
  hash_clear (vm, vm_entry_destructor);

  /* Restore the original aux value. */
  vm->aux = saved_aux;

  /* Free the buckets (hash_clear only clears them, doesn't free). */
  free (vm->buckets);
  vm->bucket_cnt = 0;
  vm->elem_cnt = 0;
}

/* Finds a vm_entry for the given virtual address. */
struct vm_entry *
vm_find (struct hash *vm, void *vaddr)
{
  struct vm_entry vme_lookup;
  struct hash_elem *e;

  /* Create a temporary vm_entry for lookup. */
  vme_lookup.vaddr = pg_round_down (vaddr);

  /* Find the hash element. */
  e = hash_find (vm, &vme_lookup.hash_elem);
  
  if (e == NULL)
    return NULL;

  return hash_entry (e, struct vm_entry, hash_elem);
}

/* Inserts a vm_entry into the supplemental page table. */
bool
vm_insert (struct hash *vm, struct vm_entry *vme)
{
  struct hash_elem *e;

  /* Insert the vm_entry into the hash table. */
  e = hash_insert (vm, &vme->hash_elem);

  /* If e is not NULL, an element with the same vaddr already exists. */
  if (e != NULL)
    return false;

  return true;
}

/* Deletes a vm_entry from the supplemental page table. */
bool
vm_delete (struct hash *vm, struct vm_entry *vme)
{
  struct hash_elem *e;

  /* Delete the vm_entry from the hash table. */
  e = hash_delete (vm, &vme->hash_elem);

  /* If e is NULL, the element was not found. */
  if (e == NULL)
    return false;

  return true;
}

/* Loads a page for the given vm_entry into the specified physical page.
   Returns true on success, false on failure. */
bool
vm_load_page (struct vm_entry *vme, void *kpage)
{
  ASSERT (vme != NULL);
  ASSERT (kpage != NULL);
  
  struct thread *cur = thread_current ();
  bool lock_held = (filesys_lock.holder == cur);

  switch (vme->type)
    {
    case VM_BIN:
    case VM_FILE:
      {
        /* Load data from file. */
        if (vme->file == NULL)
          return false;

        /* Avoid re-entering filesys_lock when we were faulting while holding it. */
        if (!lock_held)
          lock_acquire (&filesys_lock);
        
        /* Read bytes from file. */
        off_t bytes_read = 0;
        if (vme->read_bytes > 0)
          {
            bytes_read = file_read_at (vme->file, kpage, vme->read_bytes, vme->offset);
            if (bytes_read != (off_t) vme->read_bytes)
              {
                if (!lock_held)
                  lock_release (&filesys_lock);
                return false;
              }
          }
        
        if (!lock_held)
          lock_release (&filesys_lock);
        
        /* Zero bytes padding */
        memset (kpage + vme->read_bytes, 0, vme->zero_bytes);
        
        /* VM_FILE 타입 페이지: 로딩 완료 표시 및 dirty 비트 초기화 */
        if (vme->type == VM_FILE)
          {
            vme->is_loaded = true;
            if (cur->pagedir != NULL)
              pagedir_set_dirty (cur->pagedir, vme->vaddr, false);
          }
        break;
      }

    case VM_ANON:
      {
        /* Check if this page was swapped out. */
        if (vme->swap_slot != SWAP_SLOT_NONE)
          {
            /* Restore from swap disk. */
            swap_in (vme->swap_slot, kpage);
            vme->swap_slot = SWAP_SLOT_NONE;  /* Clear swap slot after loading. */
          }
        else
          {
            /* Zero the entire page (new anonymous page). */
            memset (kpage, 0, PGSIZE);
          }
        break;
      }

    default:
      return false;
    }

  return true;
}

