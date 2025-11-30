#include "vm/page.h"
#include <debug.h>
#include <stdio.h> 
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/file.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "threads/synch.h"
#include <string.h>
#include "lib/kernel/list.h"

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

/* Destructor function for hash_clear: frees vm_entry structure and associated resources.
   The aux parameter receives the pagedir (via h->aux) so we can avoid calling
   thread_current() which has assertion checks that may fail during process exit. */
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
          /* Remove frame from frame table without freeing physical memory.
             This prevents double-free: the physical memory will be freed later
             by pagedir_destroy(). We also avoid calling pagedir_clear_page()
             to prevent TLB invalidation (which calls pagedir_activate) during
             process exit, which could cause context switch and thread state issues. */
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
  ASSERT (pg_ofs (kpage) == 0);

  switch (vme->type)
    {
    case VM_BIN:
    case VM_FILE:
      {
        /* Load data from file. */
        if (vme->file == NULL)
          return false;

        lock_acquire (&filesys_lock);
        
        /* Read bytes from file. */
        off_t bytes_read = file_read_at (vme->file, kpage, vme->read_bytes, vme->offset);
        
        lock_release (&filesys_lock);

        if (bytes_read != (off_t) vme->read_bytes)
          return false;

        /* Zero the remaining bytes. */
        memset (kpage + vme->read_bytes, 0, vme->zero_bytes);
        break;
      }

    case VM_ANON:
      {
        /* Zero the entire page. */
        memset (kpage, 0, PGSIZE);
        break;
      }

    default:
      return false;
    }

  return true;
}

