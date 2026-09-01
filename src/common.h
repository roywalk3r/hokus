#ifndef hokus_common_h
#define hokus_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HK_WASM is defined by the Makefile for the freestanding browser build:
   no libc — memory comes from a static bump heap, output goes to a buffer. */
#ifdef HK_WASM
void *hk_alloc(size_t size); /* free-list first, then bump */
#define HK_REALLOC(ptr, old, new) hk_realloc_bump(ptr, old, new)
void *hk_realloc_bump(void *ptr, size_t oldSize, size_t newSize);
void hk_free_bump(void *ptr, size_t size); /* recycles blocks <= 256 bytes */
#define HK_FREE(ptr, size) hk_free_bump(ptr, size)
#else
#include <stdlib.h>
#define HK_REALLOC(ptr, old, new) realloc(ptr, new)
#define HK_FREE(ptr, size) free(ptr)
#endif

/* Truncated modulo without libm (fmod semantics): cast truncation is exact
   for |a/b| < ~2^62; shared by the VM and the compiler's constant folder so
   folded constants and runtime results never disagree. */
static inline double hkModulo(double a, double b) {
    double t = a / b;
    double q = (t >= -4.6e18 && t <= 4.6e18) ? (double)(long long)t : t;
    return a - b * q;
}

#endif
