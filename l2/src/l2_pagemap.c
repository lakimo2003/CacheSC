#define _POSIX_C_SOURCE 200809L
#include "l2_pagemap.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "64-bit file offsets are required");

int l2_pagemap_open(l2_pagemap *pm)
{
    if (!pm) {
        errno = EINVAL;
        return -1;
    }
    if (pm->fd != -1) {
        errno = EALREADY;
        return -1;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;

    pm->fd = fd;
    pm->page_size = (size_t)page_size;
    return 0;
}

int l2_pagemap_close(l2_pagemap *pm)
{
    if (!pm) {
        errno = EINVAL;
        return -1;
    }
    int fd = pm->fd;
    pm->fd = -1;
    pm->page_size = 0;
    return fd == -1 ? 0 : close(fd);
}

int l2_virtual_to_physical(const l2_pagemap *pm, uintptr_t virtual_addr,
                           uint64_t *physical_addr)
{
    if (!pm || pm->fd < 0 || pm->page_size == 0 || !physical_addr) {
        errno = EINVAL;
        return -1;
    }

    uint64_t vpn = virtual_addr / pm->page_size;
    if (vpn > (uint64_t)INT64_MAX / sizeof(uint64_t)) {
        errno = EOVERFLOW;
        return -1;
    }

    uint64_t entry;
    off_t offset = (off_t)(vpn * sizeof(entry));
    ssize_t bytes;
    do {
        bytes = pread(pm->fd, &entry, sizeof(entry), offset);
    } while (bytes == -1 && errno == EINTR);

    if (bytes == -1)
        return -1;
    if (bytes != (ssize_t)sizeof(entry)) {
        errno = EIO;
        return -1;
    }
    if (entry & (UINT64_C(1) << 62)) {
        errno = EAGAIN;
        return -1;
    }
    if (!(entry & (UINT64_C(1) << 63))) {
        errno = ENOENT;
        return -1;
    }

    uint64_t pfn = entry & ((UINT64_C(1) << 55) - 1);
    /* A zero PFN is ambiguous; reject it instead of inventing a mapping. */
    if (pfn == 0) {
        errno = EACCES;
        return -1;
    }

    uint64_t within_page = virtual_addr % pm->page_size;
    if (pfn > (UINT64_MAX - within_page) / pm->page_size) {
        errno = EOVERFLOW;
        return -1;
    }
    *physical_addr = pfn * pm->page_size + within_page;
    return 0;
}

int l2_physical_set(const l2_pagemap *pm, const void *ptr,
                    uint32_t line_size, uint32_t set_count, uint32_t *set)
{
    if (!set || line_size == 0 || set_count == 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t physical_addr;
    if (l2_virtual_to_physical(pm, (uintptr_t)ptr, &physical_addr) == -1)
        return -1;

    *set = (uint32_t)((physical_addr / line_size) % set_count);
    return 0;
}
