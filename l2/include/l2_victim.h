#ifndef L2_VICTIM_H
#define L2_VICTIM_H

#include "l2_timing.h"

/* Matches the old victim's fence followed by one eight-byte load. */
static inline __attribute__((always_inline))
void l2_victim_touch(const void *line)
{
    mfence();
    (void)read_eight_bytes(line);
}

#endif
