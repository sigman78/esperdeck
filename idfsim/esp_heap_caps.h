#pragma once
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT     0x00000004u
#define MALLOC_CAP_SPIRAM   0x00000200u
#define MALLOC_CAP_INTERNAL 0x00000400u

static inline void *heap_caps_malloc(size_t s, uint32_t c)           { (void)c; return malloc(s); }
static inline void *heap_caps_calloc(size_t n, size_t s, uint32_t c) { (void)c; return calloc(n, s); }
static inline void *heap_caps_realloc(void *p, size_t s, uint32_t c) { (void)c; return realloc(p, s); }
static inline void  heap_caps_free(void *p)                           { free(p); }
/* Real block size, so the allocation gauge in ssh_client.c is meaningful. */
static inline size_t heap_caps_get_allocated_size(void *p)            { return p ? _msize(p) : 0; }
