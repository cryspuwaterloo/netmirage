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
#pragma once

#include <stdlib.h>

// These operations perform arithmetic on two operands and return the result in
// the third. If an overflow occurs, the program aborts. All values are
// unsigned.
#if __GNUC__ >= 5
#define eadd32(a, b, res) do{ if (__builtin_add_overflow((a), (b), (res))) abort(); }while(0)
#define eadd64(a, b, res) eadd32((a), (b), (res))
#define eaddSize(a, b, res) eadd32((a), (b), (res))
#define esub32(a, b, res) do{ if (__builtin_sub_overflow((a), (b), (res))) abort(); }while(0)
#define esub64(a, b, res) esub32((a), (b), (res))
#define esubSize(a, b, res) esub32((a), (b), (res))
#define emul32(a, b, res) do{ if (__builtin_mul_overflow((a), (b), (res))) abort(); }while(0)
#define emul64(a, b, res) emul32((a), (b), (res))
#define emulSize(a, b, res) emul32((a), (b), (res))
#else
// Compatibility with older GCC. For details, see https://www.fefe.de/intof.html
#include <stdint.h>
#define ___eadd(a, b, res, t, m) do{ t ___a = (a); t ___b = (b); if (m - ___b < ___a) abort(); *(res)=___a+___b; }while(0)
#define eadd32(a, b, res) ___eadd((a), (b), (res), uint32_t, UINT32_MAX)
#define eadd64(a, b, res) ___eadd((a), (b), (res), uint64_t, UINT64_MAX)
#define eaddSize(a, b, res) ___eadd((a), (b), (res), size_t, SIZE_MAX)
#define ___esub(a, b, res, t) do{ t ___a = (a); t ___b = (b); if (___b > ___a) abort(); *(res)=___a-___b; }while(0)
#define esub32(a, b, res) ___esub((a), (b), (res), uint32_t)
#define esub64(a, b, res) ___esub((a), (b), (res), uint64_t)
#define esubSize(a, b, res) ___esub((a), (b), (res), size_t)
#define emul32(a, b, res) do{ uint64_t ___res = (uint64_t)(a)*(b); if (___res > UINT32_MAX) abort(); *(res)=(uint32_t)___res; }while(0)
#define emul64(a, b, res) do{ \
		uint64_t ___ma = (a); uint64_t ___mb = (b); \
		uint64_t ___a1 = ___ma >> 32; uint64_t ___b1 = ___mb >> 32; \
		uint64_t ___a0 = ___ma & UINT32_MAX; uint64_t ___b0 = ___mb & UINT32_MAX; \
		if (___a1 > 0 && ___b1 > 0) abort(); \
		___ma = (uint64_t)___a1*___b0 + (uint64_t)___a0*___b1; \
		if (___ma > UINT32_MAX) abort(); \
		eadd64((___ma << 32), (uint64_t)___a0*___b0, res); \
	}while(0)
#if SIZE_MAX == UINT64_MAX
#define emulSize(a, b, res) emul64((a), (b), (res))
#elif SIZE_MAX == UINT32_MAX
#define emulSize(a, b, res) emul32((a), (b), (res))
#else
#error "Unsupported size_t size in GCC 4 compatibility mode"
#endif
#endif

// These functions are analogous to malloc, calloc, and realloc, but abort if
// they fail to allocate memory.
void* emalloc(size_t size);
void* ecalloc(size_t count, size_t eltsize);
void* erealloc(void* ptr, size_t newsize);

// Convenience functions for allocating memory with size ((mul1 * mul2) + add)
// while also aborting if any arithmetic overflows or memory runs out. More
// complex size operations can be performed using the eadd/esub/emul macros.
void* eamalloc(size_t mul1, size_t mul2, size_t add);
void* eacalloc(size_t mul1, size_t mul2, size_t add);
void* earealloc(void* ptr, size_t mul1, size_t mul2, size_t add);

// The following functions support a "flexible buffer" design pattern. A
// flexBuffer consists of a block of memory, the capacity of that block, and the
// length of the bytes stored in the block. An invariant is len <= cap. Buffers
// can be grown but may not shrink. When the buffer needs to grow, its size
// becomes double the requested capacity. flexBuffers are similar to std::vector
// in C++'s STL. We do not define an explicit struct because some callers store
// the buffer, length, and capacity as part of different structures. It is
// possible to pass a NULL length to the various functions to avoid tracking the
// buffer length. In this mode, all appends to the buffer write to the start (in
// other words, it is not possible to "accumulate" data).

void flexBufferInit(void** buffer, size_t* len, size_t* cap);
void flexBufferFree(void** buffer, size_t* len, size_t* cap);

// Ensures that the flexBuffer is large enough to fit (len + additionalSpace)
// elements of size eltsize. It is grown if necessary, in which case buffer may
// point to a new memory block after the call. The program will abort if it runs
// out of memory. Callers should ensure that additionalSpace is computed with
// the e* functions to ensure that no overflow occurs.
void flexBufferGrow(void** buffer, size_t len, size_t* cap, size_t additionalSpace, size_t eltsize);

// Appends data to the end of a flexBuffer and increases its length accordingly.
// The caller should use flexBufferGrow to ensure that the buffer is large
// enough to hold the new data. The size of the data to append is
// (dataLen * eltsize) bytes.
void flexBufferAppend(void* buffer, size_t* len, const void* data, size_t dataLen, size_t eltsize);

// Convenience function that grows a buffer and appends a string to it.
void flexBufferGrowAppendStr(void** buffer, size_t* len, size_t* cap, const char* str);

// Convenience function that grows a buffer and appends a formatted string.
void flexBufferPrintf(void** buffer, size_t* len, size_t* cap, const char* fmt, ...);
