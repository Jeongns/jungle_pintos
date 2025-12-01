#ifndef VM_UNINIT_H
#define VM_UNINIT_H
#include "vm/vm.h"

struct page;
enum vm_type;

typedef bool vm_initializer(struct page *, void *aux);

/* Uninitlialized page. The type for implementing the
 * "Lazy loading". */
struct uninit_page {
	/* Initiate the contets of the page */
	vm_initializer *init; // 초기화 콜백 함수(?)
	enum vm_type type;	  // 실제 페이지 타입
	void *aux;			  // 추가 정보
	/* Initiate the struct page and maps the pa to the va */
	bool (*page_initializer)(struct page *, enum vm_type, void *kva); // 타입 변환 함수
};

void uninit_new(struct page *page, void *va, vm_initializer *init, enum vm_type type, void *aux,
				bool (*initializer)(struct page *, enum vm_type, void *kva));
#endif
