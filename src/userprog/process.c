#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/page.h"
#include "vm/frame.h"

/* External filesystem lock from syscall.c */
extern struct lock filesys_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *program_name;
  tid_t tid;
  struct thread *cur;
  struct thread *child;
  cur = running_thread ();

  program_name = palloc_get_page (0);
  if (program_name == NULL)
    return TID_ERROR;
  
  /* Find first space to extract program name */
  const char *space = strchr (file_name, ' ');
  if (space != NULL)
    {
      size_t name_len = space - file_name;
      if (name_len >= PGSIZE)
        name_len = PGSIZE - 1;
      strlcpy (program_name, file_name, name_len + 1);
    }
  else
    {
      strlcpy (program_name, file_name, PGSIZE);
    }

  /* Make a copy of FILE_NAME for start_process.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    {
      palloc_free_page (program_name);
      return TID_ERROR;
    }
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Create a new thread to execute program. */
  tid = thread_create (program_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (program_name);
      return TID_ERROR;
    }
    
  /* Set up parent-child relationship.
     Note: We must do this before the child thread runs, so we disable interrupts
     to ensure atomicity. */
  child = get_thread_by_tid (tid);
  if (child == NULL)
    {
      /* Thread creation succeeded but we can't find the child - shouldn't happen */
      palloc_free_page (program_name);
      return TID_ERROR;
    }
  
  child->parent = cur;
  list_push_back (&cur->children, &child->child_elem);
    
  /* Wait for child to finish loading */
  sema_down (&child->load_sema);
    
  /* Check if child loaded successfully */
  if (!child->load_success)
    {
      /* Child failed to load, wait for it to exit and clean up */
      sema_down (&child->exit_sema);
      
      /* Remove from children list */
      list_remove (&child->child_elem);
      
      palloc_free_page (program_name);
      return TID_ERROR;
    }
    
  palloc_free_page (program_name);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  vm_init (&thread_current ()->vm);
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  struct thread *cur;

    
  cur = thread_current ();
  
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  
  success = load (file_name, &if_.eip, &if_.esp);

  /* Set load success status and signal parent */
  cur->load_success = success;
  sema_up (&cur->load_sema);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success) 
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur;
  struct list_elem *e;
  struct thread *child = NULL;
  int exit_status;

  /* Get current thread - but we might be in BLOCKED state if called after sema_down.
     Use running_thread() instead of thread_current() to avoid assertion failure. */
  cur = running_thread ();
  
  /* Search for the child in our children list.
     Note: We must NOT use get_thread_by_tid() because the child thread
     may have already exited and been removed from the all_list. */
  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, child_elem);
      if (t->tid == child_tid)
        {
          child = t;
          break;
        }
    }
  
  /* If child not found in children list, it's either:
     1. Not a child of this process, or
     2. Already been waited on (removed from list) */
  if (child == NULL)
    return -1;
    
  /* Remove child from children list to prevent duplicate waits */
  list_remove (e);
  
  /* Wait for child to exit. 
     If the child has already exited, it will have done sema_up() on its exit_sema,
     so this sema_down() will not block. If the child hasn't exited yet, this will
     block until the child calls sema_up() in process_exit(). */
  sema_down (&child->exit_sema);
    
  /* Read child's exit status */
  exit_status = child->exit_status;
  
  /* Now that we've read the exit status, we can free the child thread's memory.
     We clear the parent pointer first to indicate that we're done with this child. */
  child->parent = NULL;
  
  /* If the child is already THREAD_DYING, free its memory now.
     Otherwise, it will be freed when it finishes exiting. */
  if (child->status == THREAD_DYING)
    palloc_free_page (child);
  
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  enum intr_level old_level;

  /* Set exit status if not already set (e.g., killed by kernel) */
  if (!cur->has_exited)
    {
      cur->exit_status = -1;
      cur->has_exited = true;
      /* Print exit message if not already printed by syscall_exit */
      printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
    }
    
  /* Disable interrupts before signaling semaphore to prevent context switch
     during cleanup. The sema_up may call thread_yield if another thread
     has higher priority, which could change our thread status. */
  old_level = intr_disable ();
  
  /* Signal parent process that we're exiting by signaling our own exit_sema.
     The parent waits on child->exit_sema in process_wait(). */
  sema_up (&cur->exit_sema);
  
  /* Re-enable interrupts after sema_up */
  intr_set_level (old_level);
  /* Clean up any children that we haven't waited on.
     Set their parent pointers to NULL so they'll be freed when they exit. */
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_front (&cur->children);
      struct thread *child = list_entry (e, struct thread, child_elem);
      list_remove (e);
      
      /* Orphan the child - set parent to NULL */
      child->parent = NULL;
      
      /* If child has already exited (THREAD_DYING), free its memory */
      if (child->status == THREAD_DYING)
        palloc_free_page (child);
    }

  /* Close all open files in file descriptor table */
  for (int i = 2; i < FD_MAX; i++)
    {
      if (cur->fd_table[i] != NULL)
        {
          file_close (cur->fd_table[i]);
          cur->fd_table[i] = NULL;
        }
    }

  /* Clean up executable file */
  if (cur->executable_file != NULL)
    {
      file_allow_write (cur->executable_file);
      file_close (cur->executable_file);
      cur->executable_file = NULL;
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Unmap all mmap regions with write-back before destroying SPT */
      struct hash_iterator i;
      struct vm_entry *vme;
      void *kpage;
      bool dirty;
      struct file *closed_files[64];  /* Track up to 64 unique files */
      int closed_count = 0;
      int j;
      bool already_closed;
      
      /* First pass: write back dirty pages and clear mappings */
      hash_first (&i, &cur->vm);
      while (hash_next (&i))
        {
          vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
          
          if (vme->type == VM_FILE)
            {
              /* Check if page is loaded */
              if (vme->is_loaded)
                {
                  /* Get physical page */
                  kpage = pagedir_get_page (pd, vme->vaddr);
                  
                  if (kpage != NULL)
                    {
                      /* Check if page is dirty */
                      dirty = pagedir_is_dirty (pd, vme->vaddr);
                      
                      if (dirty && vme->file != NULL)
                        {
                          /* Write back to file */
                          lock_acquire (&filesys_lock);
                          file_write_at (vme->file, kpage, vme->read_bytes, vme->offset);
                          lock_release (&filesys_lock);
                          
                          /* Reset dirty bit after write-back */
                          pagedir_set_dirty (pd, vme->vaddr, false);
                        }
                      
                      /* Clear page mapping */
                      pagedir_clear_page (pd, vme->vaddr);
                      
                      /* Free frame (removes from table and frees physical memory) */
                      free_frame (kpage);
                    }
                }
            }
        }
      
      /* Second pass: close unique mmap files */
      hash_first (&i, &cur->vm);
      while (hash_next (&i))
        {
          vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
          
          if (vme->type == VM_FILE && vme->file != NULL)
            {
              /* Check if we've already closed this file */
              already_closed = false;
              for (j = 0; j < closed_count; j++)
                {
                  if (closed_files[j] == vme->file)
                    {
                      already_closed = true;
                      break;
                    }
                }
              
              if (!already_closed)
                {
                  /* Close the file */
                  lock_acquire (&filesys_lock);
                  file_close (vme->file);
                  lock_release (&filesys_lock);
                  
                  /* Track it */
                  if (closed_count < 64)
                    closed_files[closed_count++] = vme->file;
                }
            }
        }
      
      /* Destroy the supplemental page table.
         This must be done before destroying the page directory
         because vm_entry_destructor needs the pagedir to look up frames. */
      vm_destroy (&cur->vm, pd);
          
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch.
  */
void
process_activate (void)
{
  struct thread *t = running_thread ();

  /* Activate thread's page tables.
     NULL pagedir means kernel thread, skip activation. */
  if (t->pagedir != NULL)
    pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool setup_args (const char *cmdline, void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t;
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *cmdline_copy = NULL;
  char *program_name = NULL;
  char *save_ptr;

  t = thread_current ();

  /* TODO: parse file name
     Parse the command line to extract the program name.
     The file_name parameter contains the entire command line (e.g., "args-single onearg"),
     but we need to extract only the program name for filesys_open(). */
  
  /* Make a copy of file_name for parsing */
  cmdline_copy = palloc_get_page (0);
  if (cmdline_copy == NULL)
    {
      goto done;
    }
  strlcpy (cmdline_copy, file_name, PGSIZE);
  
  /* Extract the first token (program name) from the command line */
  program_name = strtok_r (cmdline_copy, " ", &save_ptr);
  if (program_name == NULL)
    {
      palloc_free_page (cmdline_copy);
      goto done;
    }

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    {
      palloc_free_page (cmdline_copy);
      goto done;
    }
  process_activate ();

  /* Open executable file using only the program name (not the entire command line). */
  file = filesys_open (program_name);
  if (file == NULL) 
    {
      palloc_free_page (cmdline_copy);
      goto done; 
    }
  
  /* Store executable file and deny writes to prevent modification during execution */
  t->executable_file = file;
  file_deny_write (t->executable_file);
  
  /* Free the copy - we no longer need it since file_name will be used for setup_args */
  palloc_free_page (cmdline_copy);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Parse arguments and set up the stack for argument passing. */
  if (!setup_args (file_name, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (!success)
    {
      /* If load failed, close the file and clear executable_file */
      if (t->executable_file != NULL)
        {
          file_allow_write (t->executable_file);
          file_close (t->executable_file);
          t->executable_file = NULL;
        }
    }
  /* If load succeeded, keep the file open for the duration of the process */
  return success;
}

/* load() helpers. */

bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   This function now creates vm_entry structs for lazy loading instead
   of immediately loading pages.

   Return true if successful, false if a memory allocation error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct thread *t = thread_current ();
  off_t current_offset = ofs;
  
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Allocate a vm_entry for this page. */
      struct vm_entry *vme = malloc (sizeof (struct vm_entry));
      if (vme == NULL)
        return false;

      /* Initialize vm_entry. */
      vme->type = VM_BIN;
      vme->vaddr = upage;
      vme->writable = writable;
      vme->is_loaded = false;
      vme->pinned = false;
      vme->file = file;  /* Use file_reopen to get a separate reference if needed */
      vme->offset = current_offset;
      vme->read_bytes = page_read_bytes;
      vme->zero_bytes = page_zero_bytes;
      vme->swap_slot = SWAP_SLOT_NONE;

      /* Insert into supplemental page table. */
      if (!vm_insert (&t->vm, vme))
        {
          free (vme);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      current_offset += page_read_bytes;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct thread *t = thread_current ();
  void *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
  uint8_t *kpage;
  struct vm_entry *vme;
  bool success = false;

  /* Allocate and initialize vm_entry for stack page. */
  vme = malloc (sizeof (struct vm_entry));
  if (vme == NULL)
    return false;

  vme->type = VM_ANON;
  vme->vaddr = upage;
  vme->writable = true;
  vme->is_loaded = true;  /* Will be loaded immediately */
  vme->pinned = false;
  vme->file = NULL;
  vme->offset = 0;
  vme->read_bytes = 0;
  vme->zero_bytes = PGSIZE;
  vme->swap_slot = SWAP_SLOT_NONE;

  /* Insert into supplemental page table. */
  if (!vm_insert (&t->vm, vme))
    {
      free (vme);
      return false;
    }

  /* Allocate frame and load the first stack page immediately. */
  kpage = allocate_frame (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    {
      vm_delete (&t->vm, vme);
      free (vme);
      return false;
    }

  /* Map the page. */
  success = install_page (upage, kpage, true);
  if (success)
    {
      *esp = PHYS_BASE;
      /* Set vm_entry in frame_entry for efficient lookup during eviction. */
      set_frame_vme (kpage, vme);
    }
  else
    {
      free_frame (kpage);
      vm_delete (&t->vm, vme);
      free (vme);
    }
  
  return success;
}

/* Parse command line arguments and set up the user stack according to
   80x86 calling convention. */
static bool
setup_args (const char *cmdline, void **esp)
{
  char *cmdline_copy;
  char *save_ptr;
  char *token;
  char **argv;  /* Array to store argument strings (in kernel memory) */
  void **argv_addrs;  /* Array to store addresses of args on user stack */
  int argc = 0;
  int i;
  char *stack_ptr;
  
  /* Make a copy of cmdline for parsing (use kernel heap memory) */
  cmdline_copy = palloc_get_page (0);
  if (cmdline_copy == NULL)
    return false;
  strlcpy (cmdline_copy, cmdline, PGSIZE);
  
  /* First pass: count arguments and store them in an array */
  argv = palloc_get_page (0);
  if (argv == NULL)
    {
      palloc_free_page (cmdline_copy);
      return false;
    }
  
  token = strtok_r (cmdline_copy, " ", &save_ptr);
  while (token != NULL)
    {
      /* Check if we have too many arguments */
      if (argc >= PGSIZE / sizeof (char *))
        {
          palloc_free_page (cmdline_copy);
          palloc_free_page (argv);
          return false;
        }
      argv[argc] = token;
      argc++;
      token = strtok_r (NULL, " ", &save_ptr);
    }
  
  if (argc == 0)
    {
      palloc_free_page (cmdline_copy);
      palloc_free_page (argv);
      return true;  /* No arguments to process */
    }
  
  /* Allocate space to store user stack addresses of arguments */
  argv_addrs = palloc_get_page (0);
  if (argv_addrs == NULL)
    {
      palloc_free_page (cmdline_copy);
      palloc_free_page (argv);
      return false;
    }
  
  /* Set up the user stack according to 80x86 calling convention */
  stack_ptr = (char *) *esp;
  
  /* Step 1: Push argument strings onto stack (from last to first)
     and record their addresses */
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;  /* Include null terminator */
      stack_ptr -= len;
      memcpy (stack_ptr, argv[i], len);
      argv_addrs[i] = stack_ptr;
    }
  
  /* Step 2: Word-align the stack pointer (round down to multiple of 4) */
  stack_ptr = (char *) ((uintptr_t) stack_ptr & ~3);
  
  /* Step 3: Push NULL pointer sentinel (argv[argc]) */
  stack_ptr -= sizeof (char *);
  *(char **) stack_ptr = NULL;
  
  /* Step 4: Push pointers to argument strings (argv[argc-1] to argv[0]) */
  for (i = argc - 1; i >= 0; i--)
    {
      stack_ptr -= sizeof (char *);
      *(char **) stack_ptr = argv_addrs[i];
    }
  
  /* Step 5: Push argv (pointer to argv[0]) */
  char **argv_ptr = (char **) stack_ptr;
  stack_ptr -= sizeof (char **);
  *(char ***) stack_ptr = argv_ptr;
  
  /* Step 6: Push argc */
  stack_ptr -= sizeof (int);
  *(int *) stack_ptr = argc;
  
  /* Step 7: Push fake return address */
  stack_ptr -= sizeof (void *);
  *(void **) stack_ptr = NULL;
  
  /* Update esp to point to the new stack top */
  *esp = stack_ptr;
  
  /* Verify stack pointer is still in valid range */
  if (*esp < (void *) 0x08048000 || *esp >= (void *) PHYS_BASE)
    {
      palloc_free_page (cmdline_copy);
      palloc_free_page (argv);
      palloc_free_page (argv_addrs);
      return false;
    }
  
  /* Free allocated kernel pages */
  palloc_free_page (cmdline_copy);
  palloc_free_page (argv);
  palloc_free_page (argv_addrs);
  
  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

