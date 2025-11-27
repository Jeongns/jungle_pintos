#include "userprog/fd_util.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_SIZE 64

struct file* stdin_entry;
struct file* stdout_entry;

struct fd_table {
    int size;
    int next_fd;
    struct file** file_list;
};

static int fd_find_next(struct fd_table* fd_t);
static bool fd_table_expand(struct fd_table* fd_t);

void init_std_fds() {
    stdin_entry = (struct file*)malloc(sizeof(struct file*));
    stdout_entry = (struct file*)malloc(sizeof(struct file*));
    if (!stdin_entry) PANIC("malloc failed\n");
    if (!stdout_entry) PANIC("malloc failed\n");
}

bool fd_init(struct thread* t) {
    t->fd_table = malloc(sizeof(struct fd_table));
    if (t->fd_table == NULL) {
        free(t->fd_table);
        return false;
    }

    t->fd_table->size = DEFAULT_SIZE;
    t->fd_table->file_list = calloc(t->fd_table->size, sizeof(struct file*));

    if (t->fd_table->file_list == NULL) {
        free(t->fd_table->file_list);
        free(t->fd_table);
        return false;
    }

    t->fd_table->next_fd = 2;
    t->fd_table->file_list[0] = stdin_entry;
    t->fd_table->file_list[1] = stdout_entry;
    return true;
}

int fd_allocate(struct fd_table* fd_t, struct file* f) {
    if (f == NULL) return -1;
    int cur_fd = fd_t->next_fd;
    fd_t->file_list[cur_fd] = f;

    fd_t->next_fd = fd_find_next(fd_t);
    return cur_fd;
}

struct file* get_file(struct fd_table* fd_t, int fd) {
    if (fd < 0 || fd_t->size <= fd) return NULL;
    return fd_t->file_list[fd];
}

void fd_close(struct fd_table* fd_t, int fd) {
    struct file* file = get_file(fd_t, fd);
    if (file == NULL) return;

    fd_t->file_list[fd] = NULL;
    if (file != stdin_entry && file != stdout_entry) file_close(file);

    if (fd < fd_t->next_fd) fd_t->next_fd = fd;
}

bool copy_fd_table(struct fd_table* dst, struct fd_table* src) {
    struct file** new_file_list = calloc(src->size, sizeof(struct file*));

    if (new_file_list == NULL) {
        free(new_file_list);
        return false;
    }

    for (int i = 0; i < src->size; i++) {
        if (src->file_list[i] == NULL) continue;
        if (src->file_list[i] == stdin_entry || src->file_list[i] == stdout_entry) {
            new_file_list[i] = src->file_list[i];
            continue;
        }

        bool is_dup = false;
        if (file_reference_count(src->file_list[i]) > 1) {
            for (int j = 0; j < i; j++) {
                if (src->file_list[i] == src->file_list[j]) {
                    new_file_list[i] = file_dup2(new_file_list[j]);
                    is_dup = true;
                    break;
                }
            }
        }
        if (is_dup) continue;

        if ((new_file_list[i] = file_duplicate(src->file_list[i])) == NULL) return false;
    }

    free(dst->file_list);
    dst->next_fd = src->next_fd;
    dst->size = src->size;
    dst->file_list = new_file_list;
    return true;
}

void fd_clean(struct thread* t) {
    if (t->fd_table == NULL) return;
    for (int i = 2; i < t->fd_table->size; i++) {
        fd_close(t->fd_table, i);
    }
    free(t->fd_table->file_list);
    free(t->fd_table);
    t->fd_table = NULL;
}

int fd_dup2(struct fd_table* fd_t, int oldfd, int newfd) {
    if (oldfd == newfd) return newfd;

    struct file* file;
    if ((file = get_file(fd_t, oldfd)) == NULL) return -1;
    fd_close(fd_t, newfd);

    while (fd_t->size <= newfd) {
        if (!fd_table_expand(fd_t)) return -1;
    };

    if (file == stdin_entry || file == stdout_entry) {
        fd_t->file_list[newfd] = file;
        return newfd;
    }

    file_dup2(file);
    fd_t->file_list[newfd] = file;
    if (newfd == fd_t->next_fd) fd_t->next_fd = fd_find_next(fd_t);
    return newfd;
}

static int fd_find_next(struct fd_table* fd_t) {
    int start = fd_t->next_fd;
    while (1) {
        for (int i = start; i < fd_t->size; i++) {
            if (fd_t->file_list[i] == NULL) return i;
        }

        if (!fd_table_expand(fd_t)) return -1;
    }
}

static bool fd_table_expand(struct fd_table* fd_t) {
    int new_size = fd_t->size * 2;
    struct file** new_file_list = calloc(new_size, sizeof(struct file*));

    if (new_file_list == NULL) return false;

    memcpy(new_file_list, fd_t->file_list, fd_t->size * sizeof(struct file*));

    free(fd_t->file_list);
    fd_t->size = new_size;
    fd_t->file_list = new_file_list;

    return true;
}
