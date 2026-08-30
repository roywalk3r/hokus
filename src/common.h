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

#endif
