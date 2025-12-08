/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	if (page == NULL || kva == NULL || type != VM_FILE)
		return false;

	// 1. VM_FILE에 맞게 operations 변경
	page->operations = &file_ops;

	// 2. file_page 구조체 초기화
	struct mmap_aux *aux = page->uninit.aux;
	struct file_page *file_page = &page->file;

	*file_page = (struct file_page){
		.file = aux->file,
		.offset = aux->offset,
		.index = aux->index,
		.length = aux->length,
	};

	return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;

	if (file_page->file)
		file_close(file_page->file);

	if (page->frame != NULL) {
		// pte에서 매핑 제거
		pml4_clear_page(thread_current()->pml4, page->va);

		// 물리메모리도 제거
		palloc_free_page(page->frame->kva);

		// frame 구조체 해제
		free(page->frame);
		page->frame = NULL;
	}
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset)
{
}

/* Do the munmap */
void do_munmap(void *addr)
{
}

bool lazy_load_mmap(struct page *page, void *aux)
{
	struct vm_load_aux *vm_load_aux = (struct vm_load_aux *)aux;
	struct file *file = vm_load_aux->file;
	off_t ofs = vm_load_aux->offset;

	int read_result = file_read_at(file, page->frame->kva, PGSIZE, ofs);
	memset(page->frame->kva + read_result, 0, PGSIZE - read_result);
	page->file.page_read_bytes = read_result;

	free(aux);
	return true;
}
