#define _GNU_SOURCE
#include "l2_evset.h"
#include "l2_pagemap.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* Link-time replacements keep allocator tests independent of PFN privileges. */
static uintptr_t first_page;
static size_t translations, closes, page_size;
static size_t fail_translation;
static int duplicate_frames, deny_open;

static void reset_fake(void)
{
    first_page = 0;
    translations = closes = 0;
    fail_translation = SIZE_MAX;
    duplicate_frames = deny_open = 0;
    long size = sysconf(_SC_PAGESIZE);
    assert(size > 0);
    page_size = (size_t)size;
}

int l2_pagemap_open(l2_pagemap *pm)
{
    if (deny_open) {
        errno = EACCES;
        return -1;
    }
    pm->fd = 42;
    pm->page_size = page_size;
    return 0;
}

int l2_pagemap_close(l2_pagemap *pm)
{
    if (pm->fd != -1)
        ++closes;
    pm->fd = -1;
    pm->page_size = 0;
    return 0;
}

static uint64_t physical_for(uintptr_t address)
{
    size_t offset = address - first_page;
    size_t page = offset / page_size;
    size_t generation = duplicate_frames ? 1 : page / 2 + 1;
    return (uint64_t)generation * L2_SETS * CACHELINE_SIZE +
           (page % 2) * page_size + offset % page_size;
}

int l2_virtual_to_physical(const l2_pagemap *pm, uintptr_t address,
                           uint64_t *physical)
{
    assert(pm->fd == 42);
    assert(address % page_size == 0);
    if (translations == 0)
        first_page = address;
    assert(address == first_page + translations * page_size);
    ++translations;

    const unsigned char *bytes = (const unsigned char *)address;
    for (size_t i = 0; i < page_size; ++i)
        assert(bytes[i] == 0);

    if (translations == fail_translation) {
        errno = EACCES;
        return -1;
    }
    *physical = physical_for(address);
    return 0;
}

static void assert_empty(const l2_evsets *sets)
{
    assert(sets->pool == NULL && sets->pool_size == 0);
    assert(sets->sets == NULL && sets->set_count == 0);
    assert(sets->pages_scanned == 0);
}

static void assert_unmapped(uintptr_t address)
{
    unsigned char resident;
    errno = 0;
    assert(mincore((void *)address, page_size, &resident) == -1);
    assert(errno == ENOMEM);
}

static void test_requested_sets(void)
{
    reset_fake();
    const uint32_t ids[] = {99, 30, 60};
    const size_t max_pages = 2 * L2_ASSOCIATIVITY + 5;
    l2_evsets owner = {0};
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY, max_pages) == 0);
    assert(owner.pool_size == max_pages * page_size);
    assert((uintptr_t)owner.pool % page_size == 0);
    assert(owner.set_count == 3);
    assert(translations == 2 * L2_ASSOCIATIVITY);
    assert(owner.pages_scanned == translations && closes == 1);

    for (size_t s = 0; s < owner.set_count; ++s) {
        l2_evset *set = &owner.sets[s];
        assert(set->id == ids[s] && set->count == L2_ASSOCIATIVITY);
        l2_line *line = set->head;
        uint64_t physical[L2_ASSOCIATIVITY];
        for (size_t i = 0; i < set->count; ++i) {
            assert(i == 0 || line != set->head);
            assert((uintptr_t)line >= (uintptr_t)owner.pool);
            assert((uintptr_t)line + sizeof(*line) <=
                   (uintptr_t)owner.pool + owner.pool_size);
            assert((uintptr_t)line % CACHELINE_SIZE == 0);
            physical[i] = physical_for((uintptr_t)line);
            assert((physical[i] / CACHELINE_SIZE) % L2_SETS == ids[s]);
            for (size_t j = 0; j < i; ++j)
                assert(physical[j] != physical[i]);
            assert(line->set == ids[s]);
            assert(line->next->prev == line && line->prev->next == line);
            line = line->prev;
        }
        assert(line == set->head);
    }

    void *saved_pool = owner.pool;
    errno = 0;
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY, max_pages) == -1);
    assert(errno == EALREADY && owner.pool == saved_pool);
    l2_evsets_destroy(&owner);
    assert_empty(&owner);
    assert_unmapped((uintptr_t)saved_pool);
    l2_evsets_destroy(&owner);
    l2_evsets_destroy(NULL);
}

static void test_victim(void)
{
    reset_fake();
    uint32_t id = 30;
    l2_evsets owner = {0};
    assert(l2_evsets_create(&owner, &id, 1, 1, 8) == 0);
    assert(translations == 1);
    assert(owner.sets[0].count == 1);
    l2_line *line = owner.sets[0].head;
    assert(line->prev == line && line->next == line);
    l2_evsets_destroy(&owner);
}

static void test_failures(void)
{
    const uint32_t ids[] = {30, 60, 99};
    l2_evsets owner = {0};

    reset_fake();
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY,
                            2 * L2_ASSOCIATIVITY - 1) == -1);
    assert(errno == ENOSPC && translations == 2 * L2_ASSOCIATIVITY - 1);
    assert(closes == 1);
    assert_empty(&owner);
    assert_unmapped(first_page);

    reset_fake();
    duplicate_frames = 1;
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY,
                            2 * L2_ASSOCIATIVITY + 5) == -1);
    assert(errno == ENOSPC);
    assert_empty(&owner);
    assert_unmapped(first_page);

    reset_fake();
    fail_translation = 3;
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY, 32) == -1);
    assert(errno == EACCES && translations == 3 && closes == 1);
    assert_empty(&owner);
    assert_unmapped(first_page);

    reset_fake();
    deny_open = 1;
    assert(l2_evsets_create(&owner, ids, 3, L2_ASSOCIATIVITY, 32) == -1);
    assert(errno == EACCES && translations == 0 && closes == 0);
    assert_empty(&owner);
}

static void test_arguments(void)
{
    reset_fake();
    const uint32_t id = 30, invalid = L2_SETS, duplicates[] = {30, 30};
    l2_evsets owner = {0};
    assert(l2_evsets_create(NULL, &id, 1, 12, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, NULL, 1, 12, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, &id, 0, 12, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, &id, 1, 0, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, &id, 1, 1, 0) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, &invalid, 1, 1, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, duplicates, 2, 1, 32) == -1 && errno == EINVAL);
    assert(l2_evsets_create(&owner, &id, 1, L2_ASSOCIATIVITY + 1, 32) == -1 &&
           errno == EINVAL);
    assert(l2_evsets_create(&owner, &id, 1, 1, SIZE_MAX) == -1 &&
           errno == EOVERFLOW);
    assert(translations == 0 && closes == 1);
    assert_empty(&owner);
}

int main(void)
{
    test_requested_sets();
    test_victim();
    test_failures();
    test_arguments();
    puts("PASS: requested sets, distinct lines, circular links, bounds, cleanup");
    puts("Physical mappings were simulated; hardware eviction is not tested.");
    return 0;
}
