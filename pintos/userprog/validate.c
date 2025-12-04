#include "userprog/validate.h"

#include "threads/thread.h"
#include "threads/vaddr.h"

static int64_t get_user(const uint8_t *uaddr);
static int64_t put_user(uint8_t *udst, uint8_t byte);

bool valid_address(const void *uaddr, bool write)
{
	if (uaddr == NULL || !is_user_vaddr(uaddr))
		return false;
	return (write ? put_user(uaddr, 0) : get_user(uaddr)) != -1;
}

static int64_t get_user(const uint8_t *uaddr)
{
	int64_t result;
	__asm __volatile("movabsq $done_get, %0\n"
					 "movzbq %1, %0\n"
					 "done_get:\n"
					 : "=&a"(result)
					 : "m"(*uaddr));
	return result;
}

static int64_t put_user(uint8_t *udst, uint8_t byte)
{
	int64_t error_code;
	__asm __volatile("movabsq $done_put, %0\n"
					 "movb %b2, %1\n"
					 "done_put:\n"
					 : "=&a"(error_code), "=m"(*udst)
					 : "q"(byte));
	return error_code;
}

/* 바이트 배열의 유효성을 검사하는 함수
 * - 문자열의 경우 (size == 0): 시작과 끝(NULL terminator 또는 NAME_MAX)만 검사
 * - 바이트 배열의 경우 (size > 0): 시작과 끝만 검사
 *
 * uaddr: 검사할 주소
 * size: 배열의 크기 (문자열의 경우 0, read/write의 경우 실제 크기)
 * write: 쓰기 권한까지 필요한지 여부
 */
bool valid_address_new(const void *uaddr, size_t size, bool write)
{
	if (uaddr == NULL || !is_user_vaddr(uaddr))
		return false;

	// 시작 주소 검사
	if (!valid_address(uaddr, write))
		return false;

	// size == 0: 문자열 (NULL-terminated)
	if (size == 0) {
		const char *str = (const char *)uaddr;
		// 문자열의 끝을 찾되, NAME_MAX를 넘지 않도록
		for (size_t i = 0; i <= NAME_MAX; i++) {
			const char *current = str + i;
			// 현재 위치 검사
			if (!valid_address(current, write))
				return false;
			// NULL terminator를 찾으면 성공
			if (*current == '\0')
				return true;
		}
		// NAME_MAX를 넘어가면 실패
		return false;
	}
	// size > 0: 바이트 배열 (read/write 버퍼)
	else {
		// 끝 주소 검사 (마지막 바이트)
		const void *end_addr = (const char *)uaddr + size - 1;
		if (!is_user_vaddr(end_addr))
			return false;
		if (!valid_address(end_addr, write))
			return false;
		return true;
	}
}