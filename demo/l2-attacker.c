#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cachesc.h>

#define ATTACKER_CPU 0

#define NUMBER_OF_SETS 3

#define IPC_SHM_NAME "/cachesc_l2_ipc"

typedef struct {
    pthread_mutex_t mutex;
    sem_t ready_sem;
    sem_t go_sem;
    sem_t done_sem;
    char command;
    int stop;
    int initialized;
} ipc_state;

static uint32_t monitored_sets[NUMBER_OF_SETS] = {
    30,
    60,
    99
};

static void die_pthread(const char *operation, int error)
{
    errno = error;
    perror(operation);
    exit(EXIT_FAILURE);
}

static void sem_wait_checked(sem_t *sem, const char *operation)
{
    int result;

    do {
        result = sem_wait(sem);
    } while (result < 0 && errno == EINTR);

    if (result != 0) {
        perror(operation);
        exit(EXIT_FAILURE);
    }
}

static ipc_state *create_ipc(void)
{
    pthread_mutexattr_t mutex_attr;
    ipc_state *ipc;
    int fd;
    int result;

    shm_unlink(IPC_SHM_NAME);

    fd = shm_open(IPC_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, sizeof(*ipc)) != 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(IPC_SHM_NAME);
        exit(EXIT_FAILURE);
    }

    ipc = mmap(NULL, sizeof(*ipc), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ipc == MAP_FAILED) {
        perror("mmap");
        shm_unlink(IPC_SHM_NAME);
        exit(EXIT_FAILURE);
    }

    memset(ipc, 0, sizeof(*ipc));

    result = pthread_mutexattr_init(&mutex_attr);
    if (result != 0) {
        die_pthread("pthread_mutexattr_init", result);
    }

    result = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    if (result != 0) {
        die_pthread("pthread_mutexattr_setpshared", result);
    }

    result = pthread_mutex_init(&ipc->mutex, &mutex_attr);
    if (result != 0) {
        die_pthread("pthread_mutex_init", result);
    }

    result = pthread_mutexattr_destroy(&mutex_attr);
    if (result != 0) {
        die_pthread("pthread_mutexattr_destroy", result);
    }

    if (sem_init(&ipc->ready_sem, 1, 0) != 0 ||
        sem_init(&ipc->go_sem, 1, 0) != 0 ||
        sem_init(&ipc->done_sem, 1, 0) != 0) {
        perror("sem_init");
        munmap(ipc, sizeof(*ipc));
        shm_unlink(IPC_SHM_NAME);
        exit(EXIT_FAILURE);
    }

    ipc->initialized = 1;

    return ipc;
}

static void destroy_ipc(ipc_state *ipc)
{
    if (!ipc) {
        return;
    }

    sem_destroy(&ipc->done_sem);
    sem_destroy(&ipc->go_sem);
    sem_destroy(&ipc->ready_sem);
    pthread_mutex_destroy(&ipc->mutex);
    munmap(ipc, sizeof(*ipc));
    shm_unlink(IPC_SHM_NAME);
}

static void send_command(ipc_state *ipc, char command)
{
    pthread_mutex_lock(&ipc->mutex);
    ipc->command = command;
    pthread_mutex_unlock(&ipc->mutex);

    sem_post(&ipc->go_sem);
}

static void wait_for_victim(ipc_state *ipc)
{
    sem_wait_checked(&ipc->done_sem, "sem_wait done");
}

static void request_victim_stop(ipc_state *ipc)
{
    pthread_mutex_lock(&ipc->mutex);
    ipc->stop = 1;
    pthread_mutex_unlock(&ipc->mutex);

    sem_post(&ipc->go_sem);
}


 // command == 0:
 // Victim wakes up but does not access its cache line.
 // command == 1:
 // Victim accesses its selected cache line.
static void measure_round(
    cacheline *attacker_sets,
    ipc_state *ipc,
    char command,
    time_type *measurements)
{

    //Fill all ways of sets 30, 60 and 99
    cacheline *probe_head = prime_rev(attacker_sets);

    send_command(ipc, command);

    sem_wait(&ipc->done_sem);

    //Probe the same three sets.
    probe(L2, probe_head);

    memset(
        measurements,
        0,
        L2_SETS * sizeof(*measurements)
    );

    get_msrmts_for_all_set(
        probe_head,
        measurements
    );
}

int main(int argc, char **argv)
{
    uint32_t samples = 10000;

    if (argc == 2) {
        samples = (uint32_t) strtoul(argv[1], NULL, 10);
    }

    if (samples == 0) {
        fprintf(stderr, "Usage: %s [number-of-samples]\n", argv[0]);
        return EXIT_FAILURE;
    }

    pin_to_cpu(ATTACKER_CPU);

    fprintf(
        stderr,
        "Attacker PID %d running on CPU%d\n",
        getpid(),
        ATTACKER_CPU
    );

    cache_ctx *ctx = get_cache_ctx(L2);

    cacheline *attacker_sets =
        prepare_cache_set_ds(
            ctx,
            monitored_sets,
            NUMBER_OF_SETS
        );

    ipc_state *ipc = create_ipc();

    fprintf(stderr, "Waiting for victim program...\n");

    // blocks until the victim maps the shared state
    sem_wait_checked(&ipc->ready_sem, "sem_wait ready");

    fprintf(stderr, "Victim connected. Starting attack.\n");

    uint64_t baseline_sums[NUMBER_OF_SETS] = {0};
    uint64_t attack_sums[NUMBER_OF_SETS] = {0};

    time_type measurements[L2_SETS];

    prepare_measurement();

    for (uint32_t sample = 0; sample < samples; sample++) {
        // victim wakes and responds, but does not access
        // its selected cache line.
        measure_round(
            attacker_sets,
            ipc,
            0,
            measurements
        );

        for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
            baseline_sums[i] +=
                measurements[monitored_sets[i]];
        }

        // victim accesses its selected cache line.
        measure_round(
            attacker_sets,
            ipc,
            1,
            measurements
        );

        for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
            attack_sums[i] +=
                measurements[monitored_sets[i]];
        }
    }

    request_victim_stop(ipc);
    sem_wait(&ipc->done_sem);

    printf("\nResults over %u samples:\n\n", samples);

    double largest_difference = -1000000.0;
    uint32_t detected_set = 0;

    for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
        double baseline =
            (double) baseline_sums[i] / samples;

        double attack =
            (double) attack_sums[i] / samples;

        double difference = attack - baseline;

        printf("Set %u\n", monitored_sets[i]);
        printf("  Baseline:   %.2f cycles\n", baseline);
        printf("  With touch: %.2f cycles\n", attack);
        printf("  Difference: %.2f cycles\n\n", difference);

        if (difference > largest_difference) {
            largest_difference = difference;
            detected_set = monitored_sets[i];
        }
    }

    printf(
        "Attacker predicts that the victim touched set %u\n",
        detected_set
    );

    release_cache_set_ds(ctx, attacker_sets);
    release_cache_ctx(ctx);

    destroy_ipc(ipc);

    return EXIT_SUCCESS;
}
