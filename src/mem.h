#pragma once

#include <stdlib.h>

// These operations perform arithmetic on two operands, return the result in the
// third, and return true if and only if the result overflowed.
#ifdef __GNUC__
#define add_overflow __builtin_add_overflow
#define sub_overflow __builtin_sub_overflow
#define mul_overflow __builtin_mul_overflow
#else
#error "Integral overflow functions require GCC"
#endif

// These statement macros perform arithmetic on arbitrary integers and abort on
// overflow. They have the same argument structure as the *_overflow functions.
#define eadd(a, b, res) do{ if (add_overflow((a), (b), (res))) abort(); } while(0)
#define esub(a, b, res) do{ if (sub_overflow((a), (b), (res))) abort(); } while(0)
#define emul(a, b, res) do{ if (mul_overflow((a), (b), (res))) abort(); } while(0)

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
