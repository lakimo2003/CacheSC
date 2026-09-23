#define _GNU_SOURCE
#include "l2_types.h"
#include "l2_probe.h"
#include "l2_victim.h"

#include <assert.h>
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Stable symbols also make inspecting the optimized instruction stream easy. */
__attribute__((noinline)) l2_probe_result inspect_probe(l2_line *head)
{
    return l2_probe_set(head);
}

__attribute__((noinline)) l2_line *inspect_prime(l2_line *head)
{
    return l2_prime_set(head);
}

__attribute__((noinline)) uint64_t inspect_byte(const void *ptr)
{
    return read_one_byte(ptr);
}

__attribute__((noinline)) uint64_t inspect_qword(const void *ptr)
{
    return read_eight_bytes(ptr);
}

__attribute__((noinline)) void inspect_victim(const void *ptr)
{
    l2_victim_touch(ptr);
}

static void test_load_width(void)
{
    _Alignas(CACHELINE_SIZE) unsigned char bytes[2 * CACHELINE_SIZE];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (unsigned char)(i + 1);

    const size_t offsets[] = {0, CACHELINE_SIZE - 8, CACHELINE_SIZE - 4};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        size_t offset = offsets[i];
        uint64_t expected;
        memcpy(&expected, bytes + offset, sizeof(expected));
        assert(inspect_byte(bytes + offset) == bytes[offset]);
        assert(inspect_qword(bytes + offset) == expected);
    }

    unsigned char before[sizeof(bytes)];
    memcpy(before, bytes, sizeof(bytes));
    inspect_victim(bytes);
    assert(memcmp(before, bytes, sizeof(bytes)) == 0);
}

static void test_traversal(void)
{
    const size_t count = 2 * L2_ASSOCIATIVITY;
    l2_line *nodes = aligned_alloc(CACHELINE_SIZE, count * sizeof(*nodes));
    assert(nodes != NULL);
    memset(nodes, 0, count * sizeof(*nodes));

    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    l2_line *guard = mmap(NULL, (size_t)page_size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED);

    for (size_t i = 0; i < count; ++i) {
        nodes[i].prev = i + 1 == count ? guard : &nodes[i + 1];
        nodes[i].next = guard;
        nodes[i].cycles = 12345;
    }
    assert(inspect_prime(nodes) == &nodes[L2_ASSOCIATIVITY]);
    assert(inspect_prime(&nodes[L2_ASSOCIATIVITY]) == guard);

    l2_probe_result first = inspect_probe(nodes);
    assert(first.next == &nodes[L2_ASSOCIATIVITY]);
    assert(first.ticks > 0);
    l2_probe_result second = inspect_probe(first.next);
    assert(second.next == guard);
    assert(second.ticks > 0);

    /* A cycle verifies repeated invocation without confusing next and prev. */
    nodes[L2_ASSOCIATIVITY - 1].prev = nodes;
    for (unsigned int i = 0; i < 1000; ++i) {
        assert(inspect_prime(nodes) == nodes);
        l2_probe_result sample = inspect_probe(nodes);
        assert(sample.next == nodes);
        assert(sample.ticks > 0);
    }
    for (size_t i = 0; i < count; ++i)
        assert(nodes[i].cycles == 12345);

    assert(munmap(guard, (size_t)page_size) == 0);
    free(nodes);
}

int main(void)
{
    unsigned int a, b, c, d;
    if (!__get_cpuid(0, &a, &b, &c, &d) ||
        b != 0x756e6547 || d != 0x49656e69 || c != 0x6c65746e ||
        !__get_cpuid(0x80000001, &a, &b, &c, &d) || !(d & (1u << 27))) {
        fputs("SKIP: these probe tests require Intel x86-64 with RDTSCP.\n",
              stderr);
        return 77;
    }

    test_load_width();
    test_traversal();
    puts("PASS: load widths, exact prev traversal, guarded endpoint, and cycles");
    puts("These are functional checks, not an L2 eviction experiment.");
    return 0;
}
