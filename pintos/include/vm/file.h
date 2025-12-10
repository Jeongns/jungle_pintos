#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file;		  // 매핑된 파일 객체
	uint64_t offset;		  // 파일 객체의 오프셋 값
	uint32_t page_read_bytes; // 페이지에서 읽어야 하는 바이트의 개수
	uint32_t mmap_index;
	uint32_t mmap_length;
};

struct mmap_aux {
	struct file *file;
	uint64_t offset;
	uint32_t page_read_bytes;
	uint32_t mmap_index;
	uint32_t mmap_length;
};

void vm_file_init(void);
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap(void *va);
#endif
