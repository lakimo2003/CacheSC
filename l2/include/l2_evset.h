#ifndef L2_EVSET_H
#define L2_EVSET_H

#include <stddef.h>
#include <stdint.h>

#include "l2_types.h"

typedef struct {
    uint32_t id;
    size_t count;
    l2_line *head;
} l2_evset;

typedef struct {
    void *pool;
    size_t pool_size;
    size_t pages_scanned;
    l2_evset *sets;
    size_t set_count;
} l2_evsets;

/* Initialize out with {0}. Set order matches set_ids. Returns 0 on success,
 * -1 with errno on failure, leaving out unchanged. max_pages bounds the pool.
 * Use L2_ASSOCIATIVITY lines for the probe, or one line for the victim.
 * Assumes the configured geometry and direct physical modulo indexing. */
int l2_evsets_create(l2_evsets *out, const uint32_t *set_ids, size_t set_count,
                     size_t lines_per_set, size_t max_pages);

/* Releases the entire pool and metadata, then zeros the owner. Do not copy
 * a live owner or free individual heads. NULL and repeated cleanup are safe. */
void l2_evsets_destroy(l2_evsets *sets);

#endif
