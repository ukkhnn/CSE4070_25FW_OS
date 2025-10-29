#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
// [ADD]
int sys_fibonacci (int n);
int sys_max_of_four_int (int a, int b, int c, int d);
// [ADD] END

#endif /* userprog/syscall.h */
