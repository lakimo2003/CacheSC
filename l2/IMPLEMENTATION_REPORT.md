# Bounded L2 set builder

Implemented the five requested allocation and construction steps in
`src/l2_evset.c`, with the public interface in `include/l2_evset.h`. The new
builder uses your `l2_line` type and existing pagemap implementation. It has no
dependency on the original repository-root `src/` directory or `libcachesc`.

## What was implemented

1. **Bounded pool allocation and initialization.** The caller supplies
   `max_pages`. The builder obtains the system page size from the pagemap
   context, checks the size calculation, and maps one private anonymous region
   of `max_pages * page_size` bytes. It writes zeros to the entire region before
   translating it. The pool is page aligned and contains cache-line-aligned
   positions for `l2_line` objects.
2. **Physical classification.** The builder calls `l2_virtual_to_physical()`
   once for each page examined. Within that page it derives each candidate's
   physical address by adding its offset, then applies the same modulo set
   calculation as `l2_physical_set()`. It never assumes physical contiguity
   between different virtual pages.
3. **Selection of requested sets only.** It stores candidates only for the
   supplied set IDs. Each set stops accepting candidates when its requested
   count is reached. Duplicate physical line addresses are rejected. Scanning
   stops immediately when all requested sets are complete, or fails with
   `ENOSPC` when the bounded pool is exhausted.
4. **Independent circular lists.** After selection, each set receives its own
   doubly linked list. `prev` visits candidates in discovery order; `next` is
   the inverse. Every list contains exactly `lines_per_set` nodes, with its
   head stored alongside the set ID and count. The current attacker requests
   `L2_ASSOCIATIVITY`, which is 12. A victim can request one self-linked node.
5. **Explicit ownership and cleanup.** The owner retains the original mapping
   address, byte length, metadata array, and number of translated pages.
   `l2_evsets_destroy()` unmaps the complete pool, frees metadata, and clears
   the owner. Creation failures release partial resources and leave the
   caller's output unchanged.

The complete pool is initialized and retained even if candidate scanning ends
early. The bound covers candidate-pool bytes; the small metadata and temporary
candidate arrays are additional allocations proportional to the requested
sets and lines, not to the whole cache.

## Public API

```c
int l2_evsets_create(l2_evsets *out,
                     const uint32_t *set_ids,
                     size_t set_count,
                     size_t lines_per_set,
                     size_t max_pages);

void l2_evsets_destroy(l2_evsets *sets);
```

Initialize the owner with `{0}`. Creation returns zero on success or `-1` with
`errno` on failure. It rejects an already populated owner, invalid/duplicate
set IDs, zero counts, counts above associativity, and pool-size overflow.
The result preserves the caller's set order. After success, metadata and the
pool belong to this one owner: do not copy it or individually free a head.

## Attacker usage

```c
#include "l2_evset.h"
#include "l2_probe.h"
#include <stdio.h>

uint32_t ids[] = {30, 60, 99};
l2_evsets attacker = {0};

if (l2_evsets_create(&attacker, ids, 3, L2_ASSOCIATIVITY, 4096) == -1) {
    perror("l2_evsets_create");
    return 1;
}

l2_time_t measurements[3];
for (size_t i = 0; i < attacker.set_count; ++i)
    (void)l2_prime_set(attacker.sets[i].head);

/* Send the victim command and wait for completion here. */

for (size_t i = 0; i < attacker.set_count; ++i)
    measurements[i] = l2_probe_set(attacker.sets[i].head).ticks;

/* Consume measurements; repeat rounds while keeping attacker alive. */
l2_evsets_destroy(&attacker);
```

These statements belong inside your application function. They build 36
selected nodes across three rings. With 4 KiB pages, the example's 4,096-page
limit retains a 16 MiB candidate pool. This is a chosen bound, not a promise
that every allocation can supply the requested mappings. `measurements[i]`
corresponds to `attacker.sets[i].id`, not the numerical set ID as array index.

## Victim usage

```c
#include "l2_evset.h"
#include "l2_victim.h"
#include <stdio.h>

uint32_t selected_set = 30;
l2_evsets victim = {0};

if (l2_evsets_create(&victim, &selected_set, 1, 1, 1024) == -1) {
    perror("l2_evsets_create");
    return 1;
}

l2_line *victim_line = victim.sets[0].head;
/* In the victim-access command branch: */
l2_victim_touch(victim_line);

/* After the command loop ends: */
l2_evsets_destroy(&victim);
```

The victim creates its own pool in its own process. The one-node ring is only
a convenient common representation; touching it does not require following
any links. Do not use a one-node victim ring as a 12-line attacker eviction set.
If the experiment uses the custom L2 isolation operation, enable that mode
before allocating/classifying the victim's pages, as in the existing demo.

## Files and commands

| File | Purpose |
| --- | --- |
| `include/l2_evset.h` | Set records, pool owner, creation and destruction API. |
| `src/l2_evset.c` | Allocation, translation, selection, linking, and cleanup. |
| `examples/build_sets.c` | Runnable three-set example using real pagemap. |
| `tests/test_evset.c` | Deterministic allocation and failure-path checks. |
| `Makefile` | Builds the example and both functional test programs. |

From the repository root:

```sh
make -C l2 check
./l2/build/build_sets
```

`demo/l2-attacker.c` and `demo/l2-victim.c` now use this builder and the new
`l2` measurement functions. See [DEMO_MIGRATION.md](DEMO_MIGRATION.md) for the
application changes, preserved protocol, build commands, and validation.

## Validation and limits

The GCC build and both functional tests passed. Both tests also passed with
Clang AddressSanitizer, UndefinedBehaviorSanitizer, and leak checking. The
sanitizer run required execution outside the tracing sandbox because
LeakSanitizer cannot operate under ptrace. The builder tests supply
simulated physical mappings through link-time replacements of the pagemap
functions, while using real mmap allocation and cleanup. Checks cover the
requested IDs and ordering, 12 distinct lines per set, alignment, exact ring
length and reciprocal links, early stopping, one-line victim allocation,
bounded exhaustion, duplicate PFNs, translation failures, rejected inputs,
repeated cleanup, and release of mappings on failure.

The real-pagemap example compiled and ran but reported `Permission denied`.
Successful physical construction was therefore not verified in this session.
Linux restricts disclosure of physical frame numbers, so merely being able to
open pagemap is insufficient. The builder propagates that failure and does not
substitute virtual addresses or fabricate set membership.
[Linux pagemap documentation](https://docs.kernel.org/admin-guide/mm/pagemap.html).

The lists are candidate eviction sets under the configured physical modulo
indexing assumption. Geometry, that indexing assumption, CPU placement, L1
effects, and actual eviction reliability still need validation on the target.
Ordering is deterministic; no extra shuffling or empirical collision search
is performed. Allocation is setup work and belongs outside timed measurements.

Physical translations are snapshots. Retaining the pool preserves virtual
storage, not a guarantee against physical-page migration or later remapping.
Result stores and IPC also affect cache state, even though they are outside
the probe's timed assembly block.
