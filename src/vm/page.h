#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"

/* Forward declarations. */
struct file;
struct thread;

/* Virtual memory page type. */
enum vm_type
  {
    VM_BIN,   /* Binary file (executable). */
    VM_FILE,  /* Mapped file. */
    VM_ANON   /* Anonymous page (swap). */
  };

/* Supplemental page table entry. */
struct vm_entry
  {
    enum vm_type type;           /* Type of page (VM_BIN, VM_FILE, VM_ANON). */
    void *vaddr;                  /* Virtual address. */
    bool writable;                /* Whether page is writable. */
    bool is_loaded;               /* Whether page is currently loaded in memory. */
    
    /* File-related fields (for VM_BIN and VM_FILE). */
    struct file *file;            /* Pointer to file. */
    off_t offset;                 /* File offset. */
    uint32_t read_bytes;          /* Bytes to read from file. */
    uint32_t zero_bytes;          /* Zero bytes to pad. */
    
    /* Swap-related fields (for VM_ANON). */
    size_t swap_slot;             /* Swap slot index. */
    
    /* Hash table element. */
    struct hash_elem hash_elem;   /* For hash table storage. */
  };

void vm_init (struct hash *vm);
void vm_destroy (struct hash *vm);
struct vm_entry *vm_find (struct hash *vm, void *vaddr);
bool vm_insert (struct hash *vm, struct vm_entry *vme);
bool vm_delete (struct hash *vm, struct vm_entry *vme);
bool vm_load_page (struct vm_entry *vme, void *kpage);

#endif /* vm/page.h */

