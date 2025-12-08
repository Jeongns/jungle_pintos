/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);
static bool lazy_load_file(struct page *page, void *aux);

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
	struct file_page *aux = page->uninit.aux;
	struct file_page *file_page = &page->file;

	*file_page = (struct file_page){
		.offset = aux->offset,
		.file = aux->file,
		.page_read_bytes = aux->page_read_bytes,
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

/*
mummap을 위해 mmap시 페이지의 index와 mmap 페이지의 길이를 추가한다.
file과 aux객체에 해당 내용을 저장하여 mummap시 index와 mmap페이지 길이를 보고 mummap을 진행한다.

munmap을 위해서 struct page에 다음 페이지를 가리키는 포인터인 next_page를 추가한다.
일반적인 경우에는 next_page가 NULL이지만, mmap으로 할당할 때는 next_page가 다음 페이지를 가리키도록
업데이트한다. munmap시에는, next_page가 NULL일 때까지 순회를 돌면서 해당 munmap 로직을 수행한다.
*/

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset)
{
	file = file_reopen(file);
	int index = 0;
	void *addr_copy = addr;
	size_t read_bytes = length;
	struct page *current_page = NULL;

	while (read_bytes > 0) {
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;

		struct mmap_aux *mmap_aux = malloc(sizeof(*mmap_aux));
		if (!mmap_aux)
			return NULL;

		*mmap_aux = (struct mmap_aux){
			.file = file,
			.offset = offset,
			.page_read_bytes = page_read_bytes,
			.index = index++,						  // mmap 중에 몇번재
			.length = (length + PGSIZE - 1) / PGSIZE, // mmap 총 몇페이지
		};

		if (!vm_alloc_page_with_initializer(VM_FILE, addr_copy, writable, lazy_load_file,
											mmap_aux)) {
			free(mmap_aux);
			mmap_aux = NULL;
			goto error;
		}

		addr_copy += PGSIZE;
		offset += page_read_bytes;
		read_bytes -= page_read_bytes;
	}
	return addr;

error:
	for (size_t i = 0; i < length; i += PGSIZE) {
		struct page *rollback_page = spt_find_page(&thread_current()->spt, addr + i);
		if (rollback_page != NULL)
			destroy(rollback_page);
	}
	return NULL;
}

static bool lazy_load_file(struct page *page, void *aux)
{
	struct mmap_aux *mmap_aux = (struct mmap_aux *)aux;
	struct file *file = mmap_aux->file;
	off_t ofs = mmap_aux->offset;
	size_t page_read_bytes = mmap_aux->page_read_bytes;

	lock_acquire(&file_lock);
	int read_result = file_read_at(file, page->frame->kva, page_read_bytes, ofs);
	lock_release(&file_lock);

	page->file.page_read_bytes = read_result;
	memset(page->frame->kva + page_read_bytes, 0, PGSIZE - page_read_bytes);
	free(aux);

	return true;
}

/* Do the munmap */
void do_munmap(void *addr)
{
}
