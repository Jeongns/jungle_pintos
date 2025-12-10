/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"
#include "lib/random.h"

struct frame_table *frame_table;
struct lock frame_lock; // 프레임의 참조 카운트를 수정할 때 받아야 하는 락

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
	frame_table = malloc(sizeof(struct frame_table));

	frame_table_init(frame_table);
	lock_init(&frame_lock);
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
static bool vm_claim_page_with_frame(struct page *page, struct frame *frame);

/* frame table helpers - 여기부터 frame 관련 함수 */
static uint64_t ft_hash_func(const struct hash_elem *elem, void *aux UNUSED);
static bool ft_hash_less_func(const struct hash_elem *elem_a, const struct hash_elem *elem_b,
							  void *aux UNUSED);
static void remove_frame_from_ft(struct hash_elem *elem, void *aux UNUSED);

// frame table functions
bool ft_insert_frame(struct frame_table *ft, struct frame *frame);

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
	struct page *page = calloc(1, sizeof(struct page));
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
	page->original_writable = writable;
	page->is_cow = false;
	page->owner_thread = thread_current();

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

	if (VM_TYPE(page->operations->type) == VM_FILE)
		swap_out(page);

	struct hash_elem *e = hash_delete(&spt->spt_hash, &page->spt_hash_elem);
	if (e == NULL) {
		page->frame = NULL;
		return;
	}

	vm_dealloc_page(page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void)
{
	/* TODO: The policy for eviction is up to you. */
	/* 일단은 그냥 랜덤으로 아무거나 잡히는 거 삭제하는 걸로 결정 */
	struct hash_iterator h;

	/* 1. 프레임 테이블의 처음부터 탐색 시작 */
	lock_acquire(&frame_lock);
	hash_first(&h, &frame_table->ft_hash);

	/* 2. 랜덤한 숫자 값만큼 해시 요소를 건너뜀 */
	int steps = (random_ulong() % 31) + 1;

	for (int i = 0; i < steps; i++) {
		if (!hash_next(&h)) {
			/* 끝에 도달하면 다시 처음으로 */
			hash_first(&h, &frame_table->ft_hash);
			hash_next(&h);
		}
	}

	/* 3. 랜덤한 iterator를 토대로 elem을 찾음 */
	struct hash_elem *victim_elem = hash_cur(&h);

	/* 4. 혹시 맨 끝이면, 맨 처음으로 설정 */
	if (victim_elem == NULL) {
		hash_first(&h, &frame_table->ft_hash);
		hash_next(&h);
		victim_elem = hash_cur(&h);
	}

	struct frame *victim = hash_entry(victim_elem, struct frame, ft_hash_elem);
	lock_release(&frame_lock);

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct frame *victim = vm_get_victim();
	if (victim == NULL)
		return NULL;

	/* 기존 page swap-out 이후 spt에서 삭제 & free */
	lock_acquire(&frame_lock);
	// struct page *page = list_begin(&victim->page_list);
	// if (page->operations->type != VM_UNINIT)
	// 	swap_out(page);

	/* frame의 page_list를 순회하면서, 현재 frame을 참조하고 있는 모든 page를 swap_out 한다. */
	struct list_elem *e;
	for (e = list_begin(&victim->page_list); e != list_end(&victim->page_list);) {
		struct page *page = list_entry(e, struct page, frame_list_elem);
		e = list_remove(e);

		if (page->operations->type != VM_UNINIT)
			swap_out(page);

		page->frame = NULL;
		pml4_clear_page(page->owner_thread->pml4, page->va);
	}
	lock_release(&frame_lock);

	return victim;
}

/* palloc()으로 프레임을 획득한다. 사용가능한 페이지가 없으면 페이지를 제거한다.
 * 이 함수는 항상 유효한 주소를 반환한다. 즉, 유저풀 메모리가 가득 차있으면
 * 메모리 공간을 확보하기 위해 프레임을 제거한다. */
static struct frame *vm_get_frame(void)
{
	struct frame *frame = NULL;

	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kva == NULL) {
		frame = vm_evict_frame(); // 기존 프레임(껍데기)을 가져옴

		if (frame == NULL)
			PANIC("희생자 페이지 찾기 실패");

		lock_acquire(&frame_lock);
		list_init(&frame->page_list);
		lock_release(&frame_lock);

		return frame;
	}

	frame = malloc(sizeof(struct frame));
	if (frame == NULL) {
		// palloc_free_page(kva);
		PANIC("(vm_get_frame) malloc 실패\n");
	}

	/* 초기화 */
	lock_acquire(&frame_lock);
	frame->kva = kva;
	list_init(&frame->page_list);

	/* 장부에 새로 등록 */
	if (!ft_insert_frame(frame_table, frame)) {
		lock_release(&frame_lock);
		PANIC("ft_insert_frame 실패\n");
	}

	lock_release(&frame_lock);
	return frame;
}

/* Growing the stack. */
static bool vm_stack_growth(void *addr)
{
	addr = pg_round_down(addr);
	if (!vm_alloc_page(VM_ANON | VM_STACK_MARKER, addr, true))
		return false;

	if (!vm_claim_page(addr))
		return false;

	return true;
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page)
{
	if (!page)
		return false;

	// cow가 아니라, 그냥 권한 위반인 경우
	if (!page->is_cow) {
		thread_exit();
	}

	lock_acquire(&frame_lock);
	if (list_size(&page->frame->page_list) == 1) {
		page->writable = page->original_writable;
		lock_release(&frame_lock);

		pml4_clear_page(thread_current()->pml4, page->va);
		if (!pml4_set_page(thread_current()->pml4, page->va, page->frame->kva, page->writable))
			return false;

		return true;
	}
	lock_release(&frame_lock);

	// vm_get_frame에서 frame_lock을 획득해서, 일단 해제해줘야 함
	struct frame *new_frame = vm_get_frame();

	lock_acquire(&frame_lock);
	struct frame *old_frame = page->frame;

	// old_frame에서 new_frame으로 데이터 복사
	memcpy(new_frame->kva, old_frame->kva, PGSIZE);

	// 기존 frame_list에서 page 삭제
	list_remove(&page->frame_list_elem);

	// 새로운 frame 등록
	page->frame = new_frame;
	list_push_back(&new_frame->page_list, &page->frame_list_elem);
	lock_release(&frame_lock);

	page->writable = page->original_writable;
	pml4_clear_page(thread_current()->pml4, page->va);
	if (!pml4_set_page(thread_current()->pml4, page->va, page->frame->kva, page->writable))
		return false;

	return true;
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

		// R/O인 페이지에 쓰기 작업을 실시하는 경우 -> 유저 프로그램 exit
		if (write && !page->writable)
			return vm_handle_wp(page);

		// 페이지가 물리 메모리에 없는 경우 -> 프레임 할당 및 로드
		if (not_present)
			return vm_do_claim_page(page);

		// 다른 종류의 fault (이론상 발생하지 않아야 함)
		return false;
	}

	// Case 2: spt에 페이지가 없는 경우 -> stack growth 확인
	if (page == NULL && not_present) {
		void *rsp = user ? f->rsp : thread_current()->user_rsp;

		// stack growth가 아닌 경우 -> segmentation fault
		if (USER_STACK - (1 << 20) > addr || addr >= USER_STACK || addr < rsp - 8)
			thread_exit();

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

	lock_acquire(&frame_lock);
	// 2. 페이지와 프레임을 서로 연결한다
	list_push_back(&frame->page_list, &page->frame_list_elem);
	page->frame = frame;
	lock_release(&frame_lock);

	// 4. pte 생성
	bool success = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (!success)
		return false;

	// 5. 페이지 초기화 (uninit_initialize)
	return swap_in(page, frame->kva);
}

// 물리 프레임과 페이지를 연결하는 함수, cow에서 사용한다.
static bool vm_claim_page_with_frame(struct page *page, struct frame *frame)
{
	// 프레임과 페이지 연결
	lock_acquire(&frame_lock);
	// printf("list_push elem: %p, list: %p \n", &page->frame_list_elem, &frame->page_list);
	list_push_back(&frame->page_list, &page->frame_list_elem);
	page->frame = frame;
	lock_release(&frame_lock);

	// 어차피 해당 함수는 cow 경우에만 사용할 거라고 생각했음.
	page->writable = false;
	page->is_cow = true;

	bool success = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (!success)
		return false;

	return swap_in(page, frame->kva);
}

// spt helpers
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
								  struct supplemental_page_table *src, struct file *executable_file)
{
	// 0. 널포인터 체크
	if (dst == NULL || src == NULL)
		return false;

	// 1. dst를 비운다
	hash_clear(&dst->spt_hash, remove_page_from_spt);
	src->spt_hash.aux = executable_file;

	// 2. 순회를 하며 copy_page_from_spt 호출
	hash_apply(&src->spt_hash, copy_page_from_spt);
	src->spt_hash.aux = NULL;

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

	if (VM_TYPE(curr_page->operations->type) == VM_FILE)
		swap_out(curr_page);

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
	struct file *executable_file = (struct file *)aux;

	switch (VM_TYPE(src_page->operations->type)) {
		case VM_UNINIT:
			enum vm_type type = page_get_type(src_page);
			size_t aux_size = sizeof(struct file_page);

			struct file_page *dst_aux = malloc(aux_size);
			struct file_page *src_aux = src_page->uninit.aux;

			*dst_aux = (struct file_page){
				.file = executable_file,
				.offset = src_aux->offset,
				.page_read_bytes = src_aux->page_read_bytes,
			};

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

	memset(&dst_page->frame_list_elem, 0, sizeof(struct list_elem));

	// 프레임 즉시 할당
	if (!vm_claim_page_with_frame(dst_page, src_page->frame))
		return;

	// 물리 메모리 복사 -> cow면 필요 없음
	// memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
}

// 프레임 테이블을 초기화하는 함수
void frame_table_init(struct frame_table *frame_table)
{
	if (frame_table == NULL)
		PANIC("(frame_table_init) spt NULL!");

	if (!hash_init(&frame_table->ft_hash, ft_hash_func, ft_hash_less_func, NULL))
		PANIC("(frame_table_init) hash init FAIL!");
}

/* Free the resource hold by the supplemental page table */
void frame_table_kill(struct frame_table *frame_table)
{
	if (frame_table == NULL)
		PANIC("(supplemental_page_table_kill) spt null poiter!");

	lock_acquire(&frame_lock);
	hash_destroy(&frame_table->ft_hash, remove_frame_from_ft);
	lock_release(&frame_lock);
}

static uint64_t ft_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
	struct frame *curr_frame = hash_entry(elem, struct frame, ft_hash_elem);
	return hash_bytes(&curr_frame->kva, sizeof(curr_frame->kva));
}

static bool ft_hash_less_func(const struct hash_elem *elem_a, const struct hash_elem *elem_b,
							  void *aux UNUSED)
{
	struct frame *frame_a = hash_entry(elem_a, struct frame, ft_hash_elem);
	struct frame *frame_b = hash_entry(elem_b, struct frame, ft_hash_elem);

	return frame_a->kva < frame_b->kva;
}

static void remove_frame_from_ft(struct hash_elem *elem, void *aux UNUSED)
{
	struct frame *curr_frame = hash_entry(elem, struct frame, ft_hash_elem);

	printf("TODO: 모르겠음\n");
}

bool ft_insert_frame(struct frame_table *ft, struct frame *frame)
{
	if (ft == NULL || frame == NULL)
		return false;
	return hash_insert(&ft->ft_hash, &frame->ft_hash_elem) == NULL;
}
