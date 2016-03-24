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

// TODO: standard "buffer, length, capacity" construction
