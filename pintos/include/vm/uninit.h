#ifndef VM_UNINIT_H
#define VM_UNINIT_H
#include "vm/vm.h"

struct page;
enum vm_type;

typedef bool vm_initializer(struct page *, void *aux);

/* Uninitlialized page. The type for implementing the
 * "Lazy loading". */
struct uninit_page {
	/* Initiate the contents of the page */
	/* 실제 물리 메모리에 데이터를 로딩하는 함수 */
	vm_initializer *init;
	enum vm_type type; // 이후에 변화하게 될 type
	void *aux;
	/* Initiate the struct page and maps the pa to the va */
	/* struct page를 초기화하고, pa와 va를 매핑하는 함수 */
	bool (*page_initializer)(struct page *, enum vm_type, void *kva);
};

void uninit_new(struct page *page, void *va, vm_initializer *init, enum vm_type type, void *aux,
				bool (*initializer)(struct page *, enum vm_type, void *kva));
#endif
