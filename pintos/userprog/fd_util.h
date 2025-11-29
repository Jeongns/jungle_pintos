#ifndef FD_UTIL_H
#define FD_UTIL_H

#include <stdbool.h>

#include "filesys/file.h"
#include "threads/thread.h"

extern struct file *stdin_entry;
extern struct file *stdout_entry;

struct fd_table;
void init_std_fds();
bool fd_init(struct thread *t);
int fd_allocate(struct fd_table *fd_t, struct file *f);
struct file *get_file(struct fd_table *fd_t, int fd);
void fd_close(struct fd_table *fd_t, int fd);
void fd_clean(struct thread *t);
bool copy_fd_table(struct fd_table *dst, struct fd_table *src);
int fd_dup2(struct fd_table *fd_t, int oldfd, int newfd);
#endif