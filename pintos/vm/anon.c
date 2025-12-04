/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/mmu.h" // pml4_clear_page

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

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
	/* TODO: Set up the swap_disk. */
	swap_disk = NULL;
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_slot = 0; // 1204 아직 swap out되지 않은 상태

	return true;
}

static bool anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;

	if (page == NULL || kva == NULL)
		return false;

	// swap disk가 초기화되지 않았거나, swap out된 적이 없으면 성공
	if (swap_disk == NULL || anon_page->swap_slot == 0)
		return true;

	// swap disk에서 읽어온다
	// PGSIZE(4096) = DISK_SECTOR_SIZE(512) * 8
	// 8개 섹터 읽어서 페이지 복원
	disk_sector_t sector = anon_page->swap_slot;
	for (int i = 0; i < 8; i++) {
		// swap_disk에 저장된 페이지를 kva로 다시 읽어온다
		disk_read(swap_disk, sector + i, kva + (DISK_SECTOR_SIZE * i));
	}

	// swap slot을 해제
	anon_page->swap_slot = -1;

	return true;
}

static bool anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	if (page == NULL || page->frame == NULL)
		return false;

	// swap disk가 초기화되지 않았으면 실패
	if (swap_disk == NULL)
		return false;

	// TODO: 비트맵을 사용하여 빈 swap slot 찾기
	// 현재는 임시로 간단한 방법 사용

	// 임시: swap slot을 간단히 할당 (실제 구현 필요)
	// 여기서는 예시로 섹터 번호를 설정 (실제로는 비트맵으로 관리해야 함)
	static disk_sector_t next_slot = 0;
	disk_sector_t slot = next_slot;
	next_slot += 8; // 다음 페이지는 8섹터 뒤에

	// swap disk에 쓴다
	// PGSIZE(4096) = DISK_SECTOR_SIZE(512) * 8
	// 8개 섹터에 나눠서 쓴다
	void *kva = page->frame->kva;
	for (int i = 0; i < 8; i++) {
		disk_write(swap_disk, slot + i, kva + (DISK_SECTOR_SIZE * i));
	}

	// swap slot 정보 저장
	anon_page->swap_slot = slot;

	// 페이지 테이블에서 present 비트를 0으로 설정
	pml4_clear_page(thread_current()->pml4, page->va);

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
}
