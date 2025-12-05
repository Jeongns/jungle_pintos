#include "userprog/validate.h"

#include "threads/thread.h"
#include "threads/vaddr.h"

static int64_t get_user(const uint8_t *uaddr);
static bool put_user(uint8_t *udst, uint8_t byte);

bool copy_user_buffer(char *kernel_dst, const char *user_src, size_t max_len)
{
	if (kernel_dst == NULL || user_src == NULL || !is_user_vaddr(user_src))
		thread_exit();

	for (size_t i = 0; i < max_len; i++) {
		int64_t user_char = get_user(user_src + i);
		if (user_char == -1)
			thread_exit();
		kernel_dst[i] = (char)user_char;
	}
	return true;
}

bool copy_user_string(char *kernel_dst, const char *user_src, size_t max_len)
{
	if (kernel_dst == NULL || user_src == NULL || !is_user_vaddr(user_src))
		thread_exit();

	for (size_t i = 0; i < max_len; i++) {
		int64_t user_char = get_user(user_src + i);
		if (user_char == -1)
			thread_exit();
		kernel_dst[i] = (char)user_char;
		if (kernel_dst[i] == '\0')
			return true;
	}
	return false;
}

bool buffer_copy_to_user(char *user_dst, const char *kernel_src, size_t max_len)
{
	if (user_dst == NULL || kernel_src == NULL || !is_user_vaddr(user_dst))
		thread_exit();
	for (size_t i = 0; i < max_len; i++) {
		if (!put_user(user_dst + i, kernel_src[i]))
			thread_exit();
	}
	return true;
}

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t get_user(const uint8_t *uaddr)
{
	int64_t result;
	__asm __volatile("movabsq $done_get, %0\n"
					 "movzbq %1, %0\n"
					 "done_get:\n"
					 : "=&a"(result)
					 : "m"(*uaddr));
	return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool put_user(uint8_t *udst, uint8_t byte)
{
	int64_t error_code;
	__asm __volatile("movabsq $done_put, %0\n"
					 "movb %b2, %1\n"
					 "done_put:\n"
					 : "=&a"(error_code), "=m"(*udst)
					 : "q"(byte));
	return error_code != -1;
}