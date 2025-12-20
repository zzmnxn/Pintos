#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <round.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"

/* Type definitions for system calls */
typedef int pid_t;
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

/* Global file system lock for synchronization */
struct lock filesys_lock;

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
static mapid_t syscall_mmap (int fd, void *addr);
static void syscall_munmap (mapid_t mapid);
static void pin_buffer (void *buffer, unsigned size);
static void unpin_buffer (void *buffer, unsigned size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* Returns true if the current thread holds filesys_lock, false otherwise. */
bool
filesys_lock_held_by_current_thread (void)
{
  return lock_held_by_current_thread (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* Save user stack pointer for stack growth handling during page faults. */
  thread_current()->stack_ptr = f->esp;
  
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
      
    case SYS_MMAP:
      if (!is_valid_ptr (f->esp + 4, 8))
        {
          syscall_exit (-1);
        }
      {
        int fd = *(int *) (f->esp + 4);
        void *addr = *(void **) (f->esp + 8);
        /* Simple address validation without mapping check */
        if (addr == NULL || !is_user_vaddr (addr) || pg_ofs (addr) != 0)
          {
            f->eax = MAP_FAILED;
            break;
          }
        f->eax = syscall_mmap (fd, addr);
      }
      break;
      
    case SYS_MUNMAP:
      if (!is_valid_ptr (f->esp + 4, 4))
        {
          syscall_exit (-1);
        }
      syscall_munmap (*(mapid_t *) (f->esp + 4));
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
  
  struct thread *cur = thread_current ();  // 여기서 Panic 발생 가능!
  
  
  /* Check for NULL pointer */
  if (vaddr == NULL)
    return false;
    
  /* Check if address is in user space */
  if (!is_user_vaddr (vaddr))
    return false;
    
  /* If already known in SPT, accept. */
  void *page_addr = pg_round_down (vaddr);
  if (vm_find (&cur->vm, page_addr) != NULL)
    return true;

  /* Otherwise, allow potential stack growth: address must be near the
     current user stack pointer and below PHYS_BASE. */
  void *esp = cur->stack_ptr;
  if (esp != NULL &&
      vaddr >= (uint8_t *) esp - 32 &&
      vaddr < (void *) PHYS_BASE)
    return true;

  /* No vm_entry and not a plausible stack growth candidate. */
  return false;
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

/* Check if a file descriptor is valid and get the associated file */
static struct file *
get_file_from_fd (int fd)
{
  /* Check if fd is in valid range */
  if (fd < 0 || fd >= FD_MAX)
    return NULL;
  
  struct thread *cur = thread_current ();
  
  /* For stdin (0) and stdout (1), return NULL as they are special */
  if (fd < 2)
    return NULL;
  
  /* Check if file descriptor is open */
  return cur->fd_table[fd];
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
  /* Validate file pointer */
  if (!check_user_string (file))
    return false;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Remove file using file system */
  bool success = filesys_remove (file);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  return success;
}

/* Pin buffer pages to prevent eviction during I/O operations */
static void
pin_buffer (void *buffer, unsigned size)
{
  struct thread *cur = thread_current ();
  void *start = pg_round_down (buffer);
  void *end = pg_round_down ((uint8_t *) buffer + size);
  
  for (void *page = start; page <= end; page = (uint8_t *) page + PGSIZE)
    {
      struct vm_entry *vme = vm_find (&cur->vm, page);
      
      if (vme != NULL)
        {
          /* If page is not loaded, load it first */
          if (!vme->is_loaded)
            {
              void *kpage = allocate_frame (PAL_USER);
              if (kpage != NULL)
                {
                  if (vm_load_page (vme, kpage))
                    {
                      if (install_page (page, kpage, vme->writable))
                        {
                          vme->is_loaded = true;
                          
                          /* For VM_FILE pages, initialize dirty bit to false */
                          if (vme->type == VM_FILE)
                            {
                              pagedir_set_dirty (cur->pagedir, page, false);
                            }
                          
                          set_frame_vme (kpage, vme);
                        }
                      else
                        {
                          free_frame (kpage);
                        }
                    }
                  else
                    {
                      free_frame (kpage);
                    }
                }
            }
          
          /* Pin the page */
          vme->pinned = true;
        }
    }
}

/* Unpin buffer pages after I/O operations */
static void
unpin_buffer (void *buffer, unsigned size)
{
  struct thread *cur = thread_current ();
  void *start = pg_round_down (buffer);
  void *end = pg_round_down ((uint8_t *) buffer + size);
  
  for (void *page = start; page <= end; page = (uint8_t *) page + PGSIZE)
    {
      struct vm_entry *vme = vm_find (&cur->vm, page);
      
      if (vme != NULL)
        {
          /* Unpin the page */
          vme->pinned = false;
        }
    }
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
  /* Get file from file descriptor */
  struct file *file = get_file_from_fd (fd);
  
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
      /* Get file from file descriptor */
      struct file *file = get_file_from_fd (fd);
      
      /* Check if file descriptor is valid */
      if (file == NULL)
        return -1;
      
      /* Pin buffer pages to prevent eviction during I/O */
      pin_buffer (buffer, size);
      
      /* Acquire file system lock */
      lock_acquire (&filesys_lock);
      
      /* Read from file */
      off_t bytes_read = file_read (file, buffer, size);
      
      /* Release file system lock */
      lock_release (&filesys_lock);
      
      /* Unpin buffer pages after I/O */
      unpin_buffer (buffer, size);
      
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
  else if (fd == 1)  /* stdout - write to console */
    {
      /* Write to console - no filesys_lock needed */
      putbuf (buffer, size);
      return size;
    }
  else if (fd >= 2)  /* regular file (fd >= 2) */
    {
      /* Get file from file descriptor */
      struct file *file = get_file_from_fd (fd);
      
      /* Check if file descriptor is valid */
      if (file == NULL)
        return -1;
      
      /* Pin buffer pages to prevent eviction during I/O */
      pin_buffer ((void *) buffer, size);
      
      /* Acquire file system lock */
      lock_acquire (&filesys_lock);
      
      /* Write to file */
      off_t bytes_written = file_write (file, buffer, size);
      
      /* Release file system lock */
      lock_release (&filesys_lock);
      
      /* Unpin buffer pages after I/O */
      unpin_buffer ((void *) buffer, size);
      
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
  /* Get file from file descriptor */
  struct file *file = get_file_from_fd (fd);
  
  /* Check if file descriptor is valid */
  if (file == NULL)
    return;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Seek to position in file */
  file_seek (file, position);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
}

/* Report current position in a file */
static unsigned
syscall_tell (int fd)
{
  /* Get file from file descriptor */
  struct file *file = get_file_from_fd (fd);
  
  /* Check if file descriptor is valid */
  if (file == NULL)
    return -1;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Get current position in file */
  off_t position = file_tell (file);
  
  /* Release file system lock */
  lock_release (&filesys_lock);
  
  return (unsigned) position;
}

/* Close a file */
static void
syscall_close (int fd)
{
  /* Get file from file descriptor */
  struct file *file = get_file_from_fd (fd);
  
  /* Check if file descriptor is valid */
  if (file == NULL)
    return;
  
  struct thread *cur = thread_current ();
  
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

/* Map a file into memory */
static mapid_t
syscall_mmap (int fd, void *addr)
{
  struct thread *cur = thread_current ();
  struct file *file;
  struct file *mapped_file;
  off_t file_size;
  void *vaddr;
  off_t offset;
  uint32_t read_bytes, zero_bytes;
  struct vm_entry *vme;
  struct hash_iterator i;
  void *end_addr;
  
  /* Validate addr: must be page-aligned, not NULL, not 0 */
  if (addr == NULL || addr == 0 || pg_ofs (addr) != 0)
    return MAP_FAILED;
  
  /* Validate fd: must be >= 2 (not stdin/stdout) */
  if (fd < 2)
    return MAP_FAILED;
  
  /* Get file from file descriptor */
  file = get_file_from_fd (fd);
  if (file == NULL)
    return MAP_FAILED;
  
  /* Acquire file system lock */
  lock_acquire (&filesys_lock);
  
  /* Get file size */
  file_size = file_length (file);
  
  /* Check file size > 0 */
  if (file_size == 0)
    {
      lock_release (&filesys_lock);
      return MAP_FAILED;
    }
  
  /* Calculate end address of mapping */
  end_addr = (uint8_t *) addr + file_size;
  
  /* Check for overlapping mappings by iterating through existing vm_entries */
  hash_first (&i, &cur->vm);
  while (hash_next (&i))
    {
      struct vm_entry *existing_vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
      void *existing_end = (uint8_t *) existing_vme->vaddr + PGSIZE;
      
      /* Check if new mapping overlaps with existing mapping */
      if (addr < existing_end && existing_vme->vaddr < end_addr)
        {
          lock_release (&filesys_lock);
          return MAP_FAILED;
        }
    }
  
  /* Use file_reopen to get separate file reference for the mapping */
  mapped_file = file_reopen (file);
  if (mapped_file == NULL)
    {
      lock_release (&filesys_lock);
      return MAP_FAILED;
    }
  
  /* Release lock before creating vm_entries (may take time) */
  lock_release (&filesys_lock);
  
  /* Create vm_entry for each page */
  vaddr = addr;
  offset = 0;
  
  while (offset < file_size)
    {
      /* Calculate bytes for this page */
      read_bytes = file_size - offset < PGSIZE ? file_size - offset : PGSIZE;
      zero_bytes = PGSIZE - read_bytes;
      
      /* Allocate vm_entry */
      vme = malloc (sizeof (struct vm_entry));
      if (vme == NULL)
        {
          /* Cleanup: remove all vm_entries we've created so far */
          void *cleanup_addr = addr;
          while (cleanup_addr < vaddr)
            {
              struct vm_entry *cleanup_vme = vm_find (&cur->vm, cleanup_addr);
              if (cleanup_vme != NULL)
                {
                  vm_delete (&cur->vm, cleanup_vme);
                  if (cleanup_vme->file != NULL)
                    {
                      lock_acquire (&filesys_lock);
                      file_close (cleanup_vme->file);
                      lock_release (&filesys_lock);
                    }
                  free (cleanup_vme);
                }
              cleanup_addr = (uint8_t *) cleanup_addr + PGSIZE;
            }
          /* Close the mapped file */
          lock_acquire (&filesys_lock);
          file_close (mapped_file);
          lock_release (&filesys_lock);
          return MAP_FAILED;
        }
      
      /* Initialize vm_entry */
      vme->type = VM_FILE;
      vme->vaddr = vaddr;
      vme->writable = true;
      vme->is_loaded = false;
      vme->pinned = false;
      vme->file = mapped_file;  /* All pages share the same file reference */
      vme->offset = offset;
      vme->read_bytes = read_bytes;
      vme->zero_bytes = zero_bytes;
      vme->swap_slot = SWAP_SLOT_NONE;
      
      /* Insert into supplemental page table */
      if (!vm_insert (&cur->vm, vme))
        {
          /* Failed to insert - entry might already exist */
          free (vme);
          /* Cleanup: remove all vm_entries we've created so far */
          void *cleanup_addr = addr;
          while (cleanup_addr < vaddr)
            {
              struct vm_entry *cleanup_vme = vm_find (&cur->vm, cleanup_addr);
              if (cleanup_vme != NULL)
                {
                  vm_delete (&cur->vm, cleanup_vme);
                  free (cleanup_vme);
                }
              cleanup_addr = (uint8_t *) cleanup_addr + PGSIZE;
            }
          /* Close the mapped file */
          lock_acquire (&filesys_lock);
          file_close (mapped_file);
          lock_release (&filesys_lock);
          return MAP_FAILED;
        }
      
      /* Advance to next page */
      vaddr = (uint8_t *) vaddr + PGSIZE;
      offset += read_bytes;
    }
  
  /* Return starting address as mapid */
  return (mapid_t) addr;
}

/* Unmap a memory mapping */
static void
syscall_munmap (mapid_t mapid)
{
  struct thread *cur = thread_current ();
  void *start_addr = (void *) mapid;
  struct vm_entry *vme;
  void *kpage;
  bool dirty;
  struct file *mapped_file = NULL;
  bool file_closed = false;
  struct hash_iterator i;
  struct vm_entry *to_unmap[256];  /* Array to collect vm_entries (reasonable limit) */
  int unmap_count = 0;
  int j;
  
  /* First pass: find the mapping and identify the file */
  hash_first (&i, &cur->vm);
  while (hash_next (&i))
    {
      vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
      
      /* Check if this vm_entry belongs to the mapping */
      if (vme->type == VM_FILE && vme->vaddr == start_addr)
        {
          /* Found the start of the mapping - remember the file */
          mapped_file = vme->file;
          break;
        }
    }
  
  /* If mapping not found, return */
  if (mapped_file == NULL)
    return;
  
  /* Second pass: collect all vm_entries in the mapping */
  hash_first (&i, &cur->vm);
  while (hash_next (&i))
    {
      vme = hash_entry (hash_cur (&i), struct vm_entry, hash_elem);
      
      /* Check if this vm_entry belongs to the mapping */
      if (vme->type == VM_FILE && vme->file == mapped_file && vme->vaddr >= start_addr)
        {
          /* Add to array for processing */
          if (unmap_count < 256)
            to_unmap[unmap_count++] = vme;
        }
    }
  
  /* Third pass: unmap each vm_entry */
  for (j = 0; j < unmap_count; j++)
    {
      vme = to_unmap[j];
      
      /* Check if page is loaded */
      if (vme->is_loaded)
        {
          /* Get physical page */
          kpage = pagedir_get_page (cur->pagedir, vme->vaddr);
          
          if (kpage != NULL)
            {
              /* Pin the page to prevent eviction during write-back */
              vme->pinned = true;
              
              /* Check if page is dirty */
              dirty = pagedir_is_dirty (cur->pagedir, vme->vaddr);
              
              /* Write-back for VM_FILE type pages (mmap files) */
              if (vme->type == VM_FILE && dirty && vme->file != NULL)
                {
                  /* Write back dirty page to file (with lock protection) */
                  lock_acquire (&filesys_lock);
                  file_write_at (vme->file, kpage, vme->read_bytes, vme->offset);
                  lock_release (&filesys_lock);
                  
                  /* Reset dirty bit after write-back */
                  pagedir_set_dirty (cur->pagedir, vme->vaddr, false);
                }
              
              /* Unpin the page after write-back is complete */
              vme->pinned = false;
              
              /* Clear page mapping */
              pagedir_clear_page (cur->pagedir, vme->vaddr);
              
              /* Remove frame from table and free physical memory */
              remove_frame_from_table (kpage);
              palloc_free_page (kpage);
            }
        }
      
      /* Delete from SPT */
      vm_delete (&cur->vm, vme);
      
      /* Close file once (all pages share the same file reference) */
      if (!file_closed && vme->file != NULL)
        {
          lock_acquire (&filesys_lock);
          file_close (vme->file);
          lock_release (&filesys_lock);
          file_closed = true;
        }
      
      /* Free vm_entry struct */
      free (vme);
    }
}
