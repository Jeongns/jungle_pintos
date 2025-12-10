/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"

// 추가
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "lib/kernel/bitmap.h"
#include <string.h>

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_bitmap; // 슬롯 사용 여부 추적
static struct lock swap_lock;	   // 동기화용

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE) //페이지당 섹터 수(8개)

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

void vm_anon_init(void)
{
	swap_disk = disk_get(1, 1);
	if (NULL == swap_disk)
		PANIC("vm_anon_init: cannot get swap disk");

	// swap disk 크기 계산
	disk_sector_t swap_size = disk_size(swap_disk);
	size_t num_pages = swap_size / SECTORS_PER_PAGE; // 페이지 몇개까지 저장 가능?

	// bitmap 자료구조 생성
	// swap_bitmap: 각 비트는 스왑 디스크의 slot과 1:1로 대응됨
	swap_bitmap = bitmap_create(num_pages);
	if (NULL == swap_bitmap)
		PANIC("vm_anon_init: cannot create swap bitmap");

	// lock 초기화
	lock_init(&swap_lock);
}

bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_index = -1; // 초기값. swap에 없다는 뜻.

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;

	// swap disk에 저장된 적 있는가
	if (anon_page->swap_index == (disk_sector_t)-1)
		return false;

	lock_acquire(&swap_lock);

	// swap disk에서 데이터 읽기
	disk_sector_t start_sector = anon_page->swap_index;
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read(swap_disk, start_sector + i, kva + (i * DISK_SECTOR_SIZE));
	}

	// bitmap에서 슬롯 해제
	size_t slot_idx = start_sector / SECTORS_PER_PAGE;
	bitmap_set(swap_bitmap, slot_idx, false);

	lock_release(&swap_lock);

	// swap index 초기화 (== 더 이상 swap disk에 없다)
	anon_page->swap_index = -1;

	return true;
}

static bool anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	lock_acquire(&swap_lock);

	// bitmap에서 빈 슬롯 찾기
	size_t slot_idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
	if (slot_idx == BITMAP_ERROR) {
		lock_release(&swap_lock);
		PANIC("anon_swap_out: swap disk is full");
	}

	// 슬롯 번호를 섹터 번호로 변환 TODO: 모르겠음
	disk_sector_t start_sector = slot_idx * SECTORS_PER_PAGE;

	// 데이터를 swap disk에 쓴다
	void *kva = page->frame->kva;
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_write(swap_disk, start_sector + i, kva + (i * DISK_SECTOR_SIZE));
	}
	lock_release(&swap_lock);

	// swap index 저장
	anon_page->swap_index = start_sector;

	// pte 매핑 제거
	pml4_clear_page(page->owner->pml4, page->va);

	// 프레임-페이지 연결 해제
	page->frame->page = NULL;
	page->frame = NULL;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	// swap disk에 저장된 데이터가 있으면 해제한다
	if (anon_page->swap_index != (disk_sector_t)-1) {
		lock_acquire(&swap_lock);

		size_t slot_idx = anon_page->swap_index / SECTORS_PER_PAGE;
		bitmap_set(swap_bitmap, slot_idx, false);

		lock_release(&swap_lock);

		anon_page->swap_index = -1;
	}

	// 프레임 할당되어 있으면 해제한다
	if (page->frame != NULL) {
		// pte에서 매핑 제거
		pml4_clear_page(thread_current()->pml4, page->va);

		// 물리메모리도 제거
		palloc_free_page(page->frame->kva);

		// Frame table에서 제거
		list_remove(&page->frame->elem);

		// frame 구조체 해제
		free(page->frame);
		page->frame = NULL;
	}
}
