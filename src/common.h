#ifndef hokus_common_h
#define hokus_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HK_WASM is defined by the Makefile for the freestanding browser build:
   no libc — memory comes from a static bump heap, output goes to a buffer. */
#ifdef HK_WASM
void *hk_alloc(size_t size); /* bump allocator, never frees */
#define HK_REALLOC(ptr, old, new) hk_realloc_bump(ptr, old, new)
void *hk_realloc_bump(void *ptr, size_t oldSize, size_t newSize);
#define HK_FREE(ptr, size) ((void)(ptr), (void)(size))
#else
#include <stdlib.h>
#define HK_REALLOC(ptr, old, new) realloc(ptr, new)
#define HK_FREE(ptr, size) free(ptr)
#endif

#endif
