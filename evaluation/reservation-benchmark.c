#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PR_SCHED_CORE_RESERVE 0x43525356
#define PR_SCHED_CORE_RESERVE_ENABLE 1
#define PR_SCHED_CORE_RESERVE_DISABLE 2

#define MAX_WORKERS 128
#define ITERATIONS_PER_JOB 100000
#define NANOSECONDS_PER_SECOND 1000000000ULL

struct ProcessResult
{
    uint64_t completed_jobs;
    uint64_t checksum;
    int cpu_at_finish;
};

struct Shared
{
    atomic_int ready_worker_count;
    atomic_int start_requested;
    atomic_int stop_requested;

    uint64_t benchmark_end_time_ns;

    // index 0 is the victim, rest are workers
    struct ProcessResult process_results[MAX_WORKERS + 1];
};

static uint64_t current_time_nanoseconds(void)
{
    struct timespec current_time;

    if (clock_gettime(CLOCK_MONOTONIC, &current_time) == -1)
    {
        perror("read clock");
        exit(EXIT_FAILURE);
    }

    uint64_t whole_seconds_ns =
        (uint64_t)current_time.tv_sec * NANOSECONDS_PER_SECOND;

    return whole_seconds_ns + (uint64_t)current_time.tv_nsec;
}

static void sleep_one_millisecond(void)
{
    struct timespec sleep_duration = {0, 1000000};
    nanosleep(&sleep_duration, NULL);
}

static int read_integer_argument(const char *name, const char *text,
                                 int minimum, int maximum)
{
    char *remaining_text;

    errno = 0;
    long value = strtol(text, &remaining_text, 10);

    if (errno != 0 || remaining_text == text || *remaining_text != '\0' ||
        value < minimum || value > maximum)
    {
        fprintf(stderr, "%s must be an integer between %d and %d.\n",
                name, minimum, maximum);
        exit(EXIT_FAILURE);
    }

    return (int)value;
}

static void perform_work(struct Shared *shared, int result_index)
{
    uint64_t x = 0x123456789abcdefULL;
    uint64_t completed_jobs = 0;

    while (!atomic_load(&shared->stop_requested))
    {
        if (current_time_nanoseconds() >= shared->benchmark_end_time_ns)
        {
            break;
        }

        for (unsigned int iteration = 0;
             iteration < ITERATIONS_PER_JOB;
             iteration++)
        {
            x = x ^ (x << 13);
            x = x ^ (x >> 7);
            x = x ^ (x << 17);
        }

        // job that runs past the deadline does not count
        if (current_time_nanoseconds() <= shared->benchmark_end_time_ns)
        {
            completed_jobs++;
        }
    }

    shared->process_results[result_index].completed_jobs = completed_jobs;
    shared->process_results[result_index].checksum = x;
    shared->process_results[result_index].cpu_at_finish = sched_getcpu();
}

static void run_worker(struct Shared *shared, int result_index)
{
    atomic_fetch_add(&shared->ready_worker_count, 1);

    while (!atomic_load(&shared->start_requested))
    {
        if (atomic_load(&shared->stop_requested))
        {
            _exit(EXIT_FAILURE);
        }

        sleep_one_millisecond();
    }

    if (atomic_load(&shared->stop_requested))
    {
        _exit(EXIT_FAILURE);
    }

    perform_work(shared, result_index);

    _exit(EXIT_SUCCESS);
}

int main(int argument_count, char **arguments)
{
    // requested mode, duration, victim CPU, and worker count
    if (argument_count != 5)
    {
        fprintf(stderr, "Usage: %s on|off seconds victim_cpu workers\n",
                arguments[0]);
        return EXIT_FAILURE;
    }

    bool mitigation_enabled;
    if (strcmp(arguments[1], "on") == 0)
    {
        mitigation_enabled = true;
    }
    else if (strcmp(arguments[1], "off") == 0)
    {
        mitigation_enabled = false;
    }
    else
    {
        fprintf(stderr, "The mitigation mode must be on or off.\n");
        return EXIT_FAILURE;
    }

    int duration_seconds = read_integer_argument("Duration", arguments[2], 1, 60);
    int victim_cpu = read_integer_argument("Victim CPU", arguments[3],
                                           0, CPU_SETSIZE - 1);
    int worker_count = read_integer_argument("Worker count", arguments[4],
                                             1, MAX_WORKERS);

    cpu_set_t allowed_cpus;
    if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) == -1)
    {
        perror("read allowed CPUs");
        return EXIT_FAILURE;
    }

    if (!CPU_ISSET(victim_cpu, &allowed_cpus) || CPU_COUNT(&allowed_cpus) < 2)
    {
        fprintf(stderr, "The victim CPU must be allowed, and at least two "
                        "logical CPUs must be available.\n");
        return EXIT_FAILURE;
    }

    struct Shared *shared = mmap(
        NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (shared == MAP_FAILED)
    {
        perror("create shared memory");
        return EXIT_FAILURE;
    }

    memset(shared, 0, sizeof(*shared));
    atomic_init(&shared->ready_worker_count, 0);
    atomic_init(&shared->start_requested, 0);
    atomic_init(&shared->stop_requested, 0);

    pid_t worker_process_ids[MAX_WORKERS];
    int workers_created = 0;
    bool reservation_is_active = false;
    bool benchmark_succeeded = false;
    uint64_t enable_duration_ns = 0;
    uint64_t disable_duration_ns = 0;

    // create workers while the parent still has its original affinity.
    // workers inherit that affinity
    for (int worker_index = 0; worker_index < worker_count; worker_index++)
    {
        pid_t worker_process_id = fork();

        if (worker_process_id == -1)
        {
            perror("create worker");
            goto cleanup;
        }

        if (worker_process_id == 0)
        {
            run_worker(shared, worker_index + 1);
        }

        worker_process_ids[workers_created] = worker_process_id;
        workers_created++;
    }

    // wait until every worker is ready
    uint64_t worker_startup_deadline_ns =
        current_time_nanoseconds() + 10 * NANOSECONDS_PER_SECOND;

    while (atomic_load(&shared->ready_worker_count) < worker_count)
    {
        if (current_time_nanoseconds() >= worker_startup_deadline_ns)
        {
            fprintf(stderr, "Timed out waiting for workers.\n");
            goto cleanup;
        }

        sleep_one_millisecond();
    }

    cpu_set_t victim_cpu_set;
    CPU_ZERO(&victim_cpu_set);
    CPU_SET(victim_cpu, &victim_cpu_set);

    if (sched_setaffinity(0, sizeof(victim_cpu_set), &victim_cpu_set) == -1)
    {
        perror("pin victim to CPU");
        goto cleanup;
    }

    // enable mitigation
    if (mitigation_enabled)
    {
        uint64_t enable_start_time_ns = current_time_nanoseconds();

        if (prctl(PR_SCHED_CORE_RESERVE, PR_SCHED_CORE_RESERVE_ENABLE,
                  (unsigned long)victim_cpu, 0UL, 0UL) == -1)
        {
            perror("enable reservation");
            goto cleanup;
        }

        enable_duration_ns = current_time_nanoseconds() - enable_start_time_ns;
        reservation_is_active = true;
    }

    // the deadline to decide when to stop starting new jobs
    shared->benchmark_end_time_ns = current_time_nanoseconds() + (uint64_t)duration_seconds * NANOSECONDS_PER_SECOND;
    atomic_store(&shared->start_requested, 1);

    perform_work(shared, 0); /* The parent performs the victim workload. */
    benchmark_succeeded = true;

cleanup:
    if (reservation_is_active)
    {
        uint64_t disable_start_time_ns = current_time_nanoseconds();

        if (prctl(PR_SCHED_CORE_RESERVE, PR_SCHED_CORE_RESERVE_DISABLE,
                  0UL, 0UL, 0UL) == -1)
        {
            perror("disable reservation");
            benchmark_succeeded = false;
        }

        disable_duration_ns = current_time_nanoseconds() - disable_start_time_ns;
    }

    if (!benchmark_succeeded)
    {
        atomic_store(&shared->stop_requested, 1);

        for (int worker_index = 0; worker_index < workers_created; worker_index++)
        {
            kill(worker_process_ids[worker_index], SIGTERM);
        }
    }

    // wait for workers before reading their results
    for (int worker_index = 0; worker_index < workers_created; worker_index++)
    {
        int worker_exit_status;
        pid_t waited_process_id;

        do
        {
            waited_process_id = waitpid(worker_process_ids[worker_index],
                                        &worker_exit_status, 0);
        } while (waited_process_id == -1 && errno == EINTR);

        if (waited_process_id == -1)
        {
            perror("wait for worker");
            benchmark_succeeded = false;
        }
        else if (!WIFEXITED(worker_exit_status) ||
                 WEXITSTATUS(worker_exit_status) != EXIT_SUCCESS)
        {
            fprintf(stderr, "A worker did not finish successfully.\n");
            benchmark_succeeded = false;
        }
    }

    if (benchmark_succeeded)
    {
        uint64_t victim_jobs = shared->process_results[0].completed_jobs;
        uint64_t worker_jobs = 0;
        uint64_t combined_checksum = shared->process_results[0].checksum;

        for (int worker_index = 1; worker_index <= worker_count; worker_index++)
        {
            worker_jobs += shared->process_results[worker_index].completed_jobs;
            combined_checksum ^= shared->process_results[worker_index].checksum;
        }

        uint64_t total_jobs = victim_jobs + worker_jobs;
        double victim_jobs_per_second = (double)victim_jobs / duration_seconds;
        double worker_jobs_per_second = (double)worker_jobs / duration_seconds;
        double total_jobs_per_second = (double)total_jobs / duration_seconds;
        double enable_duration_us = enable_duration_ns / 1000.0;
        double disable_duration_us = disable_duration_ns / 1000.0;

        fprintf(stderr, "Completed: victim CPU sample=%d, allowed CPUs=%d, "
                        "checksum=%" PRIu64 "\n",
                shared->process_results[0].cpu_at_finish,
                CPU_COUNT(&allowed_cpus), combined_checksum);

        puts("mode,seconds,workers,victim_jobs,worker_jobs,total_jobs,"
             "victim_jobs_per_s,worker_jobs_per_s,total_jobs_per_s,"
             "enable_us,disable_us");

        printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
               "%.3f,%.3f,%.3f,%.3f,%.3f\n",
               arguments[1], duration_seconds, worker_count,
               victim_jobs, worker_jobs, total_jobs,
               victim_jobs_per_second, worker_jobs_per_second,
               total_jobs_per_second, enable_duration_us, disable_duration_us);
    }

    munmap(shared, sizeof(*shared));

    if (benchmark_succeeded)
    {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
