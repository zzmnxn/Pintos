#include "vm/page.h"
#include <debug.h>
#include "lib/kernel/hash.h"

/* Initializes the supplemental page table. */
void
vm_init (struct hash *vm)
{
  /* TODO: Implement. */
}

/* Destroys the supplemental page table. */
void
vm_destroy (struct hash *vm)
{
  /* TODO: Implement. */
}

/* Finds a vm_entry for the given virtual address. */
struct vm_entry *
vm_find (struct hash *vm, void *vaddr)
{
  /* TODO: Implement. */
  return NULL;
}

/* Inserts a vm_entry into the supplemental page table. */
bool
vm_insert (struct hash *vm, struct vm_entry *vme)
{
  /* TODO: Implement. */
  return false;
}

/* Deletes a vm_entry from the supplemental page table. */
bool
vm_delete (struct hash *vm, struct vm_entry *vme)
{
  /* TODO: Implement. */
  return false;
}

