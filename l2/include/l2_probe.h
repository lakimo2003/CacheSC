#ifndef L2_PROBE_H
#define L2_PROBE_H

#include <stddef.h>

#include "l2_timing.h"
#include "l2_types.h"

_Static_assert(sizeof(void *) == 8, "The probe requires 64-bit pointers");
_Static_assert(L2_ASSOCIATIVITY > 0, "The probe needs at least one line");

typedef struct {
    l2_line *next;
    l2_time_t ticks;
} l2_probe_result;

/* First L2_ASSOCIATIVITY nodes along prev must belong to the measured set.
 * next is the pointer reached after those loads; it is not dereferenced here.
 * Caller verifies geometry, mappings, and RDTSCP support during setup. */
static inline __attribute__((always_inline))
l2_probe_result l2_probe_set(l2_line *head)
{
    l2_line *cursor = head;
    uint64_t start, ticks;

    __asm__ volatile(
        "lfence\n\t"
        "rdtsc\n\t"
        "shlq $32, %%rdx\n\t"
        "orq %%rdx, %%rax\n\t"
        "movq %%rax, %[start]\n\t"
        "lfence\n\t"
        ".rept %c[ways]\n\t"
        "movq %c[prev](%[cursor]), %[cursor]\n\t"
        ".endr\n\t"
        "rdtscp\n\t"
        "lfence\n\t"
        "shlq $32, %%rdx\n\t"
        "orq %%rdx, %%rax\n\t"
        "subq %[start], %%rax\n\t"
        : [cursor] "+&r"(cursor), [start] "=&r"(start), "=&a"(ticks)
        : [prev] "i"(offsetof(l2_line, prev)),
          [ways] "i"(L2_ASSOCIATIVITY)
        : "rdx", "rcx", "cc", "memory");

    return (l2_probe_result){ .next = cursor, .ticks = ticks };
}

/* Warm the same dependent chain, outside any measurement interval. */
static inline __attribute__((always_inline))
l2_line *l2_prime_set(l2_line *head)
{
    l2_line *cursor = head;
    __asm__ volatile(
        "lfence\n\t"
        ".rept %c[ways]\n\t"
        "movq %c[prev](%[cursor]), %[cursor]\n\t"
        ".endr\n\t"
        "lfence\n\t"
        : [cursor] "+r"(cursor)
        : [prev] "i"(offsetof(l2_line, prev)),
          [ways] "i"(L2_ASSOCIATIVITY)
        : "memory");
    return cursor;
}

#endif
