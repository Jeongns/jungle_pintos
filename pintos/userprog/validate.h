#include <stdbool.h>
#include <stddef.h>

bool copy_user_buffer(char *kernel_dst, const char *user_src, size_t max_len);
bool copy_user_string(char *kernel_dst, const char *user_src, size_t max_len);
bool buffer_copy_to_user(char *user_dst, const char *kernel_src, size_t max_len);