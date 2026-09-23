# L2 attacker and victim migration

Both `demo/l2-attacker.c` and `demo/l2-victim.c` now use the implementation in
`l2/`. Their builds do not include the original root `src/` headers, link
`libcachesc`, or require the OpenSSL/Argon2 libraries used by other demos.

## Preserved experiment

- Attacker usage remains `l2-attacker [number-of-samples]`, defaulting to 10,000.
- Victim usage remains `l2-victim <set>`, accepting 30, 60, or 99.
- The attacker pins itself to CPU 0 and monitors all three sets.
- The victim's optional pin to CPU 1 remains commented out. Its startup message
  now reports the actual current CPU rather than claiming it is on CPU 1.
- The victim enables the same custom `PR_SET_L2_ISOLATION` operation before
  selecting its line, and disables it at shutdown.
- The shared-memory name remains `/cachesc_l2_ipc`. The ready/go/done semaphore
  handshake, mutex-protected command and stop fields, and command values stay
  the same. Both programs should be rebuilt and run together.
- For each sample, the attacker primes, sends command 0, waits, and probes;
  then it primes again, sends command 1, waits, and probes. Command 0 performs
  no victim-line access. Command 1 performs one eight-byte victim load after
  MFENCE. The victim acknowledges each round and the final stop request.
- Probe groups are visited in the old reverse group order: 99, 60, then 30.
  The new priming loop uses the same group order.
- The result report still prints baseline, with-touch, and difference averages
  for 30, 60, and 99, and predicts the set with the largest average difference.

## Replacement APIs

| Previous dependency | New implementation |
| --- | --- |
| `get_cache_ctx(L2)` | Compile-time geometry in `l2_config.h`. |
| `prepare_cache_set_ds()` | `l2_evsets_create()` for three independent rings. |
| `prime_rev()` | `l2_prime_set()` on each ring in reverse group order. |
| `probe()` and measurement extraction | `l2_probe_set().ticks`, stored directly in a three-element array. |
| `prepare_measurement()` | `l2_prepare_measurement()` in `l2/src/util.c`. |
| `prepare_victim()` | `l2_evsets_create()` for one set and one line. |
| `victim()` | `l2_victim_touch()`. |
| Cache/victim allocation release | `l2_evsets_destroy()`. |
| Original affinity helper | Existing rewritten `pin_to_cpu()` in `l2/src/util.c`. |

The attacker requests `L2_ASSOCIATIVITY` distinct lines per set: currently 12,
or 36 nodes total. The victim requests one node. Each process independently
bounds its candidate pool to 4,096 system pages, or 16 MiB with 4 KiB pages.
It retains the original pool mapping until shutdown; individual nodes are
never freed separately.

Measurements are 64-bit TSC differences. The legacy output label `cycles` is
retained for compatibility; it does not mean instantaneous CPU core cycles.
The new fences, per-set ring layout, line ordering, and current 12-way
configuration differ from the old library. The experiment and reporting are
preserved, but numerical timings are not expected to be identical. Calibrate
the rewritten experiment on its target instead of transferring old thresholds.

## Supporting changes

`l2_util.h` declares the rewritten affinity function and new warm-up helper.
Warm-up performs approximately two seconds of active work using the monotonic
clock, warms timestamp reads, and finishes with CPUID. It preserves the intent
of the old frequency-based heuristic without assuming a 2.9 GHz processor.
Neither version guarantees a fixed CPU frequency.

`l2_ipc.h` contains the common IPC layout to keep the two programs consistent.
The initialization flag now uses lock-free acquire/release atomics. Semaphore
waits retry `EINTR`, including the victim command wait and attacker done waits.

Invalid, overflowing, or trailing-junk numeric arguments are rejected before
allocation or isolation changes. CPU affinity failures are reported. If victim
allocation or IPC connection fails after enabling isolation, it releases any
owned pool and attempts to disable isolation. If attacker warm-up fails after
connection, it sends the stop command and waits before releasing IPC resources.

## Build and run

Build the programs at their existing demo paths:

```sh
make -C demo l2
```

Start the attacker in one terminal:

```sh
./demo/l2-attacker 10000
```

Once it prints that it is waiting for the victim, start the victim in another:

```sh
./demo/l2-victim 30
```

Alternatively, `make l2` or `make -C l2 apps` builds the same sources as
`l2/build/l2-attacker` and `l2/build/l2-victim`. These targets are independent
of the repository's original library build. Bare root `make` retains its
existing behavior for the original library and is not the L2 build command.

The real experiment still requires usable pagemap frame numbers and a kernel
implementing the custom isolation operation. The programs report failure;
they do not silently disable isolation or substitute guessed physical sets.
The victim remains unpinned as in the existing program, so CPU placement and
the isolation policy remain part of the experimental setup to verify.

## Verification

`make -C l2 check` builds both real programs and runs:

1. Existing probe/load-width functional tests.
2. Existing bounded-pool and list-construction tests with synthetic PFNs.
3. New two-process integration tests compiling the actual demo sources with
   link-time wrappers for allocation, affinity, warm-up, and the custom prctl.

The integration tests use a private temporary file as the shared-memory
backing, leaving a live experiment's POSIX shared-memory name alone. Real
process-shared mutexes, semaphores, the probe assembly, and victim load run.
The tests check all three victim choices, alternating command sequences,
sample-count and output shape, readiness, stop acknowledgement, interrupted
waits, invalid arguments, allocation/connection failures, and isolation
disable paths. Temporary child processes are terminated on test timeout.

The GCC build, both existing test programs, and integration suite passed.
Compiler dependency output includes only the new `l2` project headers, and
the production binaries link only the system runtime, not the old CacheSC
library. Hardware eviction and prediction accuracy are not established by
these simulated integration tests; they require the actual experiment target.
