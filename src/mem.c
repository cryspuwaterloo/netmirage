#include "mem.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

void* emalloc(size_t size) {
	void* p = malloc(size);
	if (p == NULL) abort();
	return p;
}

void* ecalloc(size_t count, size_t eltsize) {
	void* p = calloc(count, eltsize);
	if (p == NULL) abort();
	return p;
}

void* erealloc(void* ptr, size_t newsize) {
	void* p = realloc(ptr, newsize);
	if (p == NULL) abort();
	return p;
}

#define COMPUTE_SIZE_NOOVERFLOW(size, mul1, mul2, add) \
	do { \
		emul((mul1), (mul2), &size); \
		eadd(size, (add), &size); \
	} while (0)

void* eamalloc(size_t mul1, size_t mul2, size_t add) {
	size_t size;
	COMPUTE_SIZE_NOOVERFLOW(size, mul1, mul2, add);
	return emalloc(size);
}

void* eacalloc(size_t mul1, size_t mul2, size_t add) {
	size_t size;
	COMPUTE_SIZE_NOOVERFLOW(size, mul1, mul2, add);
	return ecalloc(size, 1);
}

void* earealloc(void* ptr, size_t mul1, size_t mul2, size_t add) {
	size_t size;
	COMPUTE_SIZE_NOOVERFLOW(size, mul1, mul2, add);
	return erealloc(ptr, size);
}
