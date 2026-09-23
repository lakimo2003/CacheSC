#define _GNU_SOURCE
#include "l2_util.h"
#include "l2_timing.h"

#include <sched.h>
#include <time.h>

int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    return sched_setaffinity(0, sizeof(set), &set);
}

int l2_prepare_measurement(void)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1)
        return -1;

    /* Preserve the warm-up phase without assuming a processor frequency. */
    do {
        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
            return -1;
    } while (now.tv_sec < start.tv_sec + 2 ||
             (now.tv_sec == start.tv_sec + 2 && now.tv_nsec < start.tv_nsec));

    for (unsigned int i = 0; i < 200; ++i)
        (void)start_timer();
    cpuid_barrier();
    return 0;
}
