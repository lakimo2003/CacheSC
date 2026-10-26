#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* One victim (parent) and N workers. No shared counter in the hot loop.
 * jobs = 100000 dependent integer operations, not a cache-attack workload.
 * Setup/teardown excluded from throughput; reservation times reported apart.
 * Run each trial as a fresh process. All children exist before reservation.
 */
#define PR_SCHED_CORE_RESERVE 0x43525356
#define PR_SCHED_CORE_RESERVE_ENABLE 1
#define PR_SCHED_CORE_RESERVE_DISABLE 2
#define MAX_WORKERS 128
struct result
{
    uint64_t jobs,
        checksum;
    int cpu;
    char padding[40];
};
struct shared
{
    atomic_int ready, go, abort_run;
    uint64_t end_ns;
    struct result result[MAX_WORKERS + 1];
};
static volatile sig_atomic_t interrupted;
static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
    {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void pause_briefly(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static long number(const char *s, long min, long max)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || end == s || *end || n < min || n > max)
    {
        fprintf(stderr, "Invalid argument: %s\n", s);
        exit(1);
    }
    return n;
}
static void work(struct shared *s, int id)
{
    uint64_t x = 0x123456789abcdefULL, count = 0;
    while (!interrupted && !atomic_load(&s->abort_run))
    {
        if (now_ns() >= s->end_ns)
            break;
        for (unsigned i = 0; i < 100000; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
        }
        /* Count only complete jobs that finished within the interval. */
        if (now_ns() <= s->end_ns)
            count++;
    }
    s->result[id].jobs = count;
    s->result[id].checksum = x;
    s->result[id].cpu = sched_getcpu();
}
int main(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[1], "on") && strcmp(argv[1], "off")))
    {
        fprintf(stderr, "Usage: %s on|off seconds victim_cpu workers\n", argv[0]);
        return 1;
    }
    int enabled = !strcmp(argv[1], "on");
    long seconds = number(argv[2], 1, 60);
    int cpu = (int)number(argv[3], 0, CPU_SETSIZE - 1);
    int workers = (int)number(argv[4], 1, MAX_WORKERS);
    cpu_set_t original, one;
    if (sched_getaffinity(0, sizeof(original), &original))
    {
        perror("affinity");
        return 1;
    }
    if (!CPU_ISSET(cpu, &original) || CPU_COUNT(&original) < 2)
    {
        fprintf(stderr, "Victim CPU must be allowed; need at least two allowed CPUs.\n");
        return 1;
    }
    struct shared *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }
    memset(s, 0, sizeof(*s));
    atomic_init(&s->ready, 0);
    atomic_init(&s->go, 0);
    atomic_init(&s->abort_run, 0);
    if (!atomic_is_lock_free(&s->ready))
    {
        fprintf(stderr, "Need lock-free atomics.\n");
        return 1;
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    pid_t children[MAX_WORKERS];
    int spawned = 0, reserved = 0, failed = 1;
    uint64_t setup_ns = 0, release_ns = 0;
    for (int i = 0; i < workers; i++)
    {
        pid_t p = fork();
        if (p < 0)
        {
            perror("fork");
            goto cleanup;
        }
        if (!p)
        {
            atomic_fetch_add(&s->ready, 1);
            while (!atomic_load_explicit(&s->go, memory_order_acquire) &&
                   !atomic_load(&s->abort_run) && !interrupted)
                pause_briefly();
            if (atomic_load(&s->abort_run) || interrupted)
                _exit(1);
            work(s, i + 1);
            _exit(interrupted ? 1 : 0);
        }
        children[spawned++] = p;
    }
    uint64_t timeout = now_ns() + 10000000000ULL;
    while (atomic_load(&s->ready) != workers)
    {
        if (interrupted || now_ns() > timeout)
        {
            fprintf(stderr, "Worker startup failed.\n");
            goto cleanup;
        }
        pause_briefly();
    }
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    if (sched_setaffinity(0, sizeof(one), &one))
    {
        perror("pin victim");
        goto cleanup;
    }
    if (enabled)
    {
        uint64_t t = now_ns();
        if (prctl(PR_SCHED_CORE_RESERVE,
                  (unsigned long)PR_SCHED_CORE_RESERVE_ENABLE,
                  (unsigned long)cpu, 0UL, 0UL);)
        {
            perror("enable reservation (no results recorded)");
            goto cleanup;
        }
        setup_ns = now_ns() - t;
        reserved = 1;
    }
    /* Publish deadline before releasing workers. Short polling startup latency
     * is included in the common wall-clock interval, equally in all modes. */
    s->end_ns = now_ns() + (uint64_t)seconds * 1000000000ULL;
    atomic_store_explicit(&s->go, 1, memory_order_release);
    work(s, 0);
    failed = interrupted != 0;
cleanup:
    if (reserved)
    {
        uint64_t t = now_ns();
        if (prctl(PR_SCHED_CORE_RESERVE, 2UL, 0UL, 0UL, 0UL))
        {
            perror("disable reservation");
            failed = 1;
        }
        release_ns = now_ns() - t;
    }
    if (failed)
    {
        atomic_store(&s->abort_run, 1);
        for (int i = 0; i < spawned; i++)
            kill(children[i], SIGTERM);
    }
    for (int i = 0; i < spawned; i++)
    {
        int status = 0;
        pid_t p;
        do
        {
            p = waitpid(children[i], &status, 0);
        } while (p < 0 && errno == EINTR);
        if (p < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
            failed = 1;
    }
    if (!failed)
    {
        uint64_t other = 0, checksum = s->result[0].checksum;
        for (int i = 1; i <= workers; i++)
        {
            other += s->result[i].jobs;
            checksum ^= s->result[i].checksum;
        }
        fprintf(stderr, "Completed: victim CPU sample=%d, allowed CPUs=%d, checksum=%" PRIu64 "\n",
                s->result[0].cpu, CPU_COUNT(&original), checksum);
        puts("mode,seconds,workers,victim_jobs,worker_jobs,total_jobs,victim_jobs_per_s,worker_jobs_per_s,total_jobs_per_s,enable_us,disable_us");
        printf("%s,%ld,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f\n",
               argv[1], seconds, workers, s->result[0].jobs, other, s->result[0].jobs + other,
               (double)s->result[0].jobs / seconds, (double)other / seconds,
               (double)(s->result[0].jobs + other) / seconds, setup_ns / 1000.0, release_ns / 1000.0);
    }
    munmap(s, sizeof(*s));
    return failed ? 1 : 0;
}
