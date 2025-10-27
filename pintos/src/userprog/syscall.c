#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"

/* Type definitions for system calls */
typedef int pid_t;

/* Global file system lock for synchronization */
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void syscall_halt (void);
static void syscall_exit (int status);
static pid_t syscall_exec (const char *cmd_line);
static int syscall_wait (pid_t pid);
static bool syscall_create (const char *file, unsigned initial_size);
static bool syscall_remove (const char *file);
static int syscall_open (const char *file);
static int syscall_filesize (int fd);
static int syscall_read (int fd, void *buffer, unsigned size);
static int syscall_write (int fd, const void *buffer, unsigned size);
static void syscall_seek (int fd, unsigned position);
static unsigned syscall_tell (int fd);
static void syscall_close (int fd);
static bool is_valid_ptr (const void *ptr, unsigned size);
static bool check_user_address (const void *vaddr);
static bool check_user_string (const char *str);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_number;
  
  /* Get system call number from user stack with memory protection */
  if (!is_valid_ptr (f->esp, 4))
    {
      syscall_exit (-1);
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
          syscall_exit (-1);
        }
      syscall_exit (*(int *) (f->esp + 4));
      break;
      
    case SYS_EXEC:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      {
        const char *cmd_line = *(const char **) (f->esp + 4);
        if (!check_user_string (cmd_line))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_exec (cmd_line);
      }
      break;
      
    case SYS_WAIT:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      f->eax = syscall_wait (*(pid_t *) (f->esp + 4));
      break;
      
    case SYS_CREATE:
      if (!is_valid_ptr (f->esp + 4, 8))
        {
          syscall_exit (-1);
        }
      {
        const char *file = *(const char **) (f->esp + 4);
        unsigned initial_size = *(unsigned *) (f->esp + 8);
        if (!check_user_string (file))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_create (file, initial_size);
      }
      break;
      
    case SYS_REMOVE:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      {
        const char *file = *(const char **) (f->esp + 4);
        if (!check_user_string (file))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_remove (file);
      }
      break;
      
    case SYS_OPEN:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      {
        const char *file = *(const char **) (f->esp + 4);
        if (!check_user_string (file))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_open (file);
      }
      break;
      
    case SYS_FILESIZE:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      f->eax = syscall_filesize (*(int *) (f->esp + 4));
      break;
      
    case SYS_READ:
      if (!is_valid_ptr (f->esp + 4, 12))
        {
          syscall_exit (-1);
        }
      {
        int fd = *(int *) (f->esp + 4);
        void *buffer = *(void **) (f->esp + 8);
        unsigned size = *(unsigned *) (f->esp + 12);
        if (!is_valid_ptr (buffer, size))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_read (fd, buffer, size);
      }
      break;
      
    case SYS_WRITE:
      if (!is_valid_ptr (f->esp + 4, 12))
        {
          syscall_exit (-1);
        }
      {
        int fd = *(int *) (f->esp + 4);
        const void *buffer = *(void **) (f->esp + 8);
        unsigned size = *(unsigned *) (f->esp + 12);
        if (!is_valid_ptr (buffer, size))
          {
            syscall_exit (-1);
          }
        f->eax = syscall_write (fd, buffer, size);
      }
      break;
      
    case SYS_SEEK:
      if (!is_valid_ptr (f->esp + 4, 8))
        {
          syscall_exit (-1);
        }
      {
        int fd = *(int *) (f->esp + 4);
        unsigned position = *(unsigned *) (f->esp + 8);
        syscall_seek (fd, position);
      }
      break;
      
    case SYS_TELL:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      f->eax = syscall_tell (*(int *) (f->esp + 4));
      break;
      
    case SYS_CLOSE:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      syscall_close (*(int *) (f->esp + 4));
      break;
      
    case SYS_FIBONACCI:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      f->eax = syscall_fibonacci (*(int *) (f->esp + 4));
      break;
      
    case SYS_MAX_OF_FOUR_INT:
      if (!is_valid_ptr (f->esp + 4, 16))
        {
          syscall_exit (-1);
        }
      {
        int a = *(int *) (f->esp + 4);
        int b = *(int *) (f->esp + 8);
        int c = *(int *) (f->esp + 12);
        int d = *(int *) (f->esp + 16);
        f->eax = syscall_max_of_four_int (a, b, c, d);
      }
      break;
      
    default:
      printf ("Unknown system call: %d\n", syscall_number);
      syscall_exit (-1);
    }
}

/* Check if a single user virtual address is valid */
static bool
check_user_address (const void *vaddr)
{
  struct thread *cur = thread_current ();
  
  /* Check for NULL pointer */
  if (vaddr == NULL)
    return false;
    
  /* Check if address is in user space */
  if (!is_user_vaddr (vaddr))
    return false;
    
  /* Check if the page is mapped */
  if (pagedir_get_page (cur->pagedir, vaddr) == NULL)
    return false;
    
  return true;
}

/* Check if a user string is valid (null-terminated) */
static bool
check_user_string (const char *str)
{
  if (!check_user_address (str))
    return false;
    
  /* Check if string is null-terminated within valid memory */
  for (const char *p = str; ; p++)
    {
      if (!check_user_address (p))
        return false;
      if (*p == '\0')
        break;
    }
    
  return true;
}

/* Check if a pointer range is valid for user access */
static bool
is_valid_ptr (const void *ptr, unsigned size)
{
  const char *start = (const char *) ptr;
  const char *end = start + size;
  
  /* Check for NULL pointer */
  if (ptr == NULL)
    return false;
    
  /* Check if address is in user space */
  if (!is_user_vaddr (ptr))
    return false;
    
  /* Check for overflow */
  if (end < start)
    return false;
    
  /* Check if range extends beyond user space */
  if (end > (const char *) PHYS_BASE)
    return false;
    
  /* Check if all pages in the range are mapped */
  for (const char *p = (const char *) pg_round_down (start); 
       p < end; 
       p += PGSIZE)
    {
      if (!check_user_address (p))
        return false;
    }
    
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
  
  /* Set exit status and mark as exited */
  cur->exit_status = status;
  cur->has_exited = true;
  
  /* Print exit message */
  printf ("%s: exit(%d)\n", cur->name, status);
  
  /* Call thread_exit which will trigger process_exit */
  thread_exit ();
}

/* Start another process */
static pid_t
syscall_exec (const char *cmd_line)
{
  return process_execute (cmd_line);
}

/* Wait for a child process to die */
static int
syscall_wait (pid_t pid)
{
  return process_wait (pid);
}

/* Create a file */
static bool
syscall_create (const char *file, unsigned initial_size)
{
  /* Validate file pointer */
  if (!check_user_string (file))
    return false;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Create file using file system */
  bool success = filesys_create (file, initial_size);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  return success;
}

/* Delete a file */
static bool
syscall_remove (const char *file)
{
  /* TODO: Implement file removal */
  printf ("remove: %s (not implemented)\n", file);
  return false;
}

/* Open a file */
static int
syscall_open (const char *file)
{
  /* Validate file pointer */
  if (!check_user_string (file))
    return -1;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Open file using file system */
  struct file *opened_file = filesys_open (file);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  /* If file opening failed, return -1 */
  if (opened_file == NULL)
    return -1;
  
  /* Find available file descriptor in current thread's table */
  struct thread *cur = thread_current ();
  for (int fd = 2; fd < FD_MAX; fd++)
    {
      if (cur->fd_table[fd] == NULL)
        {
          cur->fd_table[fd] = opened_file;
          return fd;
        }
    }
  
  /* No available file descriptor found, close the file and return -1 */
  lock_acquire (&filesys_lock);
  file_close (opened_file);
  lock_release (&filesys_lock);
  
  return -1;
}

/* Obtain a file's size */
static int
syscall_filesize (int fd)
{
  /* Validate file descriptor */
  if (fd < 0 || fd >= FD_MAX)
    return -1;
  
  struct thread *cur = thread_current ();
  struct file *file = cur->fd_table[fd];
  
  /* Check if file descriptor is valid */
  if (file == NULL)
    return -1;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Get file size */
  off_t size = file_length (file);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  return (int) size;
}

/* Read from a file */
static int
syscall_read (int fd, void *buffer, unsigned size)
{
  /* Validate buffer pointer */
  if (!is_valid_ptr (buffer, size))
    return -1;
  
  if (fd == 0)  /* stdin */
    {
      /* Read from input device - no filesys_lock needed */
      unsigned bytes_read = 0;
      char *buf = (char *) buffer;
      
      while (bytes_read < size)
        {
          char c = input_getc ();
          buf[bytes_read] = c;
          bytes_read++;
          
          /* Stop reading on newline or EOF */
          if (c == '\n' || c == '\0')
            break;
        }
      
      return bytes_read;
    }
  else if (fd == 1)  /* stdout - cannot read from stdout */
    {
      return -1;
    }
  else if (fd >= 2)  /* regular file */
    {
      /* Validate file descriptor range */
      if (fd >= FD_MAX)
        return -1;
      
      struct thread *cur = thread_current ();
      struct file *file = cur->fd_table[fd];
      
      /* Check if file descriptor is valid */
      if (file == NULL)
        return -1;
      
      /* Acquire file system lock */
      lock_acquire (&filesys_lock);
      
      /* Read from file */
      off_t bytes_read = file_read (file, buffer, size);
      
      /* Release file system lock */
      lock_release (&filesys_lock);
      
      return (int) bytes_read;
    }
  else  /* invalid fd */
    {
      return -1;
    }
}

/* Write to a file descriptor */
static int
syscall_write (int fd, const void *buffer, unsigned size)
{
  /* Validate buffer pointer */
  if (!is_valid_ptr (buffer, size))
    return -1;
  
  if (fd == 0)  /* stdin - cannot write to stdin */
    {
      return -1;
    }
  else if (fd == 1 || fd == 2)  /* stdout or stderr */
    {
      /* Write to console - no filesys_lock needed */
      putbuf (buffer, size);
      return size;
    }
  else if (fd >= 2)  /* regular file */
    {
      /* Validate file descriptor range */
      if (fd >= FD_MAX)
        return -1;
      
      struct thread *cur = thread_current ();
      struct file *file = cur->fd_table[fd];
      
      /* Check if file descriptor is valid */
      if (file == NULL)
        return -1;
      
      /* Acquire file system lock */
      lock_acquire (&filesys_lock);
      
      /* Write to file */
      off_t bytes_written = file_write (file, buffer, size);
      
      /* Release file system lock */
      lock_release (&filesys_lock);
      
      return (int) bytes_written;
    }
  else  /* invalid fd */
    {
      return -1;
    }
}

/* Change position in a file */
static void
syscall_seek (int fd, unsigned position)
{
  /* TODO: Implement file seeking */
  printf ("seek: fd %d, position %u (not implemented)\n", fd, position);
}

/* Report current position in a file */
static unsigned
syscall_tell (int fd)
{
  /* TODO: Implement file position reporting */
  printf ("tell: fd %d (not implemented)\n", fd);
  return -1;
}

/* Close a file */
static void
syscall_close (int fd)
{
  /* Validate file descriptor (0 and 1 are reserved for stdin/stdout) */
  if (fd < 2 || fd >= FD_MAX)
    return;
  
  struct thread *cur = thread_current ();
  struct file *file = cur->fd_table[fd];
  
  /* Check if file descriptor is valid */
  if (file == NULL)
    return;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Close file */
  file_close (file);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  /* Clear file descriptor table entry */
  cur->fd_table[fd] = NULL;
}

/* Calculate the Nth Fibonacci number */
int
syscall_fibonacci (int n)
{
  if (n <= 0)
    return 0;
  if (n == 1 || n == 2)
    return 1;
  
  /* Use iteration to avoid stack overflow for large n */
  int prev = 1;  /* F(n-2) */
  int curr = 1;  /* F(n-1) */
  int result = 0;
  
  for (int i = 3; i <= n; i++)
    {
      result = prev + curr;
      prev = curr;
      curr = result;
    }
  
  return result;
}

/* Return the maximum of four integers */
int
syscall_max_of_four_int (int a, int b, int c, int d)
{
  int max = a;
  
  if (b > max)
    max = b;
  if (c > max)
    max = c;
  if (d > max)
    max = d;
  
  return max;
}
