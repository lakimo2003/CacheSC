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

#include <cachesc.h>
#include <sys/prctl.h>

#define PR_SET_L2_ISOLATION 0x4C320001


#define VICTIM_CPU 1

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
        exit(EXIT_FAILURE);
    }

    ipc = mmap(NULL, sizeof(*ipc), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ipc == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    while (!ipc->initialized) {
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

// returns 0 when the attacker sends the stop command
// returns 1 when one command was received
static int receive_command(ipc_state *ipc, char *command)
{
    sem_wait(&ipc->go_sem);

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

    uint32_t selected_set =
        (uint32_t) strtoul(argv[1], NULL, 10);

    if (!valid_set(selected_set)) {
        fprintf(
            stderr,
            "Set must be 30, 60, or 99\n"
        );

        return EXIT_FAILURE;
    }

    if (prctl(PR_SET_L2_ISOLATION, 1, 0, 0, 0) == -1) {
        perror("prctl");
        return 1;
    }

    // pin_to_cpu(VICTIM_CPU);

    fprintf(
        stderr,
        "Victim PID %d running on CPU%d\n",
        getpid(),
        VICTIM_CPU
    );

    fprintf(
        stderr,
        "Victim will access L2 set %u\n",
        selected_set
    );

    cache_ctx *ctx = get_cache_ctx(L2);

    cacheline *victim_line =
        prepare_victim(ctx, selected_set);

    ipc_state *ipc = connect_ipc();

    char command;

    while (receive_command(ipc, &command)) {
        if (command == 1) {
            victim(victim_line);
        }

        send_done(ipc);
    }

    send_done(ipc);
    disconnect_ipc(ipc);

    release_victim(ctx, victim_line);
    release_cache_ctx(ctx);

    printf("Disabling L2 isolation...\n");

    if (prctl(PR_SET_L2_ISOLATION, 0, 0, 0, 0) == -1) {
        perror("prctl");
        return 1;
    }

    fprintf(stderr, "Victim finished.\n");

    return EXIT_SUCCESS;
}
