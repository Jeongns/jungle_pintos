/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"
#include <string.h>

// frame table 사용 위해 추가
#include "threads/synch.h"
#include "lib/kernel/list.h"

// 전역 변수
struct list frame_table;	  // 모든 프레임 목록
struct list_elem *clock_hand; // clock 알고리즘 포인터
struct lock frame_table_lock; // 동기화용 락

void vm_init(void)
{
	vm_anon_init();
	vm_file_init();

	// frame table 초기화
	list_init(&frame_table);
	clock_hand = NULL;
	lock_init(&frame_table_lock);

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
static struct frame *vm_evict_frame(struct frame *victim);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
// page 생성 + spt에 등록하는 함수입니다
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	// 1. spt에 이미 등록된 페이지인지 확인
	if (spt_find_page(spt, upage) != NULL)
		return false;

	// 2. struct page
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

	// page 구조체에 값 넣기
	uninit_new(page, upage, init, type, aux, initializer);
	page->writable = writable;

	if (!spt_insert_page(spt, page))
		goto err;

	return true;

err:
	free(page);
	return false;
}

// spt에서 va로 페이지를 찾아 반환하는 함수
struct page *spt_find_page(struct supplemental_page_table *spt, void *va)
{
	if (va == NULL || hash_empty(&spt->spt_hash))
		return NULL;

	// 1. 페이지 경계로 va를 내린다
	struct page dummy_page;
	dummy_page.va = pg_round_down(va);

	// 2. 해시 테이블에서 검색한다.
	struct hash_elem *find_elem = hash_find(&spt->spt_hash, &dummy_page.spt_hash_elem);

	// 3. 찾았으면 page구조체를 반환한다.
	if (find_elem == NULL)
		return NULL;

	return hash_entry(find_elem, struct page, spt_hash_elem);
}

// spt에 페이지 추가
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page)
{
	if (spt == NULL || page == NULL)
		return false;
	return hash_insert(&spt->spt_hash, &page->spt_hash_elem) == NULL;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	if (spt == NULL || page == NULL)
		return false;
	hash_delete(&spt->spt_hash, &page->spt_hash_elem);
	vm_dealloc_page(page);
}

// victim frame과 연결된 page를 swap-out해주고
// pte에서 삭제하고
// 페이지-프레임 연결 해제
static struct frame *vm_evict_frame(struct frame *victim)
{
	struct page *page = victim->page;

	// 페이지 타입에 맞는 swap_out 호출
	bool result = swap_out(page);
	if (!result)
		return false;

	// 페이지 - 프레임 연결 해제
	// TODO: swap_out 에서 이미 수행 되었을수도 있음

	return true;
}

static struct frame *vm_get_victim(void)
{
	ASSERT(lock_held_by_current_thread(&frame_table_lock));

	if (list_empty(&frame_table)) {
		return NULL;
	}

	// clock hand 초기화
	if (clock_hand == NULL || clock_hand == list_end(&frame_table))
		clock_hand = list_begin(&frame_table);

	struct list_elem *start = clock_hand;
	size_t num_frames = list_size(&frame_table);

	// clock 알고리즘 - 첫번째 라운드
	for (size_t i = 0; i < num_frames; i++) {
		struct frame *frame = list_entry(clock_hand, struct frame, elem);

		// 리스트 이동
		clock_hand = list_next(clock_hand);
		if (clock_hand == list_end(&frame_table))
			clock_hand = list_begin(&frame_table);

		// 페이지가 없는 프레임은 스킵한다. 타이밍 이슈로 짧은 순간 null 인 경우로 본다.
		// 방금 evict되었거나, 새로 생성된 프레임일 때.
		if (NULL == frame->page)
			continue;

		struct page *page = frame->page;

		// accessed bit 확인
		if (pml4_is_accessed(page->owner->pml4, page->va)) {
			// 기회를 준다. 비트 초기화
			pml4_set_accessed(page->owner->pml4, page->va, false);
		} else {
			// victim 발견
			return frame;
		}
	}

	// 두번째 라운드: 모든 accessed bit 가 0으로 초기화 된 상태
	for (size_t i = 0; i < num_frames; i++) {
		struct frame *frame = list_entry(clock_hand, struct frame, elem);

		clock_hand = list_next(clock_hand);
		if (clock_hand == list_end(&frame_table))
			clock_hand = list_begin(&frame_table);

		if (NULL == frame->page)
			continue;

		struct page *page = frame->page;
		if (!pml4_is_accessed(page->owner->pml4, page->va)) {
			return frame;
		}
	}

	// 없으면 첫번재 프레임 반환
	return list_entry(list_begin(&frame_table), struct frame, elem);
}

/* palloc()으로 프레임을 획득한다. 사용가능한 페이지가 없으면 페이지를 제거한다.
 * 이 함수는 항상 유효한 주소를 반환한다. 즉, 유저풀 메모리가 가득 차있으면
 * 메모리 공간을 확보하기 위해 프레임을 제거한다. */
static struct frame *vm_get_frame(void)
{
	// TODO: lock 필요한 이유?
	lock_acquire(&frame_table_lock);

	// 1. 물리메모리 할당 시도
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);

	//////////////////////////////////////////////
	// 2. 실패하면 eviction
	if (NULL == kva) {
		struct frame *victim = vm_get_victim();
		if (NULL == victim) {
			lock_release(&frame_table_lock);
			PANIC("vm_get_frame: no evictable frame.");
		}
		// 여기서는 아직 frame에 page A가 연결되어 있는 상태
		// page A의 데이터가 아직 물리메모리에 있는 상태임

		if (!vm_evict_frame(victim)) {
			lock_release(&frame_table_lock);
			PANIC("vm_get_frame: Eviction failed.");
		}

		// victim 프레임 사용
		kva = victim->kva; // 재사용
		victim->page = NULL;

		lock_release(&frame_table_lock);
		return victim;
	}

	//////////////////////////////////////////////
	// 3. 물리메모리 있으면 프레임 생성, 등록
	struct frame *frame = malloc(sizeof(struct frame));
	if (NULL == frame) {
		palloc_free_page(kva);
		lock_release(&frame_table_lock);
		PANIC("vm_get_frame: malloc failed.");
	}

	frame->kva = kva;
	frame->page = NULL;
	list_push_back(&frame_table, &frame->elem); // 리스트에 추가

	lock_release(&frame_table_lock);
	return frame;
}

/* Growing the stack. */
static bool vm_stack_growth(void *addr)
{
	addr = pg_round_down(addr);
	if (!vm_alloc_page(VM_ANON | VM_STACK_MAKER, addr, true))
		return false;

	if (!vm_claim_page(addr))
		return false;

	return true;
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;

	// 1. 유효성 검사
	if (spt == NULL || addr < VM_BOTTOM || is_kernel_vaddr(addr))
		return false;

	// 2. spt에 있는지 찾기
	struct page *page = spt_find_page(spt, addr);

	// Case 1: spt에 페이지가 있는 경우 (lazy loading, swap in)
	if (page != NULL) {

		// 페이지가 물리 메모리에 있는 경우 -> write protection fault
		if (write && !page->writable)
			return false; // 쓰기 불가능한 페이지에 쓰기 시도

		// 페이지가 물리 메모리에 없는 경우 -> 프레임 할당 및 로드
		if (not_present)
			return vm_do_claim_page(page);

		// 다른 종류의 fault (이론상 발생하지 않아야 함)
		return false;
	}

	// Case 2: spt에 페이지가 없는 경우 -> stack growth 확인
	if (page == NULL && not_present) {
		void *rsp = user ? f->rsp : thread_current()->user_rsp;

		// stack growth 조건 검사
		if (USER_STACK - (1 << 20) > addr || addr >= USER_STACK || addr < rsp - 8)
			return false;

		return vm_stack_growth(addr);
	}

	// 기타 모든 경우 invalid access
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
	if (va == NULL)
		return false;

	// 1. spt에서 페이지를 찾아서 page 구조체 획득
	struct page *page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;

	// 2. 실제 프레임 할당
	return vm_do_claim_page(page);
}

// 물레프레임 할당하여 페이지와 프레임을 연결한다
static bool vm_do_claim_page(struct page *page)
{
	// 1. 물리 프레임을 할당한다 (프레임에 의미있는 데이터는 없는 상태)
	struct frame *frame = vm_get_frame();

	// 2. 페이지와 프레임을 서로 연결한다
	frame->page = page;
	page->frame = frame;
	page->owner = thread_current();

	// 3. pte 생성
	bool success = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (!success)
		return false;

	// 4. 페이지 초기화 (uninit_initialize)
	return swap_in(page, frame->kva);
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

	// 2. 순회를 하며 copy_page_from_spt 호출
	hash_apply(&src->spt_hash, copy_page_from_spt);

	return true;
}
/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt)
{
	if (spt == NULL)
		PANIC("(supplemental_page_table_kill) spt null poiter!");
	hash_destroy(&spt->spt_hash, remove_page_from_spt);
}

// va로 해시키를 만들어서 반환하는 함수
static uint64_t spt_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
	struct page *curr_page = hash_entry(elem, struct page, spt_hash_elem);
	return hash_bytes(&curr_page->va, sizeof(curr_page->va));
}

/* page가 같은지, 혹은 순서가 앞서는지를 va를 기준으로 판단하는 함수
 * a가 b보다 작으면 true를 반환한다. */
static bool spt_hash_less_func(const struct hash_elem *elem_a, const struct hash_elem *elem_b,
							   void *aux UNUSED)
{
	struct page *page_a = hash_entry(elem_a, struct page, spt_hash_elem);
	struct page *page_b = hash_entry(elem_b, struct page, spt_hash_elem);
	return page_a->va < page_b->va;
}

// spt에서 해당 page를 삭제합니다
// writeback을 위해 VM_FILE은 swap_out함수를 호출합니다.
static void remove_page_from_spt(struct hash_elem *elem, void *aux UNUSED)
{
	struct page *curr_page = hash_entry(elem, struct page, spt_hash_elem);

	// if (page_get_type(curr_page) == VM_FILE) // NOTE
	// 	swap_out(curr_page);

	vm_dealloc_page(curr_page);
}

// fork시 부모 프로세스의 spt에서 자식 프로세스의 spt로 한 개의 페이지를 복사한다
// @param elem: 부모 SPT의 한 페이지를 가리키는 hash element
static void copy_page_from_spt(struct hash_elem *elem, void *aux)
{
	// 1. 부모 페이지 가져온다
	struct page *src_page = hash_entry(elem, struct page, spt_hash_elem);
	void *va = src_page->va;
	bool writable = src_page->writable;

	switch (VM_TYPE(src_page->operations->type)) {
		case VM_UNINIT:
			// AUX 값 복사하기
			enum vm_type type = page_get_type(src_page);
			size_t aux_size = sizeof(struct vm_load_aux);
			void *dst_aux = malloc(aux_size);
			memcpy(dst_aux, src_page->uninit.aux, aux_size);

			vm_alloc_page_with_initializer(type, va, writable, src_page->uninit.init, dst_aux);
			return;
		case VM_FILE:
			vm_alloc_page_with_initializer(VM_FILE, va, writable, NULL, &src_page->file);
			break;
		case VM_ANON:
			vm_alloc_page_with_initializer(VM_ANON, va, writable, NULL, &src_page->anon);
			break;
	}

	// 자식 페이지 찾기
	struct page *dst_page = spt_find_page(&thread_current()->spt, src_page->va);

	if (dst_page == NULL)
		PANIC("copy_page_from_spt: dst_page not found.");

	// 프레임 즉시 할당
	if (!vm_do_claim_page(dst_page))
		return;

	// 물리 메모리 복사
	memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
}