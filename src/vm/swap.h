#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Function declarations. */
void swap_init (void);
size_t swap_out (void *frame);
void swap_in (size_t swap_slot, void *frame);
void swap_free (size_t swap_slot);

#endif 

