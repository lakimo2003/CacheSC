#define _GNU_SOURCE
#include "l2_evset.h"
#include "l2_pagemap.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

_Static_assert(L2_SETS > 0 && L2_SETS <= UINT16_MAX + 1u,
               "Set IDs must fit in l2_line.set");
_Static_assert(L2_ASSOCIATIVITY > 0, "Associativity must be positive");

typedef struct {
    l2_line *line;
    uint64_t physical;
} candidate;

void l2_evsets_destroy(l2_evsets *sets)
{
    if (!sets)
        return;
    if (sets->pool)
        munmap(sets->pool, sets->pool_size);
    free(sets->sets);
    *sets = (l2_evsets){0};
}

int l2_evsets_create(l2_evsets *out, const uint32_t *set_ids, size_t set_count,
                     size_t lines_per_set, size_t max_pages)
{
    if (!out || !set_ids || set_count == 0 || set_count > L2_SETS ||
        lines_per_set == 0 || lines_per_set > L2_ASSOCIATIVITY || max_pages == 0) {
        errno = EINVAL;
        return -1;
    }
    if (out->pool || out->sets) {
        errno = EALREADY;
        return -1;
    }
    for (size_t i = 0; i < set_count; ++i) {
        if (set_ids[i] >= L2_SETS) {
            errno = EINVAL;
            return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            if (set_ids[i] == set_ids[j]) {
                errno = EINVAL;
                return -1;
            }
        }
    }
    if (set_count > SIZE_MAX / lines_per_set ||
        set_count * lines_per_set > SIZE_MAX / sizeof(candidate)) {
        errno = EOVERFLOW;
        return -1;
    }

    l2_pagemap pm = { .fd = -1, .page_size = 0 };
    if (l2_pagemap_open(&pm) == -1)
        return -1;

    l2_evsets result = {0};
    candidate *chosen = NULL;
    if (pm.page_size < CACHELINE_SIZE || pm.page_size % CACHELINE_SIZE != 0) {
        errno = EINVAL;
        goto fail;
    }
    if (max_pages > (size_t)PTRDIFF_MAX / pm.page_size) {
        errno = EOVERFLOW;
        goto fail;
    }

    result.pool_size = max_pages * pm.page_size;
    result.set_count = set_count;
    result.sets = calloc(set_count, sizeof(*result.sets));
    if (!result.sets)
        goto fail;
    chosen = calloc(set_count * lines_per_set, sizeof(*chosen));
    if (!chosen)
        goto fail;
    for (size_t i = 0; i < set_count; ++i)
        result.sets[i].id = set_ids[i];

    result.pool = mmap(NULL, result.pool_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result.pool == MAP_FAILED) {
        result.pool = NULL;
        goto fail;
    }
    /* Write every page before translating, establishing private backing. */
    memset(result.pool, 0, result.pool_size);

    size_t remaining = set_count;
    for (size_t page = 0; page < max_pages && remaining != 0; ++page) {
        unsigned char *base = (unsigned char *)result.pool + page * pm.page_size;
        uint64_t physical_base;
        if (l2_virtual_to_physical(&pm, (uintptr_t)base, &physical_base) == -1)
            goto fail;
        if (physical_base > UINT64_MAX - (pm.page_size - 1)) {
            errno = EOVERFLOW;
            goto fail;
        }
        result.pages_scanned = page + 1;

        for (size_t offset = 0; offset < pm.page_size && remaining != 0;
             offset += CACHELINE_SIZE) {
            uint64_t physical = physical_base + offset;
            uint32_t id = (uint32_t)((physical / CACHELINE_SIZE) % L2_SETS);
            size_t index;
            for (index = 0; index < set_count; ++index) {
                if (result.sets[index].id == id)
                    break;
            }
            if (index == set_count || result.sets[index].count == lines_per_set)
                continue;

            l2_evset *set = &result.sets[index];
            candidate *lines = chosen + index * lines_per_set;
            size_t i;
            for (i = 0; i < set->count; ++i) {
                if (lines[i].physical == physical)
                    break;
            }
            if (i != set->count)
                continue;

            lines[set->count++] = (candidate){
                .line = (l2_line *)(base + offset), .physical = physical
            };
            if (set->count == lines_per_set)
                --remaining;
        }
    }

    if (remaining != 0) {
        errno = ENOSPC;
        goto fail;
    }
    for (size_t s = 0; s < set_count; ++s) {
        candidate *lines = chosen + s * lines_per_set;
        for (size_t i = 0; i < lines_per_set; ++i) {
            l2_line *line = lines[i].line;
            line->prev = lines[(i + 1) % lines_per_set].line;
            line->next = lines[(i + lines_per_set - 1) % lines_per_set].line;
            line->set = (uint16_t)result.sets[s].id;
        }
        result.sets[s].head = lines[0].line;
    }

    if (l2_pagemap_close(&pm) == -1)
        goto fail;
    free(chosen);
    *out = result;
    return 0;

fail: {
        int error = errno;
        l2_pagemap_close(&pm);
        free(chosen);
        l2_evsets_destroy(&result);
        errno = error;
        return -1;
    }
}
