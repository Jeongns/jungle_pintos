#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "user/syscall.h"
#include "userprog/fd_util.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/validate.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

#define MAX_FILE_NAME_LEN 16

struct lock file_lock;

static void syscall_halt(void);
static void syscall_exit(int status);
static pid_t syscall_fork(const char *thread_name, struct intr_frame *if_);
static int syscall_exec(const char *cmd_line);
static int syscall_wait(int pid);
static bool syscall_create(const char *file, unsigned initial_size);
static bool syscall_remove(const char *file);
static int syscall_open(const char *file);
static int syscall_filesize(int fd);
static int syscall_read(int fd, void *buffer, unsigned size);
static int syscall_write(int fd, const void *buffer, unsigned size);
static void syscall_seek(int fd, unsigned position);
static unsigned syscall_tell(int fd);
static void syscall_close(int fd);
static int syscall_dup2(int oldfd, int newfd);

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init(&file_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
	uint64_t arg1 = f->R.rdi, arg2 = f->R.rsi, arg3 = f->R.rdx;
	switch (f->R.rax) {
		case SYS_HALT:
			syscall_halt();
			break;
		case SYS_EXIT:
			syscall_exit(arg1);
			break;
		case SYS_FORK:
			f->R.rax = syscall_fork(arg1, f);
			break;
		case SYS_EXEC:
			f->R.rax = syscall_exec(arg1);
			break;
		case SYS_WAIT:
			f->R.rax = syscall_wait(arg1);
			break;
		case SYS_CREATE:
			f->R.rax = syscall_create(arg1, arg2);
			break;
		case SYS_REMOVE:
			f->R.rax = syscall_remove(arg1);
			break;
		case SYS_OPEN:
			f->R.rax = syscall_open(arg1);
			break;
		case SYS_FILESIZE:
			f->R.rax = syscall_filesize(arg1);
			break;
		case SYS_READ:
			f->R.rax = syscall_read(arg1, arg2, arg3);
			break;
		case SYS_WRITE:
			f->R.rax = syscall_write(arg1, arg2, arg3);
			break;
		case SYS_SEEK:
			syscall_seek(arg1, arg2);
			break;
		case SYS_TELL:
			f->R.rax = syscall_tell(arg1);
			break;
		case SYS_CLOSE:
			syscall_close(arg1);
			break;
		case SYS_DUP2:
			f->R.rax = syscall_dup2(arg1, arg2);
			break;
	}
}

static void syscall_halt(void)
{
	power_off();
}

static void syscall_exit(int status)
{
	thread_current()->my_entry->exit_status = status;
	thread_exit();
}

static pid_t syscall_fork(const char *thread_name, struct intr_frame *if_)
{
	char *kernel_thread_name[MAX_FILE_NAME_LEN];
	if (!copy_user_string(kernel_thread_name, thread_name, MAX_FILE_NAME_LEN))
		return TID_ERROR;

	return process_fork(kernel_thread_name, if_);
}

static int syscall_exec(const char *cmd_line)
{
	char *kernel_cmd_line = palloc_get_page(0);

	if (!copy_user_string(kernel_cmd_line, cmd_line, PGSIZE))
		syscall_exit(-1);

	process_exec(kernel_cmd_line);
	syscall_exit(-1);
}

static int syscall_wait(int pid)
{
	return process_wait(pid);
}

static bool syscall_create(const char *file, unsigned initial_size)
{
	char *kernel_file_name[MAX_FILE_NAME_LEN];

	if (!copy_user_string(kernel_file_name, file, MAX_FILE_NAME_LEN))
		return false;

	lock_acquire(&file_lock);
	bool success = filesys_create(kernel_file_name, initial_size);
	lock_release(&file_lock);

	return success;
}

static bool syscall_remove(const char *file)
{
	char *kernel_file_name[MAX_FILE_NAME_LEN];

	if (!copy_user_string(kernel_file_name, file, MAX_FILE_NAME_LEN))
		return false;

	lock_acquire(&file_lock);
	bool success = filesys_remove(kernel_file_name);
	lock_release(&file_lock);

	return success;
}

static int syscall_open(const char *file)
{
	char *kernel_file_name[MAX_FILE_NAME_LEN];

	if (!copy_user_string(kernel_file_name, file, MAX_FILE_NAME_LEN))
		return -1;

	lock_acquire(&file_lock);
	struct file *open_file = filesys_open(kernel_file_name);
	lock_release(&file_lock);

	if (open_file == NULL)
		return -1;

	int result;
	if ((result = fd_allocate(thread_current()->fd_table, open_file)) == -1)
		file_close(open_file);
	return result;
}

static int syscall_filesize(int fd)
{
	int result;

	struct file *file = get_file(thread_current()->fd_table, fd);
	if (file == NULL || file == stdin_entry || file == stdout_entry)
		return -1;

	lock_acquire(&file_lock);
	result = file_length(file);
	lock_release(&file_lock);

	return result;
}

static int syscall_read(int fd, void *buffer, unsigned size)
{
	if (size == 0)
		return 0;

	int result;
	char *kernel_buffer = malloc(size);

	struct file *file = get_file(thread_current()->fd_table, fd);
	if (file == NULL || file == stdout_entry) {
		free(kernel_buffer);
		return -1;
	}

	lock_acquire(&file_lock);
	if (file == stdin_entry) {
		for (int i = 0; i < size; i++)
			kernel_buffer[i] = input_getc();
		result = size;
	} else {
		result = file_read(file, kernel_buffer, size);
	}
	lock_release(&file_lock);

	if (!buffer_copy_to_user(buffer, kernel_buffer, result))
		syscall_exit(-1);

	free(kernel_buffer);
	return result;
}

static int syscall_write(int fd, const void *buffer, unsigned size)
{
	if (size == 0)
		return 0;

	int result;
	char *kernel_buffer = malloc(size);
	if (!copy_user_buffer(kernel_buffer, buffer, size)) {
		free(kernel_buffer);
		return -1;
	}

	struct file *file = get_file(thread_current()->fd_table, fd);

	if (file == NULL || file == stdin_entry) {
		free(kernel_buffer);
		return -1;
	}

	lock_acquire(&file_lock);
	if (file == stdout_entry) {
		putbuf(kernel_buffer, size);
		result = size;
	} else {
		result = file_write(file, kernel_buffer, size);
	}
	lock_release(&file_lock);

	free(kernel_buffer);
	return result;
}

static void syscall_seek(int fd, unsigned position)
{
	struct file *file = get_file(thread_current()->fd_table, fd);

	if (file == NULL)
		return;

	lock_acquire(&file_lock);
	file_seek(file, position);
	lock_release(&file_lock);
}

static unsigned syscall_tell(int fd)
{
	struct file *file = get_file(thread_current()->fd_table, fd);
	if (!file)
		return 0;

	lock_acquire(&file_lock);
	unsigned result = file_tell(file);
	lock_release(&file_lock);
	return result;
}

static void syscall_close(int fd)
{
	lock_acquire(&file_lock);
	fd_close(thread_current()->fd_table, fd);
	lock_release(&file_lock);
}

static int syscall_dup2(int oldfd, int newfd)
{
	lock_acquire(&file_lock);
	int result = fd_dup2(thread_current()->fd_table, oldfd, newfd);
	lock_release(&file_lock);
	return result;
}