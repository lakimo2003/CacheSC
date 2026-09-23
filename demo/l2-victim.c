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

#define PR_SET_L2_ISOLATION 0x4C320001


#define VICTIM_CPU 1
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
    if (argc != 2) {
        fprintf(
            stderr,
            "Usage: %s <set: 30, 60, or 99>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    char *end;
    errno = 0;
    unsigned long value = strtoul(argv[1], &end, 10);
    uint32_t selected_set = (uint32_t)value;

    if (errno || end == argv[1] || *end || value > UINT32_MAX ||
        !valid_set(selected_set)) {
        fprintf(
            stderr,
            "Set must be 30, 60, or 99\n"
        );

        return EXIT_FAILURE;
    }

    if (prctl(PR_SET_L2_ISOLATION, 1UL, 0UL, 0UL, 0UL) == -1) {
        perror("prctl");
        return 1;
    }

    // pin_to_cpu(VICTIM_CPU);

    fprintf(
        stderr,
        "Victim PID %d running on CPU%d\n",
        getpid(),
        sched_getcpu()
    );

    fprintf(
        stderr,
        "Victim will access L2 set %u\n",
        selected_set
    );

    int exit_status = EXIT_FAILURE;
    l2_evsets victim_set = {0};
    if (l2_evsets_create(&victim_set, &selected_set, 1, 1, POOL_MAX_PAGES) == -1) {
        perror("l2_evsets_create");
        goto disable_isolation;
    }
    l2_line *victim_line = victim_set.sets[0].head;

    ipc_state *ipc = connect_ipc();
    if (!ipc) {
        l2_evsets_destroy(&victim_set);
        goto disable_isolation;
    }

    char command;
    int received;

    while ((received = receive_command(ipc, &command)) > 0) {
        if (command == 1) {
            l2_victim_touch(victim_line);
        }

        send_done(ipc);
    }

    send_done(ipc);
    disconnect_ipc(ipc);

    l2_evsets_destroy(&victim_set);
    exit_status = received == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

disable_isolation:
    printf("Disabling L2 isolation...\n");

    if (prctl(PR_SET_L2_ISOLATION, 0UL, 0UL, 0UL, 0UL) == -1) {
        perror("prctl");
        return 1;
    }

    fprintf(stderr, "Victim finished.\n");

    return exit_status;
}
