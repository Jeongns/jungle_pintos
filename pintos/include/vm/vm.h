#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "lib/kernel/hash.h"

enum vm_type {
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1,
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type)&7)

struct supplemental_page_table {
	// 해시테이블 사용
	// key: 가상주소, value: struct page 포인터
	struct hash pages; //해시맵 선언! 키-값이 분리되어 있지 않고, 구조체 전체를 넣고 그 중 일부를
					   //키로 사용한다
	// C언어의 해시맵은 대상 구조체에 키, 값, 연결고리가 있어야 한다.
	// 키를 설정하는 건 해시테이블 초기화할 때 hash_init을 통해 설정한다.
};

/* page 구조체는 “부모 클래스” 같은 역할을 한다.
 * 객체 지향 상속을 union + 함수 포인터로 흉내낸 설계이다.
 * 네 가지 자식 클래스 —uninit_page, file_page, anon_page, page cache(프로젝트4)—가 존재
 * 타임별 추가 데이터만 Union안에 넣어서 사용하라 */
struct page {
	const struct page_operations *operations;
	void *va;			 // 프로세스의 가상주소
	struct frame *frame; // 물리 프레임

	/* Your implementation */
	// hash_elem 추가 필요. 해시테이블이 값들을 연결할 때 필요함

	/* 한 union 안에 여러 타입의 데이터를 함께 담아두었고,
	 * 함수들이 런타임에 어떤 타입이 들어 있는지 스스로 판별해서 처리한다*/
	union {
		struct uninit_page uninit;
		struct anon_page anon; // swap_slot
		struct file_page file; // 파일 정보
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
	bool (*swap_in)(struct page *, void *);
	bool (*swap_out)(struct page *);
	void (*destroy)(struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in((page), v)
#define swap_out(page) (page)->operations->swap_out(page)
#define destroy(page)                                                                              \
	if ((page)->operations->destroy)                                                               \
	(page)->operations->destroy(page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
};

#include "threads/thread.h"
void supplemental_page_table_init(struct supplemental_page_table *spt);
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src);
void supplemental_page_table_kill(struct supplemental_page_table *spt);
struct page *spt_find_page(struct supplemental_page_table *spt, void *va);	  // FIXME:
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page); // FIXME:
void spt_remove_page(struct supplemental_page_table *spt, struct page *page); // FIXME:

void vm_init(void);
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present);

#define vm_alloc_page(type, upage, writable)                                                       \
	vm_alloc_page_with_initializer((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux);
void vm_dealloc_page(struct page *page);
bool vm_claim_page(void *va);
enum vm_type page_get_type(struct page *page);

#endif /* VM_VM_H */
