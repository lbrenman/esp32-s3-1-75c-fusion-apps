#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_8BIT 8
size_t heap_caps_get_free_size(unsigned caps);
