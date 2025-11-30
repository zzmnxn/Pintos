#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

/* Global file system lock for synchronization */
extern struct lock filesys_lock;

void syscall_init (void);
int syscall_fibonacci (int n);
int syscall_max_of_four_int (int a, int b, int c, int d);

#endif
