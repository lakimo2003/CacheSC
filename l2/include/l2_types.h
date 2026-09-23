#ifndef L2_TYPES_H
#define L2_TYPES_H

#include <stdint.h>

#include "l2_config.h"

typedef struct l2_line {
    _Alignas(CACHELINE_SIZE) struct l2_line *next;
    struct l2_line *prev;
    uint16_t set;
    uint16_t flags;
    uint32_t cycles;
    char padding[CACHELINE_SIZE - 2 * sizeof(struct l2_line *)
                    - 2 * sizeof(uint16_t) - sizeof(uint32_t)];
} l2_line;

_Static_assert(sizeof(l2_line) == CACHELINE_SIZE,
               "Each L2 node must occupy exactly one cache line");
_Static_assert(_Alignof(l2_line) == CACHELINE_SIZE,
               "L2 nodes must be cache-line aligned");

#endif
