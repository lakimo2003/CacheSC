#ifndef L2_PAGEMAP_H
#define L2_PAGEMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    size_t page_size;
} l2_pagemap;

/* Initialize with { .fd = -1, .page_size = 0 } before opening. */
int l2_pagemap_open(l2_pagemap *pm);
int l2_pagemap_close(l2_pagemap *pm);
int l2_virtual_to_physical(const l2_pagemap *pm, uintptr_t virtual_addr,
                           uint64_t *physical_addr);

/* Assumes the target L2 uses direct modulo indexing. */
int l2_physical_set(const l2_pagemap *pm, const void *ptr,
                    uint32_t line_size, uint32_t set_count, uint32_t *set);

#endif
