/*******************************************************************************
 * Copyright © 2016 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
 * Foundation for Education, Science and Community Development.
 *
 * This file is part of NetMirage.
 *
 * NetMirage is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * NetMirage is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with NetMirage. If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
#include "mem.h"

#include <stdarg.h>
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
		emulSize((mul1), (mul2), &size); \
		eaddSize(size, (add), &size); \
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
	eaddSize(len, additionalSpace, &newCap);
	if (newCap > *cap) {
		emulSize(newCap, 2, cap);
		emulSize(*cap, eltsize, &newCap); // Reuse newCap to express in # of bytes
		*buffer = erealloc(*buffer, newCap);
		#ifdef DEBUG
			// Initialize memory for tools like valgrind, even though we
			// carefully control the used portion of the buffer
			size_t lenBytes;
			eaddSize(len, additionalSpace, &lenBytes);
			emulSize(lenBytes, eltsize, &lenBytes);
			memset(&((char*)*buffer)[lenBytes], 0xCE, newCap - lenBytes);
		#endif
	}
}

void flexBufferAppend(void* buffer, size_t* len, const void* data, size_t dataLen, size_t eltsize) {
	size_t copyLen = 0;
	emulSize(dataLen, eltsize, &copyLen);

	size_t startByte = 0;
	if (len != NULL) {
		emulSize(*len, eltsize, &startByte);
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
