/*
 * L2-only adaptation of CacheSC, Copyright (C) 2020 Miro Haller.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * See ../COPYING for the license text.
 *
 * This header and cache_conf.c are C++11 code for Linux/x86-64.
 * Compile both your migrated demo and cache_conf.c with g++ -x c++.
 */
#ifndef CACHE_CONF_H
#define CACHE_CONF_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "The L2 measurement code requires Linux on x86-64."
#endif

class L2_cache_conf {
public:
    // These values describe the original demo's CPU; match them to your CPU.
    static constexpr uint32_t line_size_bytes = 64;
    static constexpr uint32_t set_count = 512;
    static constexpr uint32_t ways = 8;
    static constexpr uint32_t hit_latency_cycles = 12;
    static constexpr uint32_t line_count = set_count * ways;
    static constexpr uint32_t set_size_bytes = line_size_bytes * ways;
    static constexpr uint32_t cache_size_bytes = set_count * set_size_bytes;

    static constexpr uint32_t page_size_bytes = 4096;
    static constexpr uint32_t lines_per_page = page_size_bytes / line_size_bytes;
    static constexpr uint32_t page_group_count = set_count / lines_per_page;
};

struct alignas(L2_cache_conf::line_size_bytes) CacheLine {
    CacheLine *next;
    CacheLine *prev;
    uint16_t set_index;
    bool is_first_in_set;
    bool is_last_in_set;
    // Entire set's probe time, stored in its first line. Includes timer overhead.
    uint32_t last_probe_cycles;
    char padding[L2_cache_conf::line_size_bytes - 2 * sizeof(CacheLine *)
        - sizeof(uint16_t) - 2 * sizeof(bool) - sizeof(uint32_t)];
};

static_assert(sizeof(CacheLine) == L2_cache_conf::line_size_bytes,
              "Each CacheLine must occupy exactly one hardware cache line.");
static_assert(L2_cache_conf::ways == 8, "The probe assembly below loads eight ways.");
static_assert(L2_cache_conf::set_count > 0 && L2_cache_conf::set_count <= 65536 &&
              (L2_cache_conf::set_count & (L2_cache_conf::set_count - 1)) == 0,
              "Set count must be a power of two and fit in set_index.");
static_assert(L2_cache_conf::set_count % L2_cache_conf::lines_per_page == 0,
              "A page must span a whole group of consecutive sets.");

// physical_addresses gives actual set numbers, shared in meaning across processes.
// timing_groups gives locally numbered groups: set 99 in two allocations may differ.
enum class L2SetMapping { physical_addresses, timing_groups };

// This assumes the original CPU's simple physical-address set indexing.
constexpr uint16_t l2_set_index(uintptr_t physical_address) {
    return (physical_address / L2_cache_conf::line_size_bytes)
        & (L2_cache_conf::set_count - 1);
}

// Setup functions throw std::runtime_error/std::system_error on failure and
// std::invalid_argument for invalid inputs. No heap-allocated config is needed.
uintptr_t physical_address_of(const void *address);
uint16_t physical_l2_set_index(const void *address);

// Each requested set gets eight distinct lines. Sets must be unique and nonempty.
// Keep the returned head for release_l2_cache; prime/probe may return other heads.
CacheLine *prepare_l2_sets(const uint32_t *set_indices, size_t set_count,
    L2SetMapping mapping = L2SetMapping::physical_addresses);
CacheLine *prepare_l2_cache(
    L2SetMapping mapping = L2SetMapping::physical_addresses);
void release_l2_cache(CacheLine *head);

// The victim owns a separate allocation containing one usable line.
CacheLine *prepare_l2_victim(uint32_t set_index,
    L2SetMapping mapping = L2SetMapping::physical_addresses);
void release_l2_victim(CacheLine *line);

void pin_process_to_cpu(int cpu_index);
void warm_up_l2_measurements(uint32_t milliseconds = 2000);
void fill_random_bytes(unsigned char *bytes, size_t byte_count);
void print_banner(const char *message);
void print_l2_measurements(const uint32_t *measurements, size_t sample_count);

namespace l2_detail {

__attribute__((always_inline)) inline void serialize_cpu() {
    uint32_t leaf = 0;
    __asm__ volatile("cpuid" : "+a"(leaf) : : "rbx", "rcx", "rdx", "memory");
}

__attribute__((always_inline)) inline CacheLine *probe_one_set(CacheLine *line) {
    CacheLine *next_set;
    uint32_t elapsed;

    // Seven steps reach this set's first line; the eighth fetches the next set's
    // pointer. All eight loads read the current set, without touching the next one.
    __asm__ volatile(
        "xor %%eax, %%eax\n\t"
        "cpuid\n\t"
        "rdtsc\n\t"
        "mov %%eax, %%r8d\n\t"
        "lfence\n\t"
        ".rept 7\n\t"
        "mov %c[prev](%[line]), %[line]\n\t"
        ".endr\n\t"
        "mov %c[prev](%[line]), %[next]\n\t"
        "rdtscp\n\t"
        "sub %%r8d, %%eax\n\t"
        "mov %%eax, %[elapsed]\n\t"
        "xor %%eax, %%eax\n\t"
        "cpuid\n\t"
        : [line] "+&r"(line), [next] "=&r"(next_set), [elapsed] "=&r"(elapsed)
        : [prev] "i"(offsetof(CacheLine, prev))
        : "rax", "rbx", "rcx", "rdx", "r8", "cc", "memory");

    line->last_probe_cycles = elapsed;
    return next_set;
}

} // namespace l2_detail

// Reverse priming preserves the L2 traversal order used in src/cache.h.
// Pass a first-in-set head; the returned last-in-set head is ready for probing.
__attribute__((always_inline)) inline CacheLine *prime_l2_cache(CacheLine *head) {
    if (!head) {
        return nullptr;
    }
    CacheLine *current = head;
    l2_detail::serialize_cpu();
    do {
        current = current->prev;
        __asm__ volatile("mfence" : : : "memory");
    } while (current != head);
    l2_detail::serialize_cpu();
    return current->prev;
}

// Pass the head returned by prime_l2_cache. Returns a first-in-set head suitable
// for the next prime. Collect measurements only after probing all selected sets.
__attribute__((always_inline)) inline CacheLine *probe_l2_cache(CacheLine *head) {
    if (!head) {
        return nullptr;
    }
    CacheLine *current = head;
    do {
        current = l2_detail::probe_one_set(current);
    } while (current != head);
    return current->next;
}

// Output needs set_count entries, indexed by set number. Unselected sets become 0.
void collect_l2_measurements(const CacheLine *head, uint32_t *measurements,
    size_t measurement_count = L2_cache_conf::set_count);

__attribute__((always_inline)) inline void touch_victim_line(const void *address) {
    __asm__ volatile("mfence\n\tmovq (%0), %%r10"
        : : "r"(address) : "r10", "memory");
}

#endif // CACHE_CONF_H
