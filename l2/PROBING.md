# Load width and the L2 probe

The implementation is in `include/l2_probe.h`; `include/l2_victim.h` provides
the victim touch. Both use only the new `l2` files. Build and run the functional
checks with `make -C l2 check` from the repository root.

## When one byte versus eight bytes matters

For an aligned ordinary memory address, both widths touch the same cache line.
The wider load does not fill eight cache lines or reserve eight ways. Thus, if
the victim only needs to touch one selected line, the address and cache state
are usually more relevant than the scalar load width.

There are specific reasons to preserve eight bytes:

- The old victim did an eight-byte load; keeping it removes a variable when
  comparing old and new experiments. The new `l2_victim_touch()` does this.
- A load near a boundary can span two lines. At offset 60 in a 64-byte line,
  one byte stays in that line, whereas eight bytes cover offsets 60-67. That
  touches a second line and potentially a second set. Crossing a page boundary
  can also require a second translation or fault if the other page is unmapped.
- Scalar instructions, dependencies, and interactions with earlier stores can
  differ. Line identity alone is not a promise of identical timings.
- A pointer chase must recover a complete pointer. On this x86-64 target that
  is eight bytes; loading one byte cannot replace `cursor = cursor->prev`.

Cache-line split behavior is discussed in the
[Intel optimization manual](https://cdrdv2-public.intel.com/814198/248966-Optimization-Reference-Manual-V1-049.pdf).

Your node is one line and is now declared cache-line aligned. Its `next` pointer
is at byte 0 and `prev` is at byte 8. Both eight-byte fields fit within the line.
For dynamic allocation, use `aligned_alloc(CACHELINE_SIZE, size)` with size a
multiple of the alignment, or appropriately aligned mmap/page storage. Ordinary
`malloc` does not promise this extended alignment. A struct's size alone never
established the address alignment of the old allocation.

To check any proposed address `p` and width `w`, use:

```c
size_t offset = (uintptr_t)p % CACHELINE_SIZE;
bool crosses_line = w > CACHELINE_SIZE - offset;
```

Also ensure every byte in the range belongs to readable, initialized memory.
The explicit assembly load uses a character-array memory operand so the helper
does not introduce an incorrectly aligned or type-aliased C `uint64_t` access.

## How to test whether width affects your experiment

First inspect the instructions using:

```sh
objdump -d -Mintel --disassemble=inspect_byte l2/build/test_probe
objdump -d -Mintel --disassemble=inspect_qword l2/build/test_probe
objdump -d -Mintel --disassemble=inspect_victim l2/build/test_probe
```

Expect a byte load for `inspect_byte`, a QWORD load for `inspect_qword`, and
MFENCE followed by a QWORD load for `inspect_victim`. This checks what the
compiler emitted; it does not establish equal experimental outcomes.

For the actual experiment, compare three victim conditions: no load, one-byte
load, and eight-byte load. Use the same virtual address and keep the same
pre-load MFENCE in both access conditions. Change only the victim's load helper;
the attacker probe must remain the same eight-byte pointer chase in all cases.
Keep CPU placement, mappings, set membership, scheduling/IPC, priming order, and
sample filtering identical.

Choose each condition before priming. Balance their counts and shuffle their
order, or rotate through them, to avoid always running one width after another.
Prime afresh before every trial. Record raw per-set probe ticks after each
trial. Repeat across batches and independently constructed eviction sets.

Compare the distributions, median, and upper quantiles, together with the
victim-touch minus no-touch effect. If classifying victim activity is the goal,
compare false positives and false negatives on held-out trials with a threshold
chosen separately. Decide beforehand what difference would matter to your
experiment. Similar means alone do not establish equivalence, and the minimum
warm-load timing is not the L2 side-channel measurement.

There is no universal acceptable tick difference. If changing width alters
detection quality or its uncertainty beyond your chosen tolerance, preserve
the width and investigate why. If it does not, report that the choice was
insensitive on the tested target, with the tested addresses and setup.

## Why the probe is one assembly statement

Separate timer and load statements can be correct if their dependencies and
barriers are right. One assembly statement is useful when the exact instruction
stream matters: the compiler cannot insert its own spills, loop bookkeeping,
or unrelated instructions among the instructions in that statement. GCC does
not promise adjacent `asm volatile` statements will remain adjacent.
[GCC extended assembly documentation](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html).

The block still needs hardware ordering. It is not atomic against interrupts
and does not prevent preemption, cache prefetching, or page faults.

`l2_probe_set()` consists of:

```text
LFENCE
RDTSC; assemble/save the 64-bit start value in a register
LFENCE
    MOVQ prev(cursor), cursor      repeated L2_ASSOCIATIVITY times
RDTSCP
LFENCE
assemble the 64-bit stop value and subtract start
```

The timer orders loads according to Intel's RDTSC/RDTSCP rules. It does not
measure completion of arbitrary stores. The start bookkeeping and fence
contribute overhead, and the result is in TSC ticks, not necessarily current
core cycles. Do not compare it directly with `L2_ACCESS_TIME`.
[Intel instruction reference](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf).

`.rept` expands at assembly time, so 12 configured ways become 12 MOVQ
instructions, with no runtime counter or loop branch between the timestamps.
The `offsetof(l2_line, prev)` operand obtains the field displacement from the C
layout. There is no generated header and no dependency on the old `src/` tree.

`+&r` makes cursor a read/write register operand that cannot overlap unrelated
inputs still needed by the block. `=&r` and `=&a` declare outputs overwritten
early. RDX, RCX, flags, and memory effects are declared to the compiler. Register
outputs keep measurement stores after the end timestamp. The start value is
owned by this block instead of depending on a register surviving separate
statements.

## Where the dependent pointer chase lives

The conceptual C operation is:

```c
for (unsigned int i = 0; i < L2_ASSOCIATIVITY; ++i)
    cursor = cursor->prev;
```

The new probe implements it directly in assembly. Each MOVQ needs the address
returned by the preceding MOVQ, establishing a serial data dependency between
loads. The direction is `prev` because the old L2 probe traversed `prev`.
`read_one_byte(cursor)` loads data from a node but never advances the pointer.

The builder in `src/l2_evset.c` selects distinct physical lines and links one
ring per requested set, in discovery order. Any additional randomization must
stay within each set and happen during setup, not while measuring. Merely
assigning `node->set = 30` does not give it that physical set. See
[IMPLEMENTATION_REPORT.md](IMPLEMENTATION_REPORT.md) for the builder API and
examples using these independent rings.

Two layouts work with the current API:

- One circular list per set: after exactly N loads, the returned `next` pointer
  is the same as the initial head.
- A combined list with N consecutive nodes per set in `prev` order: after N
  loads, `next` is the first node of the following group. That node is not read
  by the previous group's probe.

For a combined list, the central attacker operations are:

```c
/* heads/groups and group order are established during setup. */
l2_line *cursor = first_group;
for (size_t i = 0; i < group_count; ++i)
    cursor = l2_prime_set(cursor);

/* Signal victim, then wait for completion using your IPC. */

cursor = first_group;
for (size_t i = 0; i < group_count; ++i) {
    l2_probe_result sample = l2_probe_set(cursor);
    measurements[set_order[i]] = sample.ticks;
    cursor = sample.next;
}
```

`measurements` must hold 64-bit `l2_time_t` values. Returning and using
`sample.next` exposes the last pointer as a result and advances to the next
group. The `volatile` assembly also prevents removal of the loads even if a
caller discards that pointer. The probe never reads metadata or writes timing
results into the eviction nodes. Their existing 32-bit `cycles` field is unused
by this API.

Result-buffer writes are outside each group's timed region but can still
perturb groups probed afterwards. Keep the ordering and buffer arrangement
consistent across conditions and check for interference. The same applies to
IPC between priming and probing. This is a measurement primitive, not a promise
that the surrounding experiment has no interference.

`l2_prime_set()` follows the same direction as the probe. A dependent pass warms
the selected lines; sufficient occupancy and eviction reliability still need
measurement on the target. It cannot force every probe load to be an L2 hit:
L1 residency must be accounted for even in an L2-focused experiment.

In the victim command handler, use `l2_victim_touch(victim_line)` for the touch
condition and perform the same IPC response for the no-touch condition. It
preserves the old MFENCE plus eight-byte load. Both demo executables now use
the new allocator and primitives; see [DEMO_MIGRATION.md](DEMO_MIGRATION.md).

## What the checks establish

`tests/test_probe.c` checks the explicit load values, unchanged victim memory,
exact traversal length, the `prev` direction, repeated cycles, and propagation
of the final pointer. A PROT_NONE endpoint catches an accidental extra load.
It does not enforce a timing threshold: timing varies by machine and noise.

Inspect the optimized probe with:

```sh
objdump -d -Mintel --disassemble=inspect_probe l2/build/test_probe
```

Expect exactly `L2_ASSOCIATIVITY` dependent QWORD loads between RDTSC and RDTSCP,
with no calls, stack accesses, stores, or loop branches in that interval.

The functional test uses ordinary aligned memory, not physically congruent
eviction sets. It needs no pagemap privilege and makes no claim of successful
L2 eviction. Verify the actual CPU geometry and address-index function before
building eviction sets. The current configuration uses 4,096 sets and 12 ways;
check it against the CPU and execution environment used for each experiment.
