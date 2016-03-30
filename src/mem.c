#include "mem.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define COMPUTE_SIZE_NOOVERFLOW(size, mul1, mul2, add) do{ \
		emul((mul1), (mul2), &size); \
		eadd(size, (add), &size); \
	} while(0)

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

void flexBufferInit(void** buffer, size_t* len, size_t* cap) {
	*buffer = NULL;
	if (len != NULL) *len = 0;
	*cap = 0;
}

void flexBufferFree(void** buffer, size_t* len, size_t* cap) {
	if (*buffer != NULL) {
		free(*buffer);
		*buffer = NULL;
	}
	if (len != NULL) *len = 0;
	*cap = 0;
}

void flexBufferGrow(void** buffer, size_t len, size_t* cap, size_t additionalSpace, size_t eltsize) {
	// All values are initially expressed in # of elements
	size_t newCap;
	eadd(len, additionalSpace, &newCap);
	if (newCap > *cap) {
		emul(newCap, 2, cap);
		emul(*cap, eltsize, &newCap); // Reuse newCap to express in # of bytes
		*buffer = erealloc(*buffer, newCap);
		#ifdef DEBUG
			// Initialize memory for tools like valgrind, even though we
			// carefully control the used portion of the buffer
			size_t lenBytes;
			eadd(len, additionalSpace, &lenBytes);
			emul(lenBytes, eltsize, &lenBytes);
			memset(&((char*)*buffer)[lenBytes], 0xCE, newCap - lenBytes);
		#endif
	}
}

void flexBufferAppend(void* buffer, size_t* len, const void* data, size_t dataLen, size_t eltsize) {
	size_t copyLen = 0;
	emul(dataLen, eltsize, &copyLen);

	size_t startByte = 0;
	if (len != NULL) {
		emul(*len, eltsize, &startByte);
		*len += dataLen;
	}

	memcpy(&((char*)buffer)[startByte], data, copyLen);
}

void flexBufferGrowAppendStr(void** buffer, size_t* len, size_t* cap, const char* str) {
	size_t dataLen = strlen(str)+1;
	flexBufferGrow(buffer, len != NULL ? *len : 0, cap, dataLen, 1);
	flexBufferAppend(*buffer, len, str, dataLen, 1);
}

static size_t tryFlexBufferPrintf(void* buffer, size_t* len, size_t cap, const char* fmt, va_list args) {
	size_t currentLen = (len != NULL ? *len : 0);
	char* start = &((char*)buffer)[currentLen];
	size_t freeSpace = cap - currentLen;
	int neededChars = vsnprintf(start, freeSpace, fmt, args);
	if (neededChars < 0) return 0; // Silently ignore errors
	if ((size_t)neededChars < freeSpace) {
		if (len != NULL) *len += (size_t)neededChars+1;
		return 0;
	}
	return (size_t)(neededChars+1);
}

void flexBufferPrintf(void** buffer, size_t* len, size_t* cap, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	size_t additionalSpace = tryFlexBufferPrintf(*buffer, len, *cap, fmt, args);
	if (additionalSpace > 0) {
		flexBufferGrow(buffer, len != NULL ? *len : 0, cap, additionalSpace, 1);
		va_start(args, fmt);
		tryFlexBufferPrintf(*buffer, len, *cap, fmt, args);
	}
	va_end(args);
}
