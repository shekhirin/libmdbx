# Async API Migration Log

## Goal

Keep the existing blocking MDBX public API intact and add a separate async API
that can cover the same operations. The first implementation slice exposes an
executor object with explicit operation handles; each executor owns one worker
thread and runs submitted operations in FIFO order.

This first API layer is intentionally conservative. It moves MDBX transaction
and cursor work off the caller thread while preserving existing transaction
ownership rules. Future performance work should add deeper async storage
integration and parallel read execution, then adapt benchmarks to submit work
through the new API.

## First Slice

Implemented public opaque handles:

- `MDBX_async`
- `MDBX_async_op`

Implemented generic operation flow:

- `mdbx_async_create()`
- `mdbx_async_destroy()`
- `mdbx_async_env()`
- `mdbx_async_submit()`
- `mdbx_async_poll()`
- `mdbx_async_wait()`
- `mdbx_async_op_release()`

Implemented benchmark-relevant typed wrappers:

- transaction begin, commit, abort, read reset, read renew
- DBI open
- get, put, delete
- cursor open, cursor reset, cursor renew, cursor get, cursor close

The typed `put`, `get`, and `del` wrappers copy key/data bytes needed by the
worker before enqueueing. The first `put` wrapper rejects `MDBX_RESERVE` and
`MDBX_MULTIPLE`, because those modes require caller-managed in-place memory.

## Current Limits

- Operations submitted to one executor execute serially on one worker thread.
- The API is async from the caller's point of view, but storage internals still
  use the current blocking MDBX operations inside the worker.
- Cursor get keeps caller-provided `MDBX_val` descriptor objects live until the
  operation completes; returned value lifetime follows the normal MDBX cursor
  rules.
- The ioarena benchmarks still use the blocking API. They have not yet been
  ported to the async API, so current benchmark numbers measure the explicit
  I/O backend work, not async API throughput.
- `ut_and_examples/async-api-bench.c` is an in-tree public-API benchmark for
  parallel read operations. It is intentionally not a deterministic pass/fail
  CTest gate.

## Benchmark Baseline

Machine-local ioarena lazy-mode logs already in the workspace show the current
explicit `io_uring` run is still slower than the earlier mapped/no-map baseline.
The earlier baseline below averages the available `mapped` and `nomap` lazy
logs. The current explicit rows use the latest `default` and `forced-nomap`
lazy logs generated with `MDBX_EXPLICIT_IO_BACKEND=io_uring`.

| phase | earlier mapped avg | current explicit default | ratio | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 840.375 ops/s | 0.813 | 1089.750 ops/s | 867.217 ops/s | 0.796 |
| crud | 55.675 Kops/s | 33.345 Kops/s | 0.599 | 59.787 Kops/s | 37.195 Kops/s | 0.622 |
| iterate | 26.143 Mops/s | 23.934 Mops/s | 0.916 | 25.827 Mops/s | 21.916 Mops/s | 0.849 |
| get | 279.409 Kops/s | 222.955 Kops/s | 0.798 | 274.039 Kops/s | 237.945 Kops/s | 0.868 |
| delete | 66.398 Kops/s | 40.005 Kops/s | 0.603 | 69.067 Kops/s | 42.249 Kops/s | 0.612 |

The current forced/default ratios inside the explicit `io_uring` run are:

- batch: 1.032
- crud: 1.115
- iterate: 0.916
- get: 1.067
- delete: 1.056

## Async Read Benchmark

Added `mdbx_async_api_bench`, which seeds a small database and compares three
public C API read paths:

- one blocking thread with one read transaction
- multiple blocking pthread readers, each with its own read transaction
- multiple async executors, each with its own read transaction, with a window of
  in-flight `mdbx_async_get()` calls

First local result using the default benchmark size:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           768.977 Kops/s
blocking parallel get         367.762 Kops/s
async parallel get            418.734 Kops/s
async/blocking parallel         1.139
async/blocking serial           0.545
```

Shorter forced no-map/tiny-cache run for smoke:

```text
async-api-bench items=20000 ops=30000 workers=4 window=64
blocking serial get           762.418 Kops/s
blocking parallel get         400.639 Kops/s
async parallel get            429.438 Kops/s
async/blocking parallel         1.072
async/blocking serial           0.563
```

The normal run shows the async executor path can beat this benchmark's blocking
pthread-parallel path for many submitted reads, but the result is not stable
enough to make it a gate. The next optimization target is lowering per-operation
allocation, key-copy, and condpair signaling overhead so async can also compete
with hot serial reads and no-map runs.

Operation allocation/release checkpoint:

- async operation handles now keep small inline key/data copies, avoiding heap
  allocation for common short payloads including the benchmark's 8-byte keys
- each executor keeps a bounded spare list for completed operation handles
- operation release no longer signals the completion condition variable; worker
  completion already wakes waiters, and destroy does not wait for handle release

Five-run default benchmark average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           771.548 Kops/s
blocking parallel get         396.309 Kops/s
async parallel get            429.108 Kops/s
async/blocking parallel         1.083
async/blocking serial           0.556
```

Three-run forced no-map/tiny-cache average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           720.980 Kops/s
blocking parallel get         394.609 Kops/s
async parallel get            397.655 Kops/s
async/blocking parallel         1.008
async/blocking serial           0.552
```

The allocation/release cleanup moves the default async read path slightly above
the previous ~419 Kops/s single-run result and keeps no-map near blocking
parallel parity, but it still does not challenge hot serial reads. The next
performance target is reducing per-operation condition-variable lock/unlock
traffic, likely through batched wait/release or a lower-overhead completion
queue.

## Validation

Completed for this checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `cmake --build @cmake-ninja-build`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K @cmake-ninja-build/mdbx_async_api_smoke`: passed
- `make -f GNUmakefile mdbx_async_api_smoke_nommap_tinycache`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional read-reuse and benchmark checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K @cmake-ninja-build/mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.139
- `make -f GNUmakefile mdbx_async_api_bench_run MDBX_ASYNC_BENCH_OPS=30000`: passed, async/blocking-parallel ratio 1.114
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- `MDBX_ASYNC_BENCH_OPS=30000 LD_LIBRARY_PATH=@cmake-ninja-build MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.072
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.067

Additional allocation/release checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 5-run average async/blocking-parallel ratio 1.083
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 3-run average async/blocking-parallel ratio 1.008
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_async_api_bench_run MDBX_ASYNC_BENCH_OPS=30000`: passed, async/blocking-parallel ratio 1.137
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
