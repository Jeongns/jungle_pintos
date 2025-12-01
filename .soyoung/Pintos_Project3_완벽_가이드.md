# Pintos Project 3: Virtual Memory 완벽 가이드

> 이 문서는 KAIST PintOS Project 3의 공식 문서를 기반으로, 실제 구현에 필요한 핵심 개념과 흐름을 통합하여 정리한 가이드입니다.

---

## 📚 목차

1. [Project 3가 해결하는 문제](#1-project-3가-해결하는-문제)
2. [핵심 개념 이해하기](#2-핵심-개념-이해하기)
3. [메모리 구조 전체 그림](#3-메모리-구조-전체-그림)
4. [구현해야 할 자료구조](#4-구현해야-할-자료구조)
5. [구현 순서와 상세 가이드](#5-구현-순서와-상세-가이드)
6. [테스트 전략](#6-테스트-전략)

---

## 1. Project 3가 해결하는 문제

### 🎯 목표
Project 2까지는 프로그램을 실행할 때 **실행 파일 전체를 메모리에 즉시 로드**했습니다. 이는 다음과 같은 한계가 있었습니다:

- 메모리보다 큰 프로그램은 실행 불가
- 여러 프로그램 동시 실행 시 메모리 부족
- 사용하지 않는 코드도 메모리 차지

**Project 3에서는 "무한한 메모리"라는 환상을 만들어냅니다:**
- **Lazy Loading**: 필요할 때만 메모리에 로드
- **Swap**: 메모리가 부족하면 디스크에 임시 저장
- **Memory Mapped Files**: 파일을 메모리처럼 사용

---

## 2. 핵심 개념 이해하기

### 2.1 두 개의 테이블: PML4 vs SPT

#### PML4 (Hardware Page Table)
```
역할: CPU의 MMU가 사용하는 하드웨어 테이블
구조: 4-level tree (희소 구조)
내용: 가상주소 → 물리주소 매핑 (Present bit = 1인 것만)
특징: 사용 중인 페이지만 entry 존재
```

#### SPT (Supplemental Page Table)
```
역할: OS가 관리하는 소프트웨어 테이블
구조: Hash table (key: 가상주소)
내용: 페이지 메타데이터 (type, file info, swap slot 등)
특징: 아직 로드 안 된 페이지도 등록 가능 (frame = NULL)
```

**핵심 차이점**:
| | PML4 | SPT |
|---|---|---|
| 관리 주체 | CPU (Hardware) | OS (Software) |
| 저장 내용 | VA → PA 매핑 | 페이지 메타데이터 |
| entry 조건 | Present = 1 (매핑 완료) | frame = NULL 가능 (미로드) |
| 사용 시점 | 모든 메모리 접근 | Page Fault 발생 시 |

### 2.2 Lazy Loading이란?

**Project 2 방식 (Eager Loading)**:
```c
load_segment() {
    for (각 페이지) {
        물리 메모리 할당 (palloc_get_page)
        파일 읽기 (file_read)
        PML4 매핑 (install_page)
    }
}
```

**Project 3 방식 (Lazy Loading)**:
```c
load_segment() {
    for (각 페이지) {
        SPT에 등록만 (vm_alloc_page_with_initializer)
        // 물리 메모리 할당 X
        // 파일 읽기 X
        // PML4 매핑 X
    }
}

// 나중에 실제 접근 시
page_fault_handler() {
    SPT에서 페이지 정보 찾기
    물리 메모리 할당
    파일 읽기
    PML4 매핑
}
```

**장점**:
- 메모리 절약 (사용하지 않는 코드는 로드 안 함)
- 프로그램 시작 속도 향상
- 더 많은 프로그램 동시 실행 가능

---

## 3. 메모리 구조 전체 그림

### 3.1 가상 메모리 레이아웃

```
User Virtual Memory (0x400000 ~ 0x47480000)
┌──────────────────────────────────────┐
│  Code Segment         (0x400000~)    │ ← Executable 코드
├──────────────────────────────────────┤
│  Initialized Data                    │ ← 전역변수 (초기값 O)
├──────────────────────────────────────┤
│  Uninitialized Data (BSS)            │ ← 전역변수 (초기값 X)
├──────────────────────────────────────┤
│  Heap (구현 안 함)                   │
├──────────────────────────────────────┤
│          ↓ (grows down)              │
│  Stack                               │ ← USER_STACK (0x47480000)
└──────────────────────────────────────┘

Kernel Virtual Memory (0x8004000000~)
┌──────────────────────────────────────┐
│  Kernel Code, Data, BSS              │
├──────────────────────────────────────┤
│  Kernel Pool (palloc)                │
├──────────────────────────────────────┤
│  User Pool (palloc)                  │
└──────────────────────────────────────┘
```

### 3.2 페이지와 프레임

**Virtual Address (64-bit)**:
```
 63    48 47   39 38   30 29   21 20   12 11    0
+--------+-------+-------+-------+-------+-------+
|Sign Ext| PML4  |  PDP  |  PD   |  PT   | Offset|
+--------+-------+-------+-------+-------+-------+
   16       9       9       9       9       12
```

**Physical Address**:
```
        12 11    0
+----------+-------+
|  Frame # | Offset|
+----------+-------+
```

**핵심**: 
- Page = Virtual Memory의 4KB 단위
- Frame = Physical Memory의 4KB 단위
- Offset은 변환 없이 그대로 사용

---

## 4. 구현해야 할 자료구조

### 4.1 Supplemental Page Table (SPT)

#### 구조체 정의 (include/vm/vm.h)
```c
struct page {
    const struct page_operations *operations;  // 페이지 타입별 동작
    void *va;                                  // 가상주소
    struct frame *frame;                       // 물리 프레임 (NULL 가능)
    
    union {
        struct uninit_page uninit;   // 아직 초기화 안 됨
        struct anon_page anon;       // Anonymous (Stack, Heap)
        struct file_page file;       // File-backed
    };
};
```

#### SPT 설계 선택
**권장: Hash Table**
- Key: 가상주소 (pg_round_down한 값)
- Value: struct page*
- 장점: O(1) 조회, 동적 크기 조절
- 구현: `lib/kernel/hash.h` 사용

```c
struct supplemental_page_table {
    struct hash pages;  // Hash table
};
```

### 4.2 Frame Table

#### 구조체 정의
```c
struct frame {
    void *kva;              // Kernel Virtual Address
    struct page *page;      // 이 프레임을 사용하는 페이지
};
```

#### Frame Table 관리
- **목적**: 어떤 프레임이 사용 중인지 추적
- **Eviction 정책**: 메모리 부족 시 어떤 페이지를 쫓아낼지 결정
- **선택지**: 
  - FIFO (간단)
  - LRU (성능 좋음)
  - Clock Algorithm (권장 - 구현과 성능의 균형)

### 4.3 Swap Table

#### 역할
- Swap disk의 슬롯 관리 (어떤 슬롯이 사용 중인지)
- Page → Swap Slot 매핑 추적

#### 구현 방법
```c
// Bitmap 사용 권장
struct bitmap *swap_table;
// bit 0 = free, bit 1 = in use
```

---

## 5. 구현 순서와 상세 가이드

### Phase 1: SPT 기본 구현 ⭐⭐⭐

**목표**: Page fault handler가 페이지 정보를 찾을 수 있게 하기

#### 1.1 `supplemental_page_table_init()`

```c
void supplemental_page_table_init(struct supplemental_page_table *spt) {
    // TODO: Hash table 초기화
    // hash_init(&spt->pages, page_hash, page_less, NULL);
}
```

**Hash 함수 필요**:
```c
static unsigned page_hash(const struct hash_elem *e, void *aux) {
    const struct page *p = hash_entry(e, struct page, hash_elem);
    return hash_bytes(&p->va, sizeof(p->va));
}

static bool page_less(const struct hash_elem *a, 
                     const struct hash_elem *b, void *aux) {
    const struct page *pa = hash_entry(a, struct page, hash_elem);
    const struct page *pb = hash_entry(b, struct page, hash_elem);
    return pa->va < pb->va;
}
```

#### 1.2 `spt_find_page()`

```c
struct page *spt_find_page(struct supplemental_page_table *spt, void *va) {
    // 중요: va를 페이지 경계로 round down!
    void *page_addr = pg_round_down(va);
    
    // Hash table에서 찾기
    struct page p;
    p.va = page_addr;
    struct hash_elem *e = hash_find(&spt->pages, &p.hash_elem);
    
    return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}
```

**핵심**: 
- `pg_round_down(va)` 필수! (0x8004567 → 0x8004000)
- 없으면 NULL 반환 (정상 케이스)

#### 1.3 `spt_insert_page()`

```c
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page) {
    // 중복 체크
    if (spt_find_page(spt, page->va) != NULL)
        return false;
    
    // Hash table에 삽입
    hash_insert(&spt->pages, &page->hash_elem);
    return true;
}
```

---

### Phase 2: Anonymous Page & Stack ⭐⭐⭐

**목표**: 스택을 lazy하게 할당하고, stack growth 지원

#### 2.1 `vm_alloc_page_with_initializer()` 

```c
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
                                   bool writable, vm_initializer *init, void *aux) {
    struct supplemental_page_table *spt = &thread_current()->spt;
    
    // 1. 이미 등록된 페이지인지 확인
    if (spt_find_page(spt, upage) != NULL)
        return false;
    
    // 2. struct page 할당
    struct page *page = malloc(sizeof(struct page));
    if (page == NULL)
        return false;
    
    // 3. Type에 맞는 initializer 선택
    bool (*page_initializer)(struct page *, enum vm_type, void *);
    switch (VM_TYPE(type)) {
        case VM_ANON:
            page_initializer = anon_initializer;
            break;
        case VM_FILE:
            page_initializer = file_backed_initializer;
            break;
        default:
            goto err;
    }
    
    // 4. Uninit page로 초기화
    uninit_new(page, upage, init, type, aux, page_initializer);
    
    // 5. SPT에 삽입
    if (!spt_insert_page(spt, page))
        goto err;
    
    return true;
    
err:
    free(page);
    return false;
}
```

**흐름 이해**:
1. SPT에 "앞으로 이 주소를 사용할 거야" 등록
2. 실제 메모리는 할당하지 않음 (frame = NULL)
3. Page fault 시 `uninit_initialize()`가 호출됨

#### 2.2 `setup_stack()` 수정

```c
// userprog/process.c
static bool setup_stack(struct intr_frame *if_) {
    void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);
    
    // VM_ANON 타입으로 등록
    if (!vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true))
        return false;
    
    // 첫 스택 페이지는 즉시 할당
    if (!vm_claim_page(stack_bottom))
        return false;
    
    if_->rsp = USER_STACK;
    return true;
}
```

#### 2.3 `vm_claim_page()` & `vm_do_claim_page()`

```c
bool vm_claim_page(void *va) {
    struct page *page = spt_find_page(&thread_current()->spt, va);
    if (page == NULL)
        return false;
    
    return vm_do_claim_page(page);
}

static bool vm_do_claim_page(struct page *page) {
    // 1. Frame 할당
    struct frame *frame = vm_get_frame();
    if (frame == NULL)
        return false;
    
    // 2. Frame ↔ Page 연결
    frame->page = page;
    page->frame = frame;
    
    // 3. PML4에 매핑 추가
    if (!pml4_set_page(thread_current()->pml4, page->va, 
                       frame->kva, true)) {
        return false;
    }
    
    // 4. Swap in (실제 데이터 로드)
    return swap_in(page, frame->kva);
}
```

#### 2.4 `vm_get_frame()`

```c
static struct frame *vm_get_frame(void) {
    // 1. 물리 메모리 할당
    void *kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        PANIC("todo: swap out");  // 나중에 구현
    }
    
    // 2. Frame 구조체 생성
    struct frame *frame = malloc(sizeof(struct frame));
    if (frame == NULL) {
        palloc_free_page(kva);
        return NULL;
    }
    
    frame->kva = kva;
    frame->page = NULL;
    
    return frame;
}
```

---

### Phase 3: Page Fault Handler ⭐⭐⭐

**목표**: Lazy loading된 페이지 접근 시 실제로 메모리에 로드

#### 3.1 `exception.c` 수정

```c
static void page_fault(struct intr_frame *f) {
    bool not_present = (f->error_code & PF_P) == 0;
    bool write = (f->error_code & PF_W) != 0;
    bool user = (f->error_code & PF_U) != 0;
    void *fault_addr = (void *)rcr2();
    
    intr_enable();
    
#ifdef VM
    // VM이 처리할 수 있는 page fault인지 확인
    if (vm_try_handle_fault(f, fault_addr, user, write, not_present))
        return;  // 정상 처리됨
#endif
    
    // 진짜 fault (invalid access)
    printf("Page fault at %p: %s error %s page in %s context.\n",
           fault_addr, not_present ? "not present" : "rights violation",
           write ? "writing" : "reading", user ? "user" : "kernel");
    kill(f);
}
```

#### 3.2 `vm_try_handle_fault()`

```c
bool vm_try_handle_fault(struct intr_frame *f, void *addr,
                        bool user, bool write, bool not_present) {
    struct supplemental_page_table *spt = &thread_current()->spt;
    
    // 1. Kernel 주소는 허용 안 함
    if (!is_user_vaddr(addr))
        return false;
    
    // 2. SPT에서 페이지 찾기
    struct page *page = spt_find_page(spt, addr);
    
    if (page == NULL) {
        // 3. Stack growth 가능성 체크
        void *rsp = user ? f->rsp : thread_current()->rsp_stack;
        if (addr >= rsp - 32 && addr < USER_STACK) {
            // Stack growth!
            vm_stack_growth(pg_round_down(addr));
            return true;
        }
        return false;  // Invalid access
    }
    
    // 4. Write to read-only page 체크
    if (write && !page->writable)
        return false;
    
    // 5. 페이지를 메모리에 로드
    return vm_do_claim_page(page);
}
```

#### 3.3 `vm_stack_growth()`

```c
static void vm_stack_growth(void *addr) {
    // 새 스택 페이지 할당
    if (vm_alloc_page(VM_ANON | VM_MARKER_0, addr, true)) {
        vm_claim_page(addr);
    }
}
```

---

### Phase 4: File-backed Pages (Lazy Loading) ⭐⭐

**목표**: 실행 파일을 lazy하게 로드

#### 4.1 `load_segment()` 수정

```c
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                        uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);
    
    while (read_bytes > 0 || zero_bytes > 0) {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;
        
        // Lazy load를 위한 aux 정보 생성
        struct lazy_load_arg *aux = malloc(sizeof(struct lazy_load_arg));
        aux->file = file;
        aux->ofs = ofs;
        aux->read_bytes = page_read_bytes;
        aux->zero_bytes = page_zero_bytes;
        
        // SPT에 등록만 (실제 로드는 page fault 시)
        if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable,
                                           lazy_load_segment, aux))
            return false;
        
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
        ofs += page_read_bytes;
    }
    return true;
}
```

#### 4.2 `lazy_load_segment()` 구현

```c
static bool lazy_load_segment(struct page *page, void *aux) {
    struct lazy_load_arg *args = (struct lazy_load_arg *)aux;
    
    // 파일에서 읽기
    file_seek(args->file, args->ofs);
    if (file_read(args->file, page->frame->kva, args->read_bytes) 
        != (int)args->read_bytes) {
        return false;
    }
    
    // 나머지는 0으로 채우기
    memset(page->frame->kva + args->read_bytes, 0, args->zero_bytes);
    
    return true;
}
```

---

### Phase 5: Swap In/Out ⭐⭐

**목표**: 메모리 부족 시 디스크로 swap

#### 5.1 Swap Disk 초기화

```c
// vm/anon.c
static struct disk *swap_disk;
static struct bitmap *swap_table;

void vm_anon_init(void) {
    swap_disk = disk_get(1, 1);  // Swap partition
    if (swap_disk == NULL)
        PANIC("Swap disk not found");
    
    size_t swap_size = disk_size(swap_disk) / 8;  // 페이지 개수
    swap_table = bitmap_create(swap_size);
}
```

#### 5.2 `anon_swap_out()`

```c
static bool anon_swap_out(struct page *page) {
    struct anon_page *anon_page = &page->anon;
    
    // 1. 빈 swap slot 찾기
    size_t slot = bitmap_scan_and_flip(swap_table, 0, 1, false);
    if (slot == BITMAP_ERROR)
        return false;
    
    // 2. Disk에 쓰기 (8 sectors = 1 page)
    for (int i = 0; i < 8; i++) {
        disk_write(swap_disk, slot * 8 + i,
                   page->frame->kva + DISK_SECTOR_SIZE * i);
    }
    
    // 3. Swap slot 번호 저장
    anon_page->swap_slot = slot;
    
    // 4. PML4에서 매핑 제거
    pml4_clear_page(thread_current()->pml4, page->va);
    
    // 5. Frame 해제
    page->frame = NULL;
    
    return true;
}
```

#### 5.3 `anon_swap_in()`

```c
static bool anon_swap_in(struct page *page, void *kva) {
    struct anon_page *anon_page = &page->anon;
    
    // 1. Swap disk에서 읽기
    for (int i = 0; i < 8; i++) {
        disk_read(swap_disk, anon_page->swap_slot * 8 + i,
                  kva + DISK_SECTOR_SIZE * i);
    }
    
    // 2. Swap slot 해제
    bitmap_set(swap_table, anon_page->swap_slot, false);
    
    return true;
}
```

---

## 6. 테스트 전략

### 6.1 단계별 테스트

```bash
# Phase 1+2: SPT + Stack
cd pintos-virtual-memory/vm
./select_test.sh -q
# 선택: pt-grow-stack, pt-grow-bad

# Phase 3: Page Fault Handler  
# 선택: pt-big-stk-obj, pt-bad-addr

# Phase 4: Lazy Loading
# 선택: lazy-file, lazy-anon

# Phase 5: Swap
# 선택: swap-anon, swap-file
```

### 6.2 디버깅 팁

#### SPT 관련
```c
// spt_find_page 디버깅
printf("Looking for page: %p (rounded: %p)\n", va, pg_round_down(va));
```

#### Page Fault 디버깅
```c
// vm_try_handle_fault 시작 부분
printf("Page fault: addr=%p, user=%d, write=%d, not_present=%d\n",
       addr, user, write, not_present);
struct page *page = spt_find_page(spt, addr);
printf("Found page: %p (type: %d)\n", page, page ? page->operations->type : -1);
```

#### Swap 디버깅
```c
// Swap out 시
printf("Swapping out page %p to slot %zu\n", page->va, slot);
```

---

## 7. 자주 하는 실수

### ❌ 실수 1: `pg_round_down()` 안 함
```c
// 잘못된 코드
struct page *page = spt_find_page(spt, 0x400567);  // 못 찾음!

// 올바른 코드
struct page *page = spt_find_page(spt, pg_round_down(0x400567));
```

### ❌ 실수 2: PML4 entry가 항상 있다고 생각
```
PML4는 희소 구조!
entry 자체가 없는 것 = 아직 사용 안 한 주소
```

### ❌ 실수 3: Frame 할당 실패 처리 안 함
```c
struct frame *frame = vm_get_frame();
if (frame == NULL) {
    // Eviction 필요!
    // 초기에는 PANIC 처리 후 나중에 구현
}
```

### ❌ 실수 4: User와 Kernel RSP 구분 안 함
```c
// Page fault 시
void *rsp = user ? f->rsp : thread_current()->rsp_stack;
```

---

## 8. 핵심 요약

### 전체 흐름 한 눈에 보기

```
프로그램 로드
   ↓
load_segment() → SPT에 등록만 (PML4 매핑 X, 물리 메모리 X)
   ↓
프로그램 실행
   ↓
CPU가 0x400000 접근 → PML4에 없음 → Page Fault!
   ↓
page_fault_handler()
   ↓
vm_try_handle_fault()
   ├─ SPT에서 페이지 찾기 (spt_find_page)
   ├─ 물리 메모리 할당 (vm_get_frame)
   ├─ 파일/Swap에서 데이터 로드 (swap_in)
   └─ PML4에 매핑 추가 (pml4_set_page)
   ↓
실행 재개 → 성공!
```

### 구현 체크리스트

- [ ] SPT 기본 (init, find, insert)
- [ ] `vm_alloc_page_with_initializer()`
- [ ] `vm_claim_page()` & `vm_do_claim_page()`
- [ ] `vm_get_frame()`
- [ ] `setup_stack()` 수정
- [ ] `page_fault()` 수정
- [ ] `vm_try_handle_fault()`
- [ ] `vm_stack_growth()`
- [ ] `load_segment()` 수정
- [ ] `lazy_load_segment()`
- [ ] Swap disk 초기화
- [ ] `anon_swap_in()` & `anon_swap_out()`
- [ ] Eviction policy (Clock algorithm)

---

## 9. 참고 자료

- KAIST PintOS 공식 문서: https://casys-kaist.github.io/pintos-kaist/
- Hash Table 사용법: `lib/kernel/hash.h` 참고
- Bitmap 사용법: `lib/kernel/bitmap.h` 참고
- PML4 관련 함수: `threads/mmu.c` 참고

---

**🎯 성공의 열쇠**: 
1. 각 단계를 완료할 때마다 테스트하기
2. 디버그 출력으로 흐름 파악하기
3. SPT와 PML4의 역할을 명확히 구분하기
4. Lazy의 의미를 항상 기억하기 (필요할 때만!)

**화이팅! 🚀**
