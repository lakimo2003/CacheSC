#ifndef L2_TIMING_H
#define L2_TIMING_H

#include <stdint.h>

#if !defined(__x86_64__)
#error "This example requires x86-64."
#endif

/* Assumes Intel LFENCE semantics and user-mode RDTSCP availability. */
typedef uint64_t l2_time_t;

static inline __attribute__((always_inline)) void cpuid_barrier(void)
{
    uint32_t a = 0, b, c = 0, d;
    __asm__ volatile("cpuid"
                     : "+a"(a), "=b"(b), "+c"(c), "=d"(d)
                     :
                     : "memory");
}

static inline __attribute__((always_inline)) void mfence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

static inline __attribute__((always_inline))
unsigned char read_one_byte(const void *ptr)
{
    return *(const volatile unsigned char *)ptr;
}

/* The caller must provide eight readable bytes of normal memory. */
static inline __attribute__((always_inline))
uint64_t read_eight_bytes(const void *ptr)
{
    uint64_t value;
    __asm__ volatile("movq %1, %0"
                     : "=r"(value)
                     : "m"(*(const unsigned char (*)[8])ptr)
                     : "memory");
    return value;
}

static inline __attribute__((always_inline)) l2_time_t start_timer(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence"
                     : "=a"(low), "=d"(high)
                     :
                     : "memory");
    return ((uint64_t)high << 32) | low;
}

static inline __attribute__((always_inline)) l2_time_t stop_timer(void)
{
    uint32_t low, high;
    __asm__ volatile("rdtscp\n\tlfence"
                     : "=a"(low), "=d"(high)
                     :
                     : "rcx", "memory");
    return ((uint64_t)high << 32) | low;
}

static inline __attribute__((always_inline))
l2_time_t access_time(const void *ptr)
{
    l2_time_t start = start_timer();
    (void)read_one_byte(ptr);
    l2_time_t stop = stop_timer();
    return stop - start;
}

#endif
