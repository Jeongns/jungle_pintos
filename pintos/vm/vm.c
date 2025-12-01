/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/thread.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vaddr.h"
#include <string.h>

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE(page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

// page 생성 + spt에 등록하는 함수입니다
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	// 1. spt에 이미 등록된 페이지인지 확인
	if (spt_find_page(spt, upage) != NULL)
		return false;

	// 2. 페이지 구조체 메모리 할당
	struct page *page = malloc(sizeof(struct page));
	if (page == NULL)
		return false;

	// 3. type에 맞는 initializer를 설정한다.
	bool (*initializer)(struct page *, enum vm_type, void *kva);
	switch (VM_TYPE(type)) {
		case VM_ANON:
			initializer = anon_initializer;
			break;
		case VM_FILE:
			initializer = file_backed_initializer;
			break;
		default:
			goto err;
	}

	// page구조체에 값 넣기. uninit 페이지 생성.
	uninit_new(page, upage, init, type, aux, initializer);
	page->writable = writable; // FIXME:

	// spt에 uninit상태로 페이지만 등록
	if (!spt_insert_page(spt, page))
		goto err;

	return true; // FIXME:

err:
	free(page); // FIXME:
	return false;
}

// spt에서 va로 페이지를 찾아 반환하는 함수
struct page *spt_find_page(struct supplemental_page_table *spt, void *va)
{
	if (va == NULL)
		return NULL;

	// 1. 페이지 경계로 va를 내린다
	struct page page;
	page.va = pg_round_down(va);

	// 2. 해시 테이블에서 검색한다.
	struct hash_elem *find_elem = hash_find(&spt->spt_hash, &page.spt_hash_elem);

	// 3. 찾았으면 page구조체를 반환한다.
	if (find_elem != NULL)
		return hash_entry(find_elem, struct page, spt_hash_elem);

	return NULL;
}

// spt에 페이지 추가
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page)
{
	if (spt == NULL || page == NULL)
		return false;
	return hash_insert(&spt->spt_hash, page) == NULL;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	if (spt == NULL || page == NULL)
		return false;
	hash_delete(&spt->spt_hash, &page->spt_hash_elem);
	vm_dealloc_page(page);
}

// 교체할 페이지를 선택하는 함수
static struct frame *vm_get_victim(void)
{
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* 한 페이지(page)를 제거(evict)하고 해당 프레임(frame)을 반환합니다.
 * 오류 발생 시 NULL을 반환합니다. */
static struct frame *vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc()으로 프레임을 획득한다. 사용가능한 페이지가 없으면 페이지를 제거한다.
 * 이 함수는 항상 유효한 주소를 반환한다. 즉, 유저풀 메모리가 가득 차있으면
 * 메모리 공간을 확보하기 위해 프레임을 제거하나다. */
static struct frame *vm_get_frame(void)
{
	// 1. 사용자풀에서 물리 페이지를 할당받는다
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO); // 물리메모리 주소 반환

	// 2. 메모리 부족하면 페이지를 교체한다
	if (kva == NULL) {
		PANIC("TODO: swap out 미구현");
		// vm_evict_frame() 구현
	}

	// 3. frame 구조체를 생성한다
	struct frame *frame = malloc(sizeof(struct frame));
	frame->kva = kva;
	frame->page = NULL;

	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED)
{
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	// vm_claim_page 호출

	return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

// 물리 프레임 할당, pte 생성, ANON 또는 FILE로 변환하는 작업을 한다
bool vm_claim_page(void *va)
{
	// 1. spt에서 페이지를 찾아 구조체를 획득한다
	struct page *page = NULL;
	page = spt_find_page(&thread_current()->spt, va);
	// 2. page 없으면 실패
	if (page == NULL)
		return false;

	// 3. 실제 프레임할당
	return vm_do_claim_page(page);
}

// 물레프레임 할당하여 페이지와 프레임을 연결한다
static bool vm_do_claim_page(struct page *page)
{
	// 1. 물리 프레임 할당 (의미있는 데이터는 없는 상태)
	struct frame *frame = vm_get_frame();

	// 2. 페이지와 프레임을 서로 연결
	frame->page = page;
	page->frame = frame;

	// 3. pte 생성
	bool success = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (!success)
		return false;

	// 4. 페이지 초기화 (프레임에 실제 데이터를 넣는다)
	return swap_in(page, frame->kva); // uninit_initialize가 실행됨
}

// spt helpers
static uint64_t spt_hash_func(const struct hash_elem *elem, void *aux UNUSED);
static uint64_t spt_hash_func(const struct hash_elem *elem, void *aux UNUSED);
static bool spt_hash_less_func(const struct hash_elem *elem_a, const struct hash_elem *elem_b,
							   void *aux UNUSED);
static void remove_page_from_spt(struct hash_elem *elem, void *aux UNUSED);
static void copy_page_from_spt(struct hash_elem *elem, void *aux);

// 해시테이블을 초기화하는 함수
void supplemental_page_table_init(struct supplemental_page_table *spt)
{
	if (spt == NULL)
		PANIC("(supplemental_page_table_init) spt NULL!");
	if (!hash_init(&spt->spt_hash, spt_hash_func, spt_hash_less_func, NULL))
		PANIC("(supplemental_page_table_init) hash init FAIL!");
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src)
{
	// 0. 널포인터 체크
	if (dst == NULL || src == NULL)
		return false;

	// 1. dst를 비운다
	hash_clear(&dst->spt_hash, remove_page_from_spt);

	// 2.src 순회 중 dst 참조를 위해 aux에 dst 할당
	src->spt_hash.aux = dst;

	// 3. 순회를 하며 copy_page_from_spt 호출
	hash_apply(&src->spt_hash, copy_page_from_spt);
	src->spt_hash.aux = NULL;

	return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt)
{
	if (spt == NULL)
		PANIC("(supplemental_page_table_kill) spt null poiter!");
	//모든 엔트리 제거 + 해시 테이블 파괴
	hash_destroy(&spt->spt_hash, remove_page_from_spt);
}

// va로 해시키를 만들어서 반환하는 함수
static uint64_t spt_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
	struct page *curr_page = hash_entry(elem, struct page, spt_hash_elem);
	return hash_bytes(&curr_page->va, sizeof(curr_page->va));
}

/* page가 같은지, 혹은 순서가 앞서는지를 va를 기준으로 판단하는 함수
 * a가 b보다 더 크면 true를 반환한다. */
static bool spt_hash_less_func(const struct hash_elem *elem_a, const struct hash_elem *elem_b,
							   void *aux UNUSED)
{
	struct page *page_a = hash_entry(elem_a, struct page, spt_hash_elem);
	struct page *page_b = hash_entry(elem_b, struct page, spt_hash_elem);
	return page_a->va > page_b->va;
}

// spt에서 해당 page를 삭제하고 메모리를 해제하는 콜백함수
// writeback을 위해 VM_FILE은 swap_out함수를 호출합니다.
static void remove_page_from_spt(struct hash_elem *elem, void *aux UNUSED)
{
	struct page *curr_page = hash_entry(elem, struct page, spt_hash_elem);

	if (page_get_type(curr_page) == VM_FILE)
		swap_out(curr_page); // 변경된 내용을 디스크에 다시 기록 (writeback)

	vm_dealloc_page(curr_page);
}

// spt의 해당 page를 다른 spt로 복사합니다
static void copy_page_from_spt(struct hash_elem *elem, void *aux)
{
	struct supplemental_page_table *dst_spt = aux;

	struct page *src_page = hash_entry(elem, struct page, spt_hash_elem);
	struct page *dst_page = malloc(sizeof(struct page));
	memcpy(dst_page, src_page, sizeof(struct page));

	hash_insert(&dst_spt->spt_hash, dst_page);
}