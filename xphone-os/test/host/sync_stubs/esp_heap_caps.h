#pragma once
#include <cstddef>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_SPIRAM 8
void* heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void*);
