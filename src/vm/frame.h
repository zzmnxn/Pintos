#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include "lib/kernel/list.h"

/* Forward declarations. */
struct thread;
struct vm_entry;

/* Physical frame table entry. */
struct frame_entry
  {
    void *frame;                  /* Physical frame address (kernel virtual address). */
    struct thread *owner;         /* Thread that owns this frame. */
    struct vm_entry *vme;         /* Associated vm_entry. */
    struct list_elem list_elem;   /* For frame table list. */
  };

/* Function declarations. */
void frame_init (void);
void *frame_alloc (struct vm_entry *vme);
void frame_free (void *frame);
struct frame_entry *frame_find (void *frame);

#endif 

