/* file.c: Implementation of memory backed file object (mmaped object). */

#include <string.h>

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/process.h"
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
	struct mmap_aux *aux = (struct mmap_aux *)page->uninit.aux;
	struct file_page *file_page = &page->file;

	*file_page = (struct file_page){
		.offset = aux->offset,
		.file = aux->file,
		.page_read_bytes = aux->page_read_bytes,
		.mmap_index = aux->mmap_index,
		.mmap_length = aux->mmap_length,
	};

	return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva)
{
	if (page == NULL || kva == NULL)
		return false;

	struct file_page *file_page = &page->file;
	if (file_page->file == NULL)
		return false;

	struct file *file = file_page->file;
	off_t ofs = file_page->offset;
	size_t page_read_bytes = file_page->page_read_bytes;

	lock_acquire(&file_lock);
	int result = file_read_at(file, page->frame->kva, page_read_bytes, ofs);
	lock_release(&file_lock);

	if (result != file_page->page_read_bytes) {
		// 파일 쓰기에 실패했다면 OS가 할 수 있는 일은 없다.
		// 데이터는 유실되더라도 메모리 누수는 막아야 한다.
		printf("File read failed! intended: %d, actual: %d", page_read_bytes, result);
	}

	memset(kva + page_read_bytes, 0, PGSIZE - page_read_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page)
{
	if (page == NULL)
		return false;

	struct file_page *file_page = &page->file;
	bool is_dirty = pml4_is_dirty(thread_current()->pml4, page->va);
	if (is_dirty) {
		struct file *file = file_page->file;
		off_t ofs = file_page->offset;
		size_t page_read_bytes = file_page->page_read_bytes;

		lock_acquire(&file_lock);
		off_t result = file_write_at(file, page->frame->kva, page_read_bytes, ofs);
		lock_release(&file_lock);

		if (result != file_page->page_read_bytes) {
			// 파일 쓰기에 실패했다면 OS가 할 수 있는 일은 없다.
			// 데이터는 유실되더라도 메모리 누수는 막아야 한다.
			printf("File write failed! intended: %d, actual: %d", page_read_bytes, result);
		}
	}

	pml4_set_dirty(thread_current()->pml4, page->va, false);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page)
{
	struct file_page *file_page = &page->file;
	if (page->frame == NULL)
		return;

	file_backed_swap_out(page);

	lock_acquire(&frame_lock);
	list_remove(&page->frame_list_elem);

	if (list_size(&page->frame->page_list) > 0) {
		// 아직 프레임을 참조하는 페이지가 있으면
		page->frame = NULL;

		lock_release(&frame_lock);
		return;
	}

	// 프레임 해제하기
	hash_delete(&frame_table->ft_hash, &page->frame->ft_hash_elem);
	lock_release(&frame_lock);

	// pte에서 매핑 제거
	pml4_clear_page(thread_current()->pml4, page->va);

	// 물리메모리도 제거
	palloc_free_page(page->frame->kva);

	// frame 구조체 해제
	free(page->frame);
	page->frame = NULL;
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
	/*
	1. length보다 크거나 같을 때까지 += PGSIZE 해주면서 페이지 늘려주기
	2. 페이지마다 만들고, vm_alloc_page_with_initializer() 호출해서 파일 & offset 전달하기 (offset
	업데이트, reopen하기)
	3. offset & read_bytes 값 업데이트하기
	4. init_aux로 file_page struct 전달할거고, init 함수는 lazy_load_segment, VM_TYPE은 VM_FILE로,
	upage는 page 시작 주소, writable 같이 전달하기
	*/

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
			.mmap_index = index++,						   // mmap 중에 몇번재
			.mmap_length = (length + PGSIZE - 1) / PGSIZE, // mmap 총 몇페이지
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
	memset(page->frame->kva + read_result, 0, PGSIZE - read_result);
	free(aux);

	return true;
}

/* Do the munmap */
void do_munmap(void *addr)
{
	struct page *mmap_page = spt_find_page(&thread_current()->spt, addr);
	if (mmap_page == NULL || page_get_type(mmap_page) != VM_FILE)
		return;

	struct file *mmap_file = mmap_page->file.file;

	int length;
	if (VM_TYPE(mmap_page->operations->type) == VM_FILE) {
		length = mmap_page->file.mmap_length;
	} else {
		struct mmap_aux *mmap_aux = mmap_page->uninit.aux;
		length = mmap_aux->mmap_length;
	}

	for (size_t i = 0; i < length; i++) {
		struct page *page = spt_find_page(&thread_current()->spt, addr + (PGSIZE * i));
		ASSERT(page != NULL);
		spt_remove_page(&thread_current()->spt, page);
	}

	file_close(mmap_file); // TODO: exit 시 file_close
}
