#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"

static void syscall_handler (struct intr_frame *);
static void syscall_halt (void);
static void syscall_exit (int status);
static int syscall_write (int fd, const void *buffer, unsigned size);
static bool is_valid_ptr (const void *ptr, unsigned size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_number;
  
  /* Get system call number from user stack */
  if (!is_valid_ptr (f->esp, 4))
    {
      printf ("Invalid stack pointer\n");
      thread_exit ();
    }
  
  syscall_number = *(int *) f->esp;
  
  /* Handle different system calls */
  switch (syscall_number)
    {
    case SYS_HALT:
      syscall_halt ();
      break;
    case SYS_EXIT:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          printf ("Invalid exit status pointer\n");
          thread_exit ();
        }
      syscall_exit (*(int *) (f->esp + 4));
      break;
    case SYS_WRITE:
      if (!is_valid_ptr (f->esp + 4, 12))
        {
          printf ("Invalid write arguments\n");
          thread_exit ();
        }
      {
        int fd = *(int *) (f->esp + 4);
        const void *buffer = *(void **) (f->esp + 8);
        unsigned size = *(unsigned *) (f->esp + 12);
        f->eax = syscall_write (fd, buffer, size);
      }
      break;
    default:
      printf ("Unknown system call: %d\n", syscall_number);
      thread_exit ();
    }
}

/* Check if a pointer is valid for user access */
static bool
is_valid_ptr (const void *ptr, unsigned size)
{
  if (!is_user_vaddr (ptr))
    return false;
  if (ptr + size < ptr)  /* Check for overflow */
    return false;
  if (ptr + size > PHYS_BASE)
    return false;
  return true;
}

/* Halt the system */
static void
syscall_halt (void)
{
  shutdown_power_off ();
}

/* Exit the current process */
static void
syscall_exit (int status)
{
  struct thread *cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

/* Write to a file descriptor */
static int
syscall_write (int fd, const void *buffer, unsigned size)
{
  if (fd == 1)  /* stdout */
    {
      if (!is_valid_ptr (buffer, size))
        return -1;
      putbuf (buffer, size);
      return size;
    }
  else
    return -1;  /* Unsupported file descriptor */
}
