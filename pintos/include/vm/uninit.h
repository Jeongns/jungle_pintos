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
	vm_initializer *init;
	enum vm_type type;
	void *aux;
	/* Initiate the struct page and maps the pa to the va */
	bool (*page_initializer)(struct page *, enum vm_type, void *kva);
};

struct vm_load_aux {
	uint64_t offset;		  // 파일 객체의 오프셋 값
	uint32_t page_read_bytes; // 페이지에서 읽어야 하는 바이트의 개수
};

void uninit_new(struct page *page, void *va, vm_initializer *init, enum vm_type type, void *aux,
				bool (*initializer)(struct page *, enum vm_type, void *kva));
#endif
