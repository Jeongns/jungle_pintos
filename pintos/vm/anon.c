/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "kernel/bitmap.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

static struct bitmap *swap_table;

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	if (swap_disk == NULL)
		PANIC("hd1:1 (swap_disk) not present, file system initialization failed");

	/* swap_disk의 사이즈를 8로 나눠서 전체 슬롯의 개수를 구함 */
	size_t swap_slot_cnt = disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE);
	swap_table = bitmap_create(swap_slot_cnt);
	bitmap_set_all(swap_table, false);
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &anon_ops;
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_table_index = BITMAP_ERROR;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;
	size_t bitmap_index = anon_page->swap_table_index;
	if (bitmap_index == BITMAP_ERROR)
		PANIC("anon swap 구현 중 오류: 스왑 디스크에 있는 페이지가 잘못된 인덱스를 가짐 ");

	disk_sector_t start_disk_sec = bitmap_index * 8;
	for (int i = 0; i < 8; i++) {
		disk_read(swap_disk, (start_disk_sec + i), page->frame->kva + (DISK_SECTOR_SIZE * i));
	}

	bitmap_set(swap_table, bitmap_index, false);
	anon_page->swap_table_index = BITMAP_ERROR;
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page)
{
	if (page == NULL)
		return false;

	struct anon_page *anon_page = &page->anon;

	/* 스왑 디스크 공간이 꽉 차있는 경우 */
	if (bitmap_all(swap_table, 0, bitmap_size(swap_table)) == true)
		return false;

	size_t bitmap_index = bitmap_scan_and_flip(swap_table, 0, 1, false);
	if (bitmap_index == BITMAP_ERROR)
		PANIC("진짜 큰일남 bitmap 이상함\n");

	disk_sector_t start_disk_sec = bitmap_index * 8;

	for (int i = 0; i < 8; i++) {
		disk_write(swap_disk, (start_disk_sec + i), page->frame->kva + (DISK_SECTOR_SIZE * i));
	}

	anon_page->swap_table_index = bitmap_index;
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	if (anon_page->swap_table_index != BITMAP_ERROR) {
		size_t bitmap_index = anon_page->swap_table_index;
		bitmap_set(swap_table, bitmap_index, false);
	}

	if (page->frame == NULL)
		return;

	lock_acquire(&frame_lock);
	list_remove(&page->frame_list_elem);

	if (list_size(&page->frame->page_list) > 0) {
		// 아직 프레임을 참조하는 페이지가 존재하는 경우
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
