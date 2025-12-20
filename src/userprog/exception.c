#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "vm/page.h"
#include "vm/frame.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

/* Maximum stack size: 8MB */
#define STACK_MAX (1024 * 1024 * 8)

/* Helper to terminate current user process on unrecoverable fault. */
static void
exit_process_on_fault (void)
{
  struct thread *cur = running_thread ();
  cur->exit_status = -1;
  cur->has_exited = true;
  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
  thread_exit ();
}

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();
  
  //printf ("PF: 1. fault_addr=%p\n", fault_addr);

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* Kernel accesses to kernel addresses are kernel bugs. */
  if (!user && !is_user_vaddr (fault_addr))
    PANIC ("Kernel bug - unexpected page fault in kernel");

  /* Validate fault address. */
  if (!is_user_vaddr (fault_addr) || fault_addr == NULL)
    {
      /* Invalid user address - terminate the process */
      exit_process_on_fault ();
    }

  /* Get the page-aligned virtual address. */
  void *page_addr = pg_round_down (fault_addr);
  
  struct thread *cur = running_thread ();
  
 // printf ("PF: 2. page_addr=%p, thread=%s\n", page_addr, cur->name);

  /* Find the vm_entry in the supplemental page table. */
  struct vm_entry *vme = vm_find (&cur->vm, page_addr);
  
  /* Check if page is pinned (being evicted). */
  if (vme != NULL && vme->pinned)
    {
      /* Page is currently being evicted - terminate process */
      exit_process_on_fault ();
    }
  
  if (vme == NULL)
    {
      /* No vm_entry found - check if this is a stack growth case */
      void *user_esp;
      void *stack_bottom = (uint8_t *) PHYS_BASE - STACK_MAX;
      
      /* Get user stack pointer: use f->esp if user mode, otherwise use saved stack_ptr */
      if (user)
        user_esp = f->esp;
      else
        user_esp = cur->stack_ptr;
      
      /* Check stack growth conditions:
         1. fault_addr must be in user space (already validated above)
         2. fault_addr must be below PHYS_BASE - 8MB (stack max size limit)
         3. fault_addr must be within stack growth range: >= esp - 32 and < PHYS_BASE
      */
      if (is_user_vaddr (fault_addr) &&
          fault_addr >= stack_bottom &&
          user_esp != NULL &&
          fault_addr >= (uint8_t *) user_esp - 32 &&
          fault_addr < (uint8_t *) PHYS_BASE)
        {
          /* This is a valid stack growth - create VM_ANON entry */
          vme = malloc (sizeof (struct vm_entry));
          if (vme == NULL)
            {
              /* Failed to allocate vm_entry */
              cur->exit_status = -1;
              cur->has_exited = true;
              thread_exit ();
            }
          
          /* Initialize vm_entry for stack page */
          vme->type = VM_ANON;
          vme->vaddr = page_addr;
          vme->writable = true;
          vme->is_loaded = false;
          vme->pinned = false;
          vme->file = NULL;
          vme->offset = 0;
          vme->read_bytes = 0;
          vme->zero_bytes = PGSIZE;
          vme->swap_slot = SWAP_SLOT_NONE;
          
          /* Insert into supplemental page table */
          if (!vm_insert (&cur->vm, vme))
            {
              /* Failed to insert - entry might already exist, free and terminate */
              free (vme);
      cur->exit_status = -1;
      cur->has_exited = true;
      thread_exit ();
            }
          
          /* Continue with normal page loading flow below */
        }
      else
        {
          /* No vm_entry found and not a valid stack growth - invalid access */
          exit_process_on_fault ();
        }
    }

  /* Check if page is already loaded. */
  if (vme->is_loaded)
    {
      //printf ("PF: Page already loaded - checking permissions\n");
      /* Page is already loaded - should not fault unless there's a rights violation */
      if (!not_present && write && !vme->writable)
        {
          /* Writing to read-only page */
          exit_process_on_fault ();
        }
      /* Otherwise, this shouldn't happen - terminate */
      exit_process_on_fault ();
    }

  /* Check write permission. */
  if (write && !vme->writable)
    {
      /* Attempting to write to read-only page */
      cur->exit_status = -1;
      cur->has_exited = true;
      //printf ("%s: exit(-1)\n", cur->name);
      thread_exit ();
    }

  /* Allocate a frame for this page. */
  //printf ("PF: 4. Allocating frame\n");
  void *kpage = allocate_frame (PAL_USER);
  if (kpage == NULL)
    {
      /* Frame allocation failed */
      exit_process_on_fault ();
    }
  
  /* Load the page data. */
  if (!vm_load_page (vme, kpage))
    {
      /* Failed to load page data - free the frame and terminate */
      free_frame (kpage);
      exit_process_on_fault ();
    }

  /* Map the page in the page table. */
  if (!install_page (page_addr, kpage, vme->writable))
    {
      /* Failed to install page - free the frame and terminate */
      free_frame (kpage);
      exit_process_on_fault ();
    }

  /* Mark the page as loaded. */
  vme->is_loaded = true;
  
  /* Set vm_entry in frame_entry for efficient lookup during eviction. */
  set_frame_vme (kpage, vme);
}

