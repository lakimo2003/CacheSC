#define _GNU_SOURCE
#include "l2_evset.h"
#include "l2_ipc.h"
#include "l2_util.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static ipc_state *shared;
static int shared_fd = -1;

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_sem_wait(sem_t *);
int __real_sem_post(sem_t *);
void __real_l2_evsets_destroy(l2_evsets *);

static int failure(const char *name)
{
    const char *setting = getenv("L2_TEST_FAIL");
    return setting && strcmp(setting, name) == 0;
}

static int is_victim(void)
{
    return strcmp(getenv("L2_TEST_ROLE"), "victim") == 0;
}

static void trace(const char *format, ...)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s.trace", getenv("L2_TEST_DIR"),
                     getenv("L2_TEST_ROLE"));
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "a");
    assert(file != NULL);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    assert(fclose(file) == 0);
}

static void shared_path(char path[PATH_MAX], const char *name)
{
    assert(strcmp(name, IPC_SHM_NAME) == 0);
    int n = snprintf(path, PATH_MAX, "%s/ipc", getenv("L2_TEST_DIR"));
    assert(n > 0 && n < PATH_MAX);
}

/* File-backed shared memory isolates tests from a live demo's POSIX SHM name. */
int __wrap_shm_open(const char *name, int flags, mode_t mode)
{
    char path[PATH_MAX];
    shared_path(path, name);
    shared_fd = open(path, flags, mode);
    return shared_fd;
}

int __wrap_shm_unlink(const char *name)
{
    char path[PATH_MAX];
    shared_path(path, name);
    return unlink(path);
}

void *__wrap_mmap(void *address, size_t length, int prot, int flags,
                  int fd, off_t offset)
{
    void *result = __real_mmap(address, length, prot, flags, fd, offset);
    if (result != MAP_FAILED && fd >= 0 && fd == shared_fd &&
        length == sizeof(ipc_state))
        shared = result;
    return result;
}

int __wrap_sem_wait(sem_t *sem)
{
    static unsigned int interrupted;
    unsigned int bit = 0;
    if (shared) {
        if (sem == &shared->ready_sem) {
            trace("wait-ready\n");
            bit = 1;
        } else if (sem == &shared->done_sem) {
            bit = 2;
        } else if (sem == &shared->go_sem) {
            bit = 4;
        }
    }
    if (getenv("L2_TEST_EINTR") && bit && !(interrupted & bit)) {
        interrupted |= bit;
        errno = EINTR;
        return -1;
    }
    int result = __real_sem_wait(sem);
    if (result == 0 && shared && is_victim() && sem == &shared->go_sem) {
        if (shared->stop)
            trace("stop\n");
        else
            trace("command %d\n", shared->command);
    }
    return result;
}

int __wrap_sem_post(sem_t *sem)
{
    if (shared && is_victim()) {
        if (sem == &shared->ready_sem)
            trace("ready\n");
        else if (sem == &shared->done_sem)
            trace("done\n");
    }
    return __real_sem_post(sem);
}

int __wrap_pin_to_cpu(int cpu)
{
    trace("pin %d\n", cpu);
    assert(cpu == 0 && !is_victim());
    return 0;
}

int __wrap_l2_prepare_measurement(void)
{
    trace("warmup\n");
    if (failure("warmup")) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int __wrap_prctl(int option, ...)
{
    assert(option == 0x4C320001 && is_victim());
    va_list args;
    va_start(args, option);
    unsigned long enable = va_arg(args, unsigned long);
    assert(va_arg(args, unsigned long) == 0);
    assert(va_arg(args, unsigned long) == 0);
    assert(va_arg(args, unsigned long) == 0);
    va_end(args);
    trace("isolation %lu\n", enable);
    if (failure(enable ? "isolation-enable" : "isolation-disable")) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int __wrap_l2_evsets_create(l2_evsets *out, const uint32_t *ids, size_t count,
                            size_t lines, size_t max_pages)
{
    trace("create %zu %zu\n", count, lines);
    assert(max_pages == 4096);
    if (is_victim()) {
        assert(count == 1 && lines == 1);
    } else {
        assert(count == 3 && lines == L2_ASSOCIATIVITY);
        assert(ids[0] == 30 && ids[1] == 60 && ids[2] == 99);
    }
    if (failure("create")) {
        errno = EACCES;
        return -1;
    }

    out->set_count = count;
    out->pool_size = count * lines * sizeof(l2_line);
    out->pool = __real_mmap(NULL, out->pool_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    out->sets = calloc(count, sizeof(*out->sets));
    assert(out->pool != MAP_FAILED && out->sets != NULL);
    for (size_t s = 0; s < count; ++s) {
        trace("set %u\n", ids[s]);
        l2_line *nodes = (l2_line *)out->pool + s * lines;
        out->sets[s] = (l2_evset){ .id = ids[s], .count = lines, .head = nodes };
        for (size_t i = 0; i < lines; ++i) {
            nodes[i].prev = &nodes[(i + 1) % lines];
            nodes[i].next = &nodes[(i + lines - 1) % lines];
            nodes[i].set = (uint16_t)ids[s];
        }
    }
    return 0;
}

void __wrap_l2_evsets_destroy(l2_evsets *sets)
{
    trace("destroy\n");
    __real_l2_evsets_destroy(sets);
}
