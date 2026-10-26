#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>

#include "l2_evset.h"
#include "l2_ipc.h"
#include "l2_util.h"
#include "l2_victim.h"
#include <sys/prctl.h>
#include <stdbool.h>

#ifndef PR_SCHED_CORE_RESERVE
#define PR_SCHED_CORE_RESERVE 0x43525356
#endif

#ifndef PR_SCHED_CORE_RESERVE_ENABLE
#define PR_SCHED_CORE_RESERVE_ENABLE 1
#endif

#ifndef PR_SCHED_CORE_RESERVE_DISABLE
#define PR_SCHED_CORE_RESERVE_DISABLE 2
#endif

#define VICTIM_CPU 11
#define POOL_MAX_PAGES 4096

static int valid_set(uint32_t set)
{
    return set == 30 ||
           set == 60 ||
           set == 99;
}

static ipc_state *connect_ipc(void)
{
    ipc_state *ipc;
    int fd = shm_open(IPC_SHM_NAME, O_RDWR, 0);

    if (fd < 0) {
        perror("shm_open");
        fprintf(stderr, "Start the attacker first.\n");
        return NULL;
    }

    ipc = mmap(NULL, sizeof(*ipc), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ipc == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    while (!atomic_load_explicit(&ipc->initialized, memory_order_acquire)) {
        usleep(1000);
    }

    sem_post(&ipc->ready_sem);

    return ipc;
}

static void disconnect_ipc(ipc_state *ipc)
{
    if (ipc) {
        munmap(ipc, sizeof(*ipc));
    }
}

// Returns 0 for stop, 1 for a command, or -1 for a wait failure.
static int receive_command(ipc_state *ipc, char *command)
{
    int result;
    do {
        result = sem_wait(&ipc->go_sem);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("sem_wait go");
        return -1;
    }

    pthread_mutex_lock(&ipc->mutex);
    if (ipc->stop) {
        pthread_mutex_unlock(&ipc->mutex);
        return 0;
    }

    *command = ipc->command;
    pthread_mutex_unlock(&ipc->mutex);

    return 1;
}

static void send_done(ipc_state *ipc)
{
    sem_post(&ipc->done_sem);
}

int main(int argc, char **argv)
{
    if (argc != 3 ||
        (strcmp(argv[2], "on") != 0 &&
         strcmp(argv[2], "off") != 0)) {
        fprintf(stderr,
                "Usage: %s <set: 30, 60, or 99> <on|off>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    bool mitigation = strcmp(argv[2], "on") == 0;

    char *end;
    errno = 0;
    unsigned long value = strtoul(argv[1], &end, 10);

    if (errno || end == argv[1] || *end ||
        value > UINT32_MAX || !valid_set((uint32_t)value)) {
        fprintf(stderr, "Set must be 30, 60, or 99\n");
        return EXIT_FAILURE;
    }

    uint32_t selected_set = (uint32_t)value;
    int exit_status = EXIT_FAILURE;
    bool reserved = false;
    bool ready = false;
    l2_evsets victim_set = {0};

    ipc_state *ipc = connect_ipc();
    if (!ipc)
        return EXIT_FAILURE;

    /*
     * Build the victim's cache-line mapping on its intended CPU.
     * CPU 11 will also be in its affinity mask when enable is called.
     */
    if (pin_to_cpu(VICTIM_CPU) == -1) {
        perror("pin_to_cpu victim");
        goto cleanup;
    }

    if (l2_evsets_create(&victim_set, &selected_set,
                         1, 1, POOL_MAX_PAGES) == -1) {
        perror("l2_evsets_create");
        goto cleanup;
    }

    l2_line *victim_line = victim_set.sets[0].head;

    if (mitigation) {
        if (prctl(PR_SCHED_CORE_RESERVE,
                  (unsigned long)PR_SCHED_CORE_RESERVE_ENABLE,
                  (unsigned long)VICTIM_CPU,
                  0UL, 0UL) == -1) {
            perror("enable core reservation");
            goto cleanup;
        }

        reserved = true;
    }

    fprintf(stderr,
            "Victim PID %d on CPU %d; set %u; mitigation %s\n",
            getpid(), sched_getcpu(), selected_set,
            mitigation ? "ON" : "OFF");

    /*
     * The attacker may start measuring only after reservation succeeds.
     */
    ready = true;
    sem_post(&ipc->ready_sem);

    char command;
    int received;

    while ((received = receive_command(ipc, &command)) > 0) {
        if (command == 1)
            l2_victim_touch(victim_line);

        send_done(ipc);
    }

    exit_status = received == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    if (reserved) {
        if (prctl(PR_SCHED_CORE_RESERVE,
                  (unsigned long)PR_SCHED_CORE_RESERVE_DISABLE,
                  0UL, 0UL, 0UL) == -1) {
            perror("disable core reservation");
            exit_status = EXIT_FAILURE;
        }
    }

    if (!ready) {
        /*
         * Setup failed: wake the attacker and tell it not to measure.
         */
        pthread_mutex_lock(&ipc->mutex);
        ipc->stop = 1;
        pthread_mutex_unlock(&ipc->mutex);

        sem_post(&ipc->ready_sem);
    } else {
        /*
         * Acknowledge termination only after releasing the reservation.
         */
        send_done(ipc);
    }

    disconnect_ipc(ipc);
    l2_evsets_destroy(&victim_set);

    fprintf(stderr, "Victim finished.\n");
    return exit_status;
}