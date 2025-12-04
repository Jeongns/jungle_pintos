#include <stdbool.h>
#include <stddef.h>

bool valid_address(const void *uaddr, bool write);
bool valid_address_new(const void *uaddr, size_t size, bool write);
