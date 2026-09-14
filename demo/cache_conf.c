/*
 * L2-only adaptation of src/cache.c, cache_types.h, asm.h, victim.c and util.c.
 * Original CacheSC: Copyright (C) 2020 Miro Haller.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * See ../COPYING for the license text.
 * Compile as C++: g++ -x c++ -std=c++11 -O2 -c demo/cache_conf.c
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cache_conf.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <new>
#include <random>
#include <sched.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using Config = L2_cache_conf;
using PageGroups = std::array<std::vector<CacheLine *>, Config::page_group_count>;

constexpr size_t max_page_attempts = 4096;
constexpr uint32_t collision_samples = 100;
// Original src/device_conf.h uses 30 cycles for L3 and 12 for L2.
constexpr uint32_t collision_extra_cycles = 30 - Config::hit_latency_cycles;

std::mt19937 &random_engine() {
    static std::mt19937 engine(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return engine;
}

CacheLine *page_start(CacheLine *line) {
    return reinterpret_cast<CacheLine *>(reinterpret_cast<uintptr_t>(line)
        & ~(uintptr_t(Config::page_size_bytes) - 1));
}

// Own every candidate, including rejected pages, until setup completes. Keeping
// rejects prevents the allocator from repeatedly handing back the same page.
struct FreePage {
    void operator()(CacheLine *page) const { std::free(page); }
};

struct CachePage {
    CacheLine lines[Config::lines_per_page];
};

class PagePool {
public:
    CacheLine *allocate(size_t page_count = 1) {
        if (pages.size() >= max_page_attempts) {
            throw std::runtime_error("L2 setup exhausted its page search limit; "
                "check cache geometry, available page groups and timing noise.");
        }
        void *memory = nullptr;
        const int error = posix_memalign(&memory, Config::page_size_bytes,
            page_count * Config::page_size_bytes);
        if (error) {
            throw std::system_error(error, std::generic_category(), "allocate L2 page");
        }
        CachePage *storage = new (memory) CachePage{};
        std::unique_ptr<CacheLine, FreePage> page(storage->lines);
        CacheLine *result = page.get();
        pages.push_back(std::move(page));
        return result;
    }

    void transfer_used_pages(const std::vector<CacheLine *> &lines) {
        for (auto &page : pages) {
            for (CacheLine *line : lines) {
                if (page.get() == page_start(line)) {
                    page.release();
                    break;
                }
            }
        }
    }

private:
    std::vector<std::unique_ptr<CacheLine, FreePage>> pages;
};

class PhysicalAddressReader {
public:
    PhysicalAddressReader() {
        if (sysconf(_SC_PAGESIZE) != Config::page_size_bytes) {
            throw std::runtime_error("L2 setup requires 4096-byte system pages.");
        }
        descriptor = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(), "open pagemap");
        }
    }

    ~PhysicalAddressReader() { close(descriptor); }
    PhysicalAddressReader(const PhysicalAddressReader &) = delete;
    PhysicalAddressReader &operator=(const PhysicalAddressReader &) = delete;

    uintptr_t translate(const void *address) const {
        uint64_t entry = 0;
        const uintptr_t virtual_address = reinterpret_cast<uintptr_t>(address);
        const off_t offset = (virtual_address / Config::page_size_bytes) * sizeof(entry);
        size_t bytes_read = 0;
        while (bytes_read < sizeof(entry)) {
            const ssize_t count = pread(descriptor,
                reinterpret_cast<char *>(&entry) + bytes_read,
                sizeof(entry) - bytes_read, offset + bytes_read);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::system_error(count < 0 ? errno : EIO,
                    std::generic_category(), "read pagemap entry");
            }
            bytes_read += static_cast<size_t>(count);
        }
        if (!(entry & (uint64_t(1) << 63)) || (entry & (uint64_t(1) << 62))) {
            throw std::runtime_error("The requested page is not resident in memory.");
        }
        const uint64_t frame_number = entry & ((uint64_t(1) << 55) - 1);
        if (!frame_number) {
            throw std::runtime_error("Physical page numbers are hidden in pagemap. "
                "Exact L2 set selection needs permission to read them. "
                "L2SetMapping::timing_groups provides only local set labels.");
        }
        return frame_number * Config::page_size_bytes
            + virtual_address % Config::page_size_bytes;
    }

private:
    int descriptor;
};

PageGroups find_physical_groups(PagePool &pool,
    const std::vector<uint32_t> &set_indices, size_t pages_per_group)
{
    PhysicalAddressReader addresses;
    PageGroups groups;
    std::array<bool, Config::page_group_count> needed{};
    size_t remaining = 0;
    for (uint32_t set : set_indices) {
        const size_t group = set / Config::lines_per_page;
        if (!needed[group]) {
            needed[group] = true;
            remaining += pages_per_group;
        }
    }
    while (remaining) {
        CacheLine *page = pool.allocate();
        const size_t group = l2_set_index(addresses.translate(page)) / Config::lines_per_page;
        if (needed[group] && groups[group].size() < pages_per_group) {
            groups[group].push_back(page);
            --remaining;
        }
    }
    return groups;
}

void append_line(CacheLine *&head, CacheLine *line) {
    if (!head) {
        line->next = line;
        line->prev = line;
        head = line;
        return;
    }
    line->next = head;
    line->prev = head->prev;
    head->prev->next = line;
    head->prev = line;
}

void replace_line(CacheLine *old_line, CacheLine *replacement) {
    replacement->next = old_line->next;
    replacement->prev = old_line->prev;
    old_line->next->prev = replacement;
    old_line->prev->next = replacement;
}

__attribute__((always_inline)) inline uint32_t measure_list(CacheLine *head) {
    CacheLine *current = head;
    uint32_t elapsed;
    // One assembly block keeps all pointer loads inside the timed interval.
    __asm__ volatile(
        "xor %%eax, %%eax\n\t"
        "cpuid\n\t"
        "rdtsc\n\t"
        "mov %%eax, %%r8d\n\t"
        "lfence\n\t"
        "1: mov %c[prev](%[current]), %[current]\n\t"
        "cmp %[head], %[current]\n\t"
        "jne 1b\n\t"
        "rdtscp\n\t"
        "sub %%r8d, %%eax\n\t"
        "mov %%eax, %[elapsed]\n\t"
        "xor %%eax, %%eax\n\t"
        "cpuid\n\t"
        : [current] "+&r"(current), [elapsed] "=&r"(elapsed)
        : [head] "r"(head), [prev] "i"(offsetof(CacheLine, prev))
        : "rax", "rbx", "rcx", "rdx", "r8", "cc", "memory");
    return elapsed;
}

// Retain src/cache.c's replacement-based collision test. It compares a primed
// ring against the same ring with one line replaced, from every starting point.
bool causes_collision(CacheLine *candidate, CacheLine *head, size_t line_count) {
    if (line_count <= Config::ways) {
        return false;
    }
    size_t slower_starts = 0;
    CacheLine *current = head;
    do {
        uint32_t baseline = UINT32_MAX;
        for (uint32_t sample = 0; sample < collision_samples; ++sample) {
            touch_victim_line(candidate);
            prime_l2_cache(current);
            baseline = std::min(baseline, measure_list(current));
        }
        replace_line(current, candidate);
        uint64_t total = 0;
        for (uint32_t sample = 0; sample < collision_samples; ++sample) {
            prime_l2_cache(candidate);
            total += measure_list(candidate);
        }
        if (total >= (uint64_t(baseline) + collision_extra_cycles) * collision_samples) {
            ++slower_starts;
        }
        replace_line(candidate, current);
        current = current->next;
    } while (current != head);
    return slower_starts >= line_count - Config::ways;
}

struct CandidatePage {
    CacheLine *lines;
    bool group_identified;
};

void identify_timing_group(CacheLine *candidate, CacheLine *head,
    std::vector<CandidatePage> &accepted_pages, PageGroups &groups,
    size_t &identified_group_count)
{
    if (identified_group_count == groups.size()) {
        return;
    }
    std::vector<CandidatePage *> matching_pages;
    for (auto &page : accepted_pages) {
        if (page.group_identified) {
            continue;
        }
        CacheLine *original = page.lines;
        replace_line(original, candidate);
        CacheLine *test_head = original == head ? candidate : head;
        const bool matches = causes_collision(original, test_head, accepted_pages.size());
        replace_line(candidate, original);
        if (matches) {
            matching_pages.push_back(&page);
        }
    }
    if (matching_pages.size() == Config::ways) {
        for (CandidatePage *page : matching_pages) {
            groups[identified_group_count].push_back(page->lines);
            page->group_identified = true;
        }
        ++identified_group_count;
    }
}

PageGroups find_timing_groups(PagePool &pool) {
    if (sysconf(_SC_PAGESIZE) != Config::page_size_bytes) {
        throw std::runtime_error("L2 setup requires 4096-byte system pages.");
    }
    PageGroups groups;
    std::array<CacheLine *, Config::lines_per_page> lists_by_page_offset{};
    std::vector<CandidatePage> accepted_pages;
    size_t identified_group_count = 0;
    size_t repeated_collisions = 0;
    const size_t required_pages = Config::page_group_count * Config::ways;

    while (accepted_pages.size() < required_pages) {
        // Vary the allocation size to break repeated page-allocation patterns.
        CacheLine *candidate = pool.allocate(repeated_collisions >= 3 ? 2 : 1);
        if (repeated_collisions >= 3) {
            repeated_collisions = 0;
        }
        size_t colliding_offsets = 0;
        for (size_t offset = 0; offset < Config::lines_per_page; ++offset) {
            if (causes_collision(candidate + offset, lists_by_page_offset[offset],
                                 accepted_pages.size())) {
                ++colliding_offsets;
            }
        }
        if (colliding_offsets == Config::lines_per_page) {
            ++repeated_collisions;
            identify_timing_group(candidate, lists_by_page_offset[0], accepted_pages,
                                  groups, identified_group_count);
        } else {
            repeated_collisions = 0;
            for (size_t offset = 0; offset < Config::lines_per_page; ++offset) {
                append_line(lists_by_page_offset[offset], candidate + offset);
            }
            accepted_pages.push_back({candidate, false});
        }
    }
    while (identified_group_count < groups.size()) {
        identify_timing_group(pool.allocate(), lists_by_page_offset[0], accepted_pages,
                              groups, identified_group_count);
    }
    return groups;
}

std::vector<uint32_t> validate_sets(const uint32_t *set_indices, size_t set_count) {
    if (!set_indices || !set_count || set_count > Config::set_count) {
        throw std::invalid_argument("Provide between 1 and 512 L2 set indices.");
    }
    std::array<bool, Config::set_count> seen{};
    for (size_t i = 0; i < set_count; ++i) {
        if (set_indices[i] >= Config::set_count || seen[set_indices[i]]) {
            throw std::invalid_argument("L2 set indices must be unique and in range.");
        }
        seen[set_indices[i]] = true;
    }
    return std::vector<uint32_t>(set_indices, set_indices + set_count);
}

CacheLine *build_list(const PageGroups &groups, std::vector<uint32_t> set_indices,
    size_t lines_per_set, PagePool &pool)
{
    std::shuffle(set_indices.begin(), set_indices.end(), random_engine());
    std::vector<CacheLine *> ordered_lines;
    ordered_lines.reserve(set_indices.size() * lines_per_set);
    for (uint32_t set : set_indices) {
        const auto &pages = groups[set / Config::lines_per_page];
        if (pages.size() < lines_per_set) {
            throw std::runtime_error("L2 group has too few pages.");
        }
        const size_t first = ordered_lines.size();
        for (size_t way = 0; way < lines_per_set; ++way) {
            CacheLine *line = pages[way] + set % Config::lines_per_page;
            line->set_index = static_cast<uint16_t>(set);
            line->last_probe_cycles = 0;
            line->is_first_in_set = false;
            line->is_last_in_set = false;
            ordered_lines.push_back(line);
        }
        std::shuffle(ordered_lines.begin() + first, ordered_lines.end(), random_engine());
        ordered_lines[first]->is_first_in_set = true;
        ordered_lines.back()->is_last_in_set = true;
    }
    const size_t count = ordered_lines.size();
    for (size_t i = 0; i < count; ++i) {
        ordered_lines[i]->next = ordered_lines[(i + 1) % count];
        ordered_lines[i]->prev = ordered_lines[(i + count - 1) % count];
    }
    pool.transfer_used_pages(ordered_lines);
    return ordered_lines.front();
}

CacheLine *prepare_lines(const uint32_t *set_indices, size_t set_count,
    size_t lines_per_set, L2SetMapping mapping)
{
    const auto selected_sets = validate_sets(set_indices, set_count);
    PagePool pool;
    PageGroups groups;
    switch (mapping) {
    case L2SetMapping::physical_addresses:
        groups = find_physical_groups(pool, selected_sets, lines_per_set);
        break;
    case L2SetMapping::timing_groups:
        groups = find_timing_groups(pool);
        break;
    default:
        throw std::invalid_argument("Unknown L2 set mapping mode.");
    }
    return build_list(groups, selected_sets, lines_per_set, pool);
}

} // namespace

uintptr_t physical_address_of(const void *address) {
    if (!address) {
        throw std::invalid_argument("Cannot translate a null address.");
    }
    return PhysicalAddressReader().translate(address);
}

uint16_t physical_l2_set_index(const void *address) {
    return l2_set_index(physical_address_of(address));
}

CacheLine *prepare_l2_sets(const uint32_t *set_indices, size_t set_count,
    L2SetMapping mapping)
{
    return prepare_lines(set_indices, set_count, Config::ways, mapping);
}

CacheLine *prepare_l2_cache(L2SetMapping mapping) {
    std::array<uint32_t, Config::set_count> sets{};
    for (uint32_t set = 0; set < Config::set_count; ++set) {
        sets[set] = set;
    }
    return prepare_l2_sets(sets.data(), sets.size(), mapping);
}

void release_l2_cache(CacheLine *head) {
    if (!head) {
        return;
    }
    // Collect every unique page before freeing: multiple sets can share a page.
    std::array<CacheLine *, Config::page_group_count * Config::ways> pages{};
    size_t page_count = 0;
    CacheLine *current = head;
    do {
        CacheLine *page = page_start(current);
        if (std::find(pages.begin(), pages.begin() + page_count, page)
            == pages.begin() + page_count) {
            pages[page_count++] = page;
        }
        current = current->next;
    } while (current != head);
    for (size_t i = 0; i < page_count; ++i) {
        std::free(pages[i]);
    }
}

CacheLine *prepare_l2_victim(uint32_t set_index, L2SetMapping mapping) {
    return prepare_lines(&set_index, 1, 1, mapping);
}

void release_l2_victim(CacheLine *line) {
    release_l2_cache(line);
}

void collect_l2_measurements(const CacheLine *head, uint32_t *measurements,
    size_t measurement_count)
{
    if (!measurements || measurement_count < Config::set_count) {
        throw std::invalid_argument("Measurement buffer needs one entry per L2 set.");
    }
    std::fill(measurements, measurements + Config::set_count, 0);
    if (!head) {
        return;
    }
    const CacheLine *current = head;
    do {
        if (current->is_first_in_set) {
            measurements[current->set_index] = current->last_probe_cycles;
        }
        current = current->prev;
    } while (current != head);
}

void pin_process_to_cpu(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= CPU_SETSIZE) {
        throw std::invalid_argument("CPU index is out of range.");
    }
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu_index, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) < 0) {
        throw std::system_error(errno, std::generic_category(), "set CPU affinity");
    }
}

void warm_up_l2_measurements(uint32_t milliseconds) {
    const auto end = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(milliseconds);
    do {
        for (unsigned int i = 0; i < 200; ++i) {
            __asm__ volatile("rdtsc" : : : "rax", "rdx", "memory");
        }
    } while (std::chrono::steady_clock::now() < end);
    l2_detail::serialize_cpu();
}

void fill_random_bytes(unsigned char *bytes, size_t byte_count) {
    if (!bytes && byte_count) {
        throw std::invalid_argument("Random byte buffer is null.");
    }
    std::uniform_int_distribution<unsigned int> byte(0, 255);
    for (size_t i = 0; i < byte_count; ++i) {
        bytes[i] = static_cast<unsigned char>(byte(random_engine()));
    }
}

void print_banner(const char *message) {
    const char *border = "################################################################";
    std::printf("%s\n# %-60.60s #\n%s\n", border, message, border);
    std::fflush(stdout);
}

void print_l2_measurements(const uint32_t *measurements, size_t sample_count) {
    if (!measurements && sample_count) {
        throw std::invalid_argument("Measurement buffer is null.");
    }
    // Retain the sample format consumed by scripts/parser.py.
    for (size_t sample = 0; sample < sample_count; ++sample) {
        std::printf("#### Sample number %zu:\n", sample);
        for (size_t set = 0; set < Config::set_count; ++set) {
            std::printf("%3u ", measurements[sample * Config::set_count + set]);
        }
        std::putchar('\n');
    }
    std::fflush(stdout);
}
