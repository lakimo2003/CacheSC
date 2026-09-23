#ifndef L2_IPC_H
#define L2_IPC_H

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define IPC_SHM_NAME "/cachesc_l2_ipc"

_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "The shared initialization flag requires lock-free atomics");

typedef struct {
    pthread_mutex_t mutex;
    sem_t ready_sem;
    sem_t go_sem;
    sem_t done_sem;
    char command;
    int stop;
    atomic_int initialized;
} ipc_state;

#endif
