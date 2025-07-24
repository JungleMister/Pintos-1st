#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
static void check_address(const void *addr);
static struct file_descriptor * find_fd(int fd);

#endif /* userprog/syscall.h */
