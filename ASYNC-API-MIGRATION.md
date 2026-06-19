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

Transaction management checkpoint:

- added `mdbx_async_txn_break()` for marking async-owned transactions broken
  before abort
- added `mdbx_async_txn_park()` and `mdbx_async_txn_unpark()` for read
  transactions owned by the async executor worker
- added `mdbx_async_txn_refresh()` for refreshing read transactions on the
  worker thread
- added `mdbx_async_txn_info()` for transaction metadata, with caller-owned
  `MDBX_txn_info` storage valid until completion
- smoke coverage now checks async transaction info, park/unpark, refresh, and
  break-before-abort paths

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

Batch wait/release checkpoint:

- added `mdbx_async_wait_all()` and `mdbx_async_op_release_all()` for batches of
  operation handles from the same executor
- batch release validates and marks the batch under the executor lock, then
  recycles all completed handles in one pass
- smoke coverage now uses batch wait/release for put, get, delete, and mixed
  success/not-found get results
- `mdbx_async_api_bench` now waits and releases one submitted window at a time

Three-run default benchmark average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           759.867 Kops/s
blocking parallel get         398.038 Kops/s
async parallel get            430.763 Kops/s
async/blocking parallel         1.082
async/blocking serial           0.567
```

Three-run forced no-map/tiny-cache average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           776.398 Kops/s
blocking parallel get         391.460 Kops/s
async parallel get            405.979 Kops/s
async/blocking parallel         1.037
async/blocking serial           0.523
```

The batch API improves the public async surface for windowed callers and keeps
the benchmark above blocking pthread-parallel reads in both default and forced
no-map samples. The short GNUmake 30k sample remained noisy, ranging from
slightly below parity to above parity across runs, so the async read benchmark
still should not be made a deterministic gate.

Batch get checkpoint:

- added `mdbx_async_get_batch()` for submitting a whole get window as one async
  operation
- batch get stores per-key `mdbx_get()` return codes in a caller-provided result
  array and returns `MDBX_SUCCESS` for the async operation once the batch has
  run
- for throughput, batch get does not copy keys; the key descriptors and key
  bytes must remain valid until the batch operation completes
- smoke coverage now checks batch get for all-success reads and mixed
  success/not-found reads
- `mdbx_async_api_bench` now reports both the per-operation async get path and
  the single-operation batch get path

Five-run default benchmark average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           778.484 Kops/s
blocking parallel get         393.281 Kops/s
async parallel get            423.463 Kops/s
async batch parallel get      432.603 Kops/s
async/blocking parallel         1.077
async-batch/blocking parallel   1.100
async-batch/blocking serial     0.556
```

Three-run forced no-map/tiny-cache average after this checkpoint:

```text
async-api-bench items=20000 ops=200000 workers=4 window=64
blocking serial get           769.301 Kops/s
blocking parallel get         380.444 Kops/s
async parallel get            416.253 Kops/s
async batch parallel get      417.337 Kops/s
async/blocking parallel         1.094
async-batch/blocking parallel   1.097
async-batch/blocking serial     0.542
```

Batch get reduces async submission surface for windowed callers and is the best
default-sample async result so far, but the benchmark still appears dominated
by hot `mdbx_get()` work rather than operation submission overhead.

Batch mutation checkpoint:

- added `mdbx_async_put_batch()` and `mdbx_async_del_batch()` for submitting
  many mutations as one async operation
- batch put/delete use caller-owned key/value descriptors and payload bytes
  until completion, matching `mdbx_async_get_batch()` and avoiding per-item
  copies on the submission thread
- batch put/delete store per-item `mdbx_put()`/`mdbx_del()` return codes in a
  caller-provided result array and return `MDBX_SUCCESS` for the async operation
  once the batch has run
- smoke coverage now keeps single put/delete coverage while using batch put for
  the rest of the initial load and batch delete for the remaining every-third
  deletion set

This checkpoint broadens the async API surface for write-heavy callers. The
read benchmark is unchanged except for rebuild noise; a one-run default sample
after this change reported async-batch/blocking-parallel ratio 1.044.

Replace checkpoint:

- added `mdbx_async_replace()` for async replace/delete-with-old-value retrieval
  using the existing `mdbx_replace()` semantics
- key and new-data bytes are copied during submission; for duplicate-selection
  mode (`MDBX_CURRENT | MDBX_NOOVERWRITE`) the old-data selector bytes are also
  copied, while normal old-value output uses caller-owned descriptor/buffer
  storage until completion
- smoke coverage now replaces a committed value, verifies the returned old
  value, replaces it back, and verifies the temporary replacement value was
  returned

Cursor mutation checkpoint:

- added `mdbx_async_cursor_put()` and `mdbx_async_cursor_del()` so async cursors
  can mutate records as well as read them
- async cursor put copies key/data during submission and rejects
  `MDBX_RESERVE`/`MDBX_MULTIPLE`, matching the current table put wrapper
- smoke coverage now uses cursor put to add a write-transaction record and
  cursor delete to remove one of the keys later verified as missing

This narrows the remaining gap between cursor operations in the blocking API
and the additive async API. A one-run default read benchmark after this change
reported async-batch/blocking-parallel ratio 1.121.

Cursor deletion helper checkpoint:

- added `mdbx_async_cursor_delete_range()` for async range deletion between
  positioned cursors
- added `mdbx_async_cursor_bunch_delete()` for async neighboring-item deletion
  using `MDBX_bunch_action_t`
- smoke coverage now creates temporary named tables, deletes a positioned
  inclusive range with two cursors, deletes a suffix bunch from one cursor, and
  verifies affected counts and remaining table entries

Cursor navigation checkpoint:

- added `mdbx_async_cursor_distance()` for async distance measurement between
  two positioned cursor handles
- added `mdbx_async_cursor_scroll()` for async signed cursor movement using the
  existing `mdbx_cursor_scroll()` semantics
- smoke coverage now opens a second read cursor, measures first-to-last
  distance, scrolls the first cursor forward, verifies the current key/value,
  and re-measures the remaining distance

Cursor distribution checkpoint:

- added `mdbx_async_cursor_distribute()` for async range split setup using the
  existing `mdbx_cursor_distribute()` semantics
- the wrapper stores the caller-owned cursor array until completion, matching
  the blocking API's in-place cursor mutation model
- smoke coverage now opens three output cursors, distributes them across a
  first-to-last range, and verifies the expected split keys before closing the
  distributed cursors

Extended get checkpoint:

- added `mdbx_async_get_ex()` so async reads can return the same optional
  duplicate-count metadata as `mdbx_get_ex()`
- added `mdbx_async_get_equal_or_great()` for lower-bound lookups, preserving
  both exact-match `MDBX_SUCCESS` and greater-key `MDBX_RESULT_TRUE` results
- both wrappers copy submitted key bytes before enqueue; the equal-or-greater
  wrapper also copies the submitted data descriptor payload for duplicate-aware
  lower-bound searches
- smoke coverage now checks `mdbx_async_get_ex()` count/data return and both
  exact and greater-key `mdbx_async_get_equal_or_great()` result paths

Cursor batch read checkpoint:

- added `mdbx_async_cursor_get_batch()` so iteration-heavy callers can retrieve
  multiple key/value descriptors with one async operation
- the wrapper uses caller-owned `count` and `pairs` storage until completion,
  matching `mdbx_cursor_get_batch()` without copying returned database-owned
  key/value bytes
- smoke coverage now checks a partial cursor batch that completes with
  `MDBX_SUCCESS` and a full-table cursor batch that reports end-of-data with
  `MDBX_RESULT_TRUE`

Cursor batch benchmark checkpoint:

- `mdbx_async_api_bench` now also measures cursor-batch iteration using
  `mdbx_cursor_get_batch()` and `mdbx_async_cursor_get_batch()`
- the benchmark reports blocking serial cursor-batch, blocking pthread-parallel
  cursor-batch, async cursor-batch, and async-cursor/blocking ratios
- added `MDBX_ASYNC_BENCH_CURSOR_BATCH_PAIRS`, defaulting to 2048 pairs per
  cursor batch, so cursor iteration chunk size can be tuned independently from
  the point-get submission window
- `mdbx_cursor_get_batch()` now retains explicit-cache page references for
  returned batch descriptors before moving to another leaf page, preserving the
  public descriptor lifetime under tiny no-map caches

Cursor utility checkpoint:

- added `mdbx_async_cursor_count()` and `mdbx_async_cursor_count_ex()` for
  retrieving duplicate counts and nested duplicate-tree statistics from an
  async-owned cursor
- added async cursor state probes for EOF, first item, first duplicate, last
  item, and last duplicate; the async operation result preserves the underlying
  `MDBX_RESULT_TRUE`/`MDBX_RESULT_FALSE` return code
- smoke coverage now checks cursor count/count_ex and state transitions at the
  first and last cursor positions

DBI metadata checkpoint:

- added `mdbx_async_dbi_stat()` for table statistics, with caller-owned
  `MDBX_stat` output lifetime matching the other async wrappers
- added `mdbx_async_dbi_flags_ex()` for table flags/state and
  `mdbx_async_dbi_dupsort_depthmask()` for dupsort depth-mask inspection
- smoke coverage now verifies async DBI stats after the initial load, checks
  non-dupsort flags, and preserves the `MDBX_RESULT_TRUE` non-dupsort
  depth-mask result

DBI write-management checkpoint:

- added `mdbx_async_dbi_sequence()` for async sequence read/increment, including
  read-only transaction readback with `increment == 0`
- added `mdbx_async_drop()` for async table purge/delete using the existing
  `mdbx_drop()` semantics
- smoke coverage now sets `maxdbs`, creates a temporary named table, checks
  `drop(false)` empties it, checks `drop(true)` deletes it, and verifies the
  main DBI sequence increment persists after commit

Named DBI checkpoint:

- added `mdbx_async_dbi_open2()` for arbitrary-length table names and changed
  async DBI open dispatch to use the copied `MDBX_val` name path internally
- added `mdbx_async_dbi_rename()` and `mdbx_async_dbi_rename2()` for async table
  renames, copying the new name bytes before enqueue
- smoke coverage now creates a named table with an embedded-NUL name, renames it
  through both C-string and `MDBX_val` wrappers, verifies the payload survives,
  and then deletes the renamed table

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

Additional batch wait/release checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 3-run average async/blocking-parallel ratio 1.082
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 3-run average async/blocking-parallel ratio 1.037
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_async_api_bench_run MDBX_ASYNC_BENCH_OPS=100000`: passed, async/blocking-parallel ratio 1.024
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional batch get checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 5-run average async-batch/blocking-parallel ratio 1.100
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, 3-run average async-batch/blocking-parallel ratio 1.097
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_async_api_bench_run MDBX_ASYNC_BENCH_OPS=100000`: passed, async-batch/blocking-parallel ratio 1.097
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional batch mutation checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.044
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor mutation checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.121
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional extended get checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.096
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor batch read checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.115
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor batch benchmark checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.090, async-cursor/blocking-parallel ratio 1.015
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.050, async-cursor/blocking-parallel ratio 1.149
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor utility checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-cursor/blocking-parallel ratio 1.103
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional DBI metadata checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.076, async-cursor/blocking-parallel ratio 1.425
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional DBI write-management checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.060
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional named DBI checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.062, async-cursor/blocking-parallel ratio 1.608
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional replace checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async-batch/blocking-parallel ratio 1.049
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor deletion helper checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.064, async-cursor/blocking-parallel ratio 1.002
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor navigation checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed twice; samples reported async/blocking-parallel ratios 0.757 and 1.065, async-batch/blocking-parallel ratios 0.804 and 1.064, and async-cursor/blocking-parallel ratios 0.876 and 1.798
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor distribution checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed twice; samples reported async/blocking-parallel ratios 0.890 and 1.067, async-batch/blocking-parallel ratios 0.948 and 1.057, and async-cursor/blocking-parallel ratios 1.023 and 0.902
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional transaction management checkpoint:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed twice; samples reported async/blocking-parallel ratios 0.903 and 1.084, async-batch/blocking-parallel ratios 0.863 and 1.075, and async-cursor/blocking-parallel ratios 1.590 and 1.219
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional canary and estimate checkpoint:

- added `mdbx_async_canary_put()` and `mdbx_async_canary_get()` for async
  canary marker updates and reads; put copies the optional canary payload at
  submission time, while get uses caller-owned output storage until completion
- added `mdbx_async_estimate_distance()`, `mdbx_async_estimate_move()`, and
  `mdbx_async_estimate_range()` for async query-planning estimates; range
  submission copies optional bound descriptors and preserves NULL and
  `MDBX_EPSILON` sentinels, while move submission copies key/data bytes only
  for cursor operations that use input descriptors
- smoke coverage now checks write/read transaction canary visibility, cursor
  distance estimates, move estimates with returned key/value descriptors, full
  range estimates, copied bounded range estimates, and epsilon single-key
  estimates
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.071, async-batch/blocking-parallel ratio 1.075, and async-cursor/blocking-parallel ratio 1.755
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional cursor utility checkpoint:

- added async wrappers for unbound cursor creation, cursor user context
  set/get, bind/unbind, DBI lookup, cursor copy, cursor compare, disabling
  cursor order checks for tool use, and bulk transaction cursor release
- direct-return cursor helpers (`mdbx_cursor_get_userctx()`,
  `mdbx_cursor_dbi()`, and `mdbx_cursor_compare()`) store their return values
  in caller-provided outputs while the async operation result reports wrapper
  completion
- smoke coverage now creates an unbound cursor, verifies user context
  round-tripping, binds and reads through it, copies and compares cursor
  positions, unbinds/closes reusable cursors, and releases the final bound
  cursor through `mdbx_async_txn_release_all_cursors()`
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.037, async-batch/blocking-parallel ratio 1.046, and async-cursor/blocking-parallel ratio 0.982
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17

Additional environment metadata/control checkpoint:

- added async wrappers for environment statistics, environment information,
  sync, warmup, generic option get/set, flag get/set, path/fd lookup, geometry
  updates, environment user context set/get, and environment max-size helpers
- wrappers are bound to the executor's environment; transaction-scoped
  stat/info/warmup calls accept an optional transaction and let the underlying
  MDBX API enforce normal environment/transaction compatibility
- direct-return helpers such as environment user context and max-size queries
  store the returned value in caller-provided output storage while the async
  operation result reports wrapper completion
- smoke coverage now checks env context round-tripping, path/fd lookup,
  option set/get, flag toggling, geometry no-op update, sync/warmup,
  value-returning max-size helpers, and stat/info snapshots both outside and
  inside a read transaction
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.126, async-batch/blocking-parallel ratio 1.125, and async-cursor/blocking-parallel ratio 0.957

Additional transaction utility/lifecycle checkpoint:

- added async wrappers for transaction clone, user context set/get, env/flags/id
  lookup, straggler lag reporting, checkpoint, commit-and-embark-read, amend,
  and rollback
- direct-return helpers store the returned value in caller-provided output
  storage while the async operation result reports wrapper completion;
  `mdbx_async_txn_straggler()` stores the lag separately because the blocking
  API returns lag as a non-error integer
- smoke coverage now verifies transaction user context round-tripping,
  env/flags/id/straggler helpers, checkpoint preserving an earlier write,
  rollback discarding a later write, cloning a main-thread read transaction
  into the async worker, commit-embark-read returning a read transaction, and
  amending a read transaction into a write transaction
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.075, async-batch/blocking-parallel ratio 1.079, and async-cursor/blocking-parallel ratio 1.126

Additional DBI/data utility checkpoint:

- added async wrappers for DBI handle close, named-table enumeration, cached
  gets (`mdbx_cache_get()` and `mdbx_cache_get_SingleThreaded()`), and
  preservation-callback replacement via `mdbx_replace_ex()`
- cache-get wrappers copy the submitted key before enqueueing and write both
  the returned data descriptor and full `MDBX_cache_result_t` after the worker
  finishes; the caller-owned cache entry and output storage remain live until
  completion
- `mdbx_async_replace()` and `mdbx_async_replace_ex()` now share the same
  enqueue helper, preserving the existing async restrictions on `MDBX_RESERVE`
  and `MDBX_MULTIPLE` while letting the worker invoke the requested blocking
  replacement function
- smoke coverage now verifies async table enumeration callbacks, successful
  async DBI handle close after a committed open, async cached read refresh plus
  single-thread cache hit, and a dirty-page `replace_ex` preservation callback
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.089, async-batch/blocking-parallel ratio 1.088, and async-cursor/blocking-parallel ratio 0.649

Additional cursor/read utility checkpoint:

- added async wrappers for predicate cursor scans (`mdbx_cursor_scan()` and
  `mdbx_cursor_scan_from()`), dirty-page probing with `mdbx_is_dirty()`, and
  table key/data comparisons with `mdbx_cmp()` and `mdbx_dcmp()`
- cursor-scan predicates run on the async executor worker thread; scan-from
  copies the submitted key and optional value bytes before enqueueing and
  writes final key/value descriptors back when the scan result is not an error
- comparison wrappers copy both operands before enqueueing and store the signed
  comparator result in caller-provided output storage while the async operation
  result reports wrapper completion
- smoke coverage now verifies async clean-value dirty probing, key/data
  comparison ordering, predicate scan from first item, and predicate scan from
  a lower-bound key
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.063, async-batch/blocking-parallel ratio 1.073, and async-cursor/blocking-parallel ratio 1.417

Additional copy/backup checkpoint:

- added async wrappers for environment copies to pathname or file descriptor and
  transaction snapshot copies to pathname or file descriptor
- pathname wrappers copy the submitted destination path before enqueueing; file
  descriptor wrappers require the caller to keep the descriptor valid until the
  operation completes
- copy operations run on the async executor worker thread and preserve the
  blocking copy functions' snapshot, compaction, and flush flag semantics
- smoke coverage now verifies async environment copy and async transaction
  compact copy by reopening each copied database and reading expected records
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.060, async-batch/blocking-parallel ratio 1.100, and async-cursor/blocking-parallel ratio 0.660

Additional environment maintenance checkpoint:

- added async wrappers for reader-table enumeration and cleanup
  (`mdbx_reader_list()` and `mdbx_reader_check()`)
- added async wrappers for registering and unregistering the executor worker
  thread as an MDBX reader (`mdbx_thread_register()` and
  `mdbx_thread_unregister()`)
- added async HSR callback set/get wrappers and async write-transaction lock
  acquire/release wrappers; lock and unlock run on the same executor worker to
  preserve owner-thread semantics
- smoke coverage now verifies async HSR set/get/clear, worker reader
  registration/list/check/unregister, and a queued write-lock/unlock round trip
  before normal transaction work begins
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 0.991, async-batch/blocking-parallel ratio 1.028, and async-cursor/blocking-parallel ratio 1.572

Additional GC information checkpoint:

- added `mdbx_async_gc_info()` for asynchronous GC/page-usage summaries on an
  executor-owned transaction
- caller-owned `MDBX_gc_info_t` storage and the optional GC iterator
  callback/context remain live until completion; the iterator callback runs on
  the async executor worker thread
- smoke coverage now calls async GC info from the async read transaction, accepts
  the documented empty-GC result, verifies returned geometry, and exercises the
  optional iterator callback when GC spans are present
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean rerun of `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.072, async-batch/blocking-parallel ratio 1.076, and async-cursor/blocking-parallel ratio 0.800

Additional environment defrag checkpoint:

- added `mdbx_async_env_defrag()` for asynchronous environment defragmentation
  using the full blocking API parameter set
- optional progress callback/context and result storage remain live until
  completion; progress callbacks run on the async executor worker thread
- smoke coverage now runs a bounded async defrag after all async transactions are
  closed and verifies the result does not report an error stop reason
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.230, async-batch/blocking-parallel ratio 1.282, and async-cursor/blocking-parallel ratio 0.693

Additional environment check checkpoint:

- added `mdbx_async_env_chk()` for asynchronous database integrity checks on an
  opened async environment
- caller-provided check callbacks and context remain live until completion;
  check callbacks run on the async executor worker thread
- smoke coverage now runs a fast async environment check with btree and KV
  traversal skipped, verifies stage callbacks were invoked, and checks that the
  context is no longer active after completion
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.079, async-batch/blocking-parallel ratio 1.079, and async-cursor/blocking-parallel ratio 1.829

Additional recovery maintenance checkpoint:

- added `mdbx_async_env_turn_for_recovery()` for queuing recovery meta-page
  turns on an executor attached to a writable recovery-open environment
- smoke coverage now opens a separate recovery environment on the smoke
  database, creates an async executor for it, and verifies the queued recovery
  turn completes successfully
- pre-open recovery helpers still need a separate async factory/standalone
  operation shape because the current executor requires an already-open
  environment
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.065, async-batch/blocking-parallel ratio 1.089, and async-cursor/blocking-parallel ratio 1.426

Additional pre-open checkpoint:

- `mdbx_async_create()` can now create an unbound executor by passing a null
  environment pointer; env-bound async wrappers still require an executor with
  an open environment
- added async pre-open wrappers for `mdbx_preopen_snapinfo()` and
  `mdbx_env_open_for_recovery()`; pathname arguments are copied before
  enqueueing, while caller-owned output/environment handles remain live until
  completion
- smoke coverage now uses an unbound executor to run async preopen snapinfo and
  async recovery-open before creating an env-bound executor for the existing
  async recovery turn
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.062, async-batch/blocking-parallel ratio 1.084, and async-cursor/blocking-parallel ratio 1.045

Additional environment lifecycle checkpoint:

- added async wrappers for regular environment open, close, and delete
- `mdbx_async_env_open()` runs on an unbound executor and binds that executor to
  the opened environment on success; `mdbx_async_env_close_ex()` unbinds the
  executor when the environment handle is destroyed
- async recovery-open now follows the same binding rule, allowing the queued
  recovery turn to run on the same executor after recovery-open completes
- smoke coverage now opens the main environment asynchronously, closes it
  asynchronously, deletes it asynchronously, and verifies binding/unbinding
  transitions for both regular and recovery-open environments
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.068, async-batch/blocking-parallel ratio 1.067, and async-cursor/blocking-parallel ratio 1.756

Additional environment create checkpoint:

- added `mdbx_async_env_create()` for asynchronous environment-handle creation
  on an unbound executor; successful creation binds the executor to the new,
  still-closed environment
- `mdbx_async_env_open()` and async recovery-open now accept either an unbound
  executor or an executor already bound to the same environment by async create
- smoke coverage now creates the main environment asynchronously, sets
  `MDBX_opt_max_db` through the async option wrapper before open, then opens the
  same environment asynchronously; the recovery pre-open path now also creates,
  recovery-opens, turns, and closes through the same async executor
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.081, async-batch/blocking-parallel ratio 1.032, and async-cursor/blocking-parallel ratio 0.655

Additional named environment option checkpoint:

- added async wrappers for the named environment configuration helpers:
  `mdbx_async_env_set_syncbytes()`, `mdbx_async_env_get_syncbytes()`,
  `mdbx_async_env_set_syncperiod()`, `mdbx_async_env_get_syncperiod()`,
  `mdbx_async_env_set_mapsize()`, `mdbx_async_env_set_maxreaders()`,
  `mdbx_async_env_get_maxreaders()`, `mdbx_async_env_set_maxdbs()`, and
  `mdbx_async_env_get_maxdbs()`
- typed option getters now complete through a dedicated async worker case so
  `size_t`, `unsigned`, and `MDBX_dbi` outputs are written with the correct
  caller-visible type
- smoke coverage now uses named async max-DB and max-reader helpers before
  async open, and named sync-byte/sync-period helpers after open while keeping
  generic option-get coverage
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.066, async-batch/blocking-parallel ratio 1.079, and async-cursor/blocking-parallel ratio 0.647

Additional compatibility alias checkpoint:

- added async compatibility wrappers for the environment stat/info/sync
  shortcuts: `mdbx_async_env_stat()`, `mdbx_async_env_info()`,
  `mdbx_async_env_sync()`, `mdbx_async_env_sync_poll()`, and the deprecated
  `mdbx_async_env_get_maxkeysize()`
- added `mdbx_async_dbi_flags()` as the shortcut counterpart to
  `mdbx_dbi_flags()`; the shared async DBI flags worker now discards DBI state
  into a worker-local slot when callers do not provide a state output
- smoke coverage now exercises async sync shortcut/poll aliases and the
  state-discarding `mdbx_async_dbi_flags()` path
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.072, async-batch/blocking-parallel ratio 1.071, and async-cursor/blocking-parallel ratio 0.638

Additional custom-comparator DBI open checkpoint:

- added deprecated async custom-comparator DBI open wrappers:
  `mdbx_async_dbi_open_ex()` and `mdbx_async_dbi_open_ex2()`
- the async DBI-open worker now carries optional key/data comparator function
  pointers and calls `mdbx_dbi_open_ex2()`; existing `mdbx_async_dbi_open()`
  and `mdbx_async_dbi_open2()` continue to submit null comparators
- smoke coverage now opens custom-comparator named tables through both the
  C-string and arbitrary-length-name async wrappers, writes payloads, and
  verifies the resulting tables
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.024, async-batch/blocking-parallel ratio 1.027, and async-cursor/blocking-parallel ratio 1.141

Additional transaction extended-name alias checkpoint:

- added async naming-compatible aliases for extended transaction helpers:
  `mdbx_async_txn_begin_ex()`, `mdbx_async_txn_commit_ex()`,
  `mdbx_async_txn_abort_ex()`, and
  `mdbx_async_txn_release_all_cursors_ex()`
- the aliases forward to the existing async transaction implementations, whose
  signatures already carried context, latency, or cursor-count outputs
- smoke coverage now starts a transaction through the begin-ex alias with an
  initial user context, releases all cursors through the release-all-cursors-ex
  alias, and uses commit-ex/abort-ex aliases on later transaction transitions
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.122, async-batch/blocking-parallel ratio 1.152, and async-cursor/blocking-parallel ratio 0.836

Additional cursor close2 alias checkpoint:

- added `mdbx_async_cursor_close2()` as the naming-compatible async
  counterpart to `mdbx_cursor_close2()`
- the alias forwards to the existing `mdbx_async_cursor_close()` implementation,
  whose worker already calls `mdbx_cursor_close2()`
- smoke coverage now closes one copied cursor through the close2 alias while
  retaining existing coverage of `mdbx_async_cursor_close()`
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.057, async-batch/blocking-parallel ratio 1.081, and async-cursor/blocking-parallel ratio 1.297

Additional cursor transaction observer checkpoint:

- added `mdbx_async_cursor_txn()` as the async counterpart to
  `mdbx_cursor_txn()`, storing the cursor's transaction handle in
  caller-provided output storage while the async operation result reports
  wrapper completion
- smoke coverage now verifies that an async-bound utility cursor reports the
  expected transaction handle
- repaired the previous migration-log validation placement so the cursor-close2
  benchmark sample is recorded under the cursor-close2 checkpoint
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 1.029, async-batch/blocking-parallel ratio 1.063, and async-cursor/blocking-parallel ratio 1.077

Additional cache initialization checkpoint:

- an exact-name audit of exported blocking functions against exported async
  wrappers shows the remaining uncovered names are pure/global utility helpers
  (`mdbx_limits_*`, key conversion helpers, `mdbx_strerror*`, comparators,
  debug setup), platform aliases/macros, or special-case helpers such as
  `mdbx_env_resurrect_after_fork()` and `mdbx_env_chk_encount_problem()`
- added `mdbx_async_cache_init()` as the missing async counterpart for the
  cache-entry initializer in the CRUD/cache family, so cache callers can
  initialize entries through the same executor before using
  `mdbx_async_cache_get()` or `mdbx_async_cache_get_SingleThreaded()`
- smoke coverage now initializes a deliberately nonzero cache entry through
  `mdbx_async_cache_init()` and verifies that the worker reset all fields before
  issuing async cache reads
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench`: passed, async/blocking-parallel ratio 0.803, async-batch/blocking-parallel ratio 0.840, and async-cursor/blocking-parallel ratio 1.019

Additional async enqueue wakeup checkpoint:

- reduced async executor queue wakeups by signaling the worker only when
  enqueueing transitions an idle executor to queued work; if the worker is
  already active or the queue already contains work, the worker will observe the
  new operation without an extra condition-variable signal
- this keeps FIFO ordering and operation completion semantics unchanged while
  reducing wakeup overhead for windowed callers that submit many single
  `mdbx_async_get()` operations before waiting
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- three clean `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_bench` samples passed; average ratios were async/blocking-parallel 1.098, async-batch/blocking-parallel 1.116, and async-cursor/blocking-parallel 1.485

Additional async wait-all sequence checkpoint:

- added FIFO sequence counters to async executors and operation handles
- `mdbx_async_wait_all()` now finds the newest operation in the submitted
  handle set and waits for the executor's completed sequence to reach it,
  rather than rescanning every handle after every completion wakeup
- because each executor runs its queue FIFO, completion of the newest handle in
  a same-executor batch proves completion of all earlier handles in that batch;
  result collection still reads each operation's stored result after the wait
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.057, async-batch/blocking-parallel 1.081, and
  async-cursor/blocking-parallel 1.313
- larger `MDBX_ASYNC_BENCH_OPS=1000000` default sample passed with ratios
  async/blocking-parallel 1.100, async-batch/blocking-parallel 1.078, and
  async-cursor/blocking-parallel 1.531
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` sample passed with
  ratios async/blocking-parallel 1.010, async-batch/blocking-parallel 1.078,
  and async-cursor/blocking-parallel 1.016

Additional completion-signal suppression checkpoint:

- added a completion-waiter count to the async executor
- `mdbx_async_wait()`, `mdbx_async_wait_all()`, and draining
  `mdbx_async_destroy()` register themselves while actually waiting on the
  completion condition; the worker now signals completions only when such a
  waiter exists
- this removes unnecessary per-operation completion wakeups while callers are
  still filling an async window, without changing operation ordering or the
  visible wait/poll/release semantics
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.105, async-batch/blocking-parallel 1.090, and
  async-cursor/blocking-parallel 1.129
- larger `MDBX_ASYNC_BENCH_OPS=1000000` default sample passed with ratios
  async/blocking-parallel 1.111, async-batch/blocking-parallel 1.116, and
  async-cursor/blocking-parallel 1.580
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` sample passed with
  ratios async/blocking-parallel 1.035, async-batch/blocking-parallel 1.080,
  and async-cursor/blocking-parallel 1.522

Additional recycled-op reset checkpoint:

- added `async_op_prepare()` so recycled operation handles reset only their
  fixed header, payload-pointer, and copied-value descriptor fields instead of
  clearing the entire large operation object and opcode-specific union
- kept fresh operation allocation zero-initialized, while the spare-list hot
  path avoids the full-object `memset()` used by many single-op async reads
- made `mdbx_async_del()` initialize its optional-data flag explicitly so a
  recycled operation cannot inherit a stale `has_data` state when called with a
  null data selector
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.133, async-batch/blocking-parallel 1.133, and
  async-cursor/blocking-parallel 1.554
- larger `MDBX_ASYNC_BENCH_OPS=1000000` default sample passed with ratios
  async/blocking-parallel 1.105, async-batch/blocking-parallel 1.107, and
  async-cursor/blocking-parallel 1.436
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples were mixed:
  three-sample averages were async/blocking-parallel 0.984,
  async-batch/blocking-parallel 1.055, and async-cursor/blocking-parallel
  1.207
- longer forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=1000000` sample passed
  with ratios async/blocking-parallel 1.039, async-batch/blocking-parallel
  1.044, and async-cursor/blocking-parallel 1.921

Additional benchmark cursor restart checkpoint:

- audited exported C API coverage against exported async wrappers; the only
  remaining exported blocking names without async counterparts are
  `mdbx_assert_fail()` and `mdbx_module_handler()`, which are process/module
  support entry points rather than database operations
- simplified `mdbx_async_api_bench` cursor-batch restart logic so both blocking
  and async cursor-batch loops use `MDBX_FIRST` after an end-of-data batch
  instead of issuing a separate cursor reset
- this removes a redundant async enqueue/wait/release cycle after each full
  cursor scan while keeping the benchmark on the public cursor-batch contract:
  `mdbx_cursor_get_batch()` supports `MDBX_FIRST` and repositions a hard-EOF
  cursor itself
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench` plus
  `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.047, async-batch/blocking-parallel 1.046, and
  async-cursor/blocking-parallel 1.062
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` remained noisy; the
  first sample reported async-cursor/blocking-parallel 0.615, while the
  immediate three-sample rerun averaged async/blocking-parallel 1.055,
  async-batch/blocking-parallel 1.061, and async-cursor/blocking-parallel
  1.470
- spot checks with `MDBX_ASYNC_BENCH_CURSOR_BATCH_PAIRS` set to 4096, 8192, and
  16384 did not justify changing the default 2048-pair cursor batch size; the
  cursor ratios remained scheduler-noisy across both default and forced no-map
  runs

Additional coarse submitted-cursor benchmark checkpoint:

- added an `mdbx_async_api_bench` cursor path that uses the existing generic
  `mdbx_async_submit()` API to run one complete cursor-batch loop per async
  executor instead of submitting one typed `mdbx_async_cursor_get_batch()`
  operation per cursor chunk
- the benchmark now reports `async submitted cursor`,
  `async-submit-cursor/par`, and `async-submit-cursor/ser`; this keeps the
  typed cursor-batch wrapper measurement while also measuring coarse async work
  that better amortizes operation-handle and condition-variable overhead
- a rejected tuning sweep tried `MDBX_ASYNC_BENCH_WINDOW=128` and
  `MDBX_ASYNC_BENCH_CURSOR_BATCH_PAIRS=20000`; neither justified changing the
  defaults because cursor and forced no-map samples remained noisy or regressed
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench` plus
  `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.075, async-batch/blocking-parallel 1.077,
  typed async-cursor/blocking-parallel 1.004, and
  async-submit-cursor/blocking-parallel 1.278
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  average ratios were async/blocking-parallel 1.001,
  async-batch/blocking-parallel 0.993, typed async-cursor/blocking-parallel
  0.971, and async-submit-cursor/blocking-parallel 1.125
- longer `MDBX_ASYNC_BENCH_OPS=1000000` samples were mixed: default reported
  async-submit-cursor/blocking-parallel 0.977 while forced no-map/tiny-cache
  reported 1.457; the point-get paths remained above parity in both longer
  runs

Additional typed cursor-loop checkpoint:

- added `mdbx_async_cursor_get_batches()` plus `MDBX_cursor_batch_func` so
  callers can submit a complete cursor-batch loop as one typed async operation
  instead of using the generic `mdbx_async_submit()` escape hatch
- the worker-side operation allocates a temporary batch descriptor array,
  repeatedly calls `mdbx_cursor_get_batch()`, restarts with `MDBX_FIRST` after
  an end-of-data batch, optionally invokes a per-batch callback on the worker
  thread, and reports consumed key/value pairs through caller-owned storage
- smoke coverage now targets more pairs than fit in the table to verify the
  helper wraps after EOF and validates each callback batch
- `mdbx_async_api_bench` now reports the coarse path as `async cursor loop`,
  `async-loop-cursor/par`, and `async-loop-cursor/ser`; this replaces the
  previous benchmark-only generic-submit cursor path with the public typed API
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.060, async-batch/blocking-parallel 1.100,
  typed async-cursor/blocking-parallel 0.649, and
  async-loop-cursor/blocking-parallel 0.980
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  average ratios were async/blocking-parallel 0.994,
  async-batch/blocking-parallel 1.029, typed async-cursor/blocking-parallel
  1.119, and async-loop-cursor/blocking-parallel 1.092
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- cursor benchmark samples remain scheduler-noisy, so this checkpoint should be
  treated as API-surface progress rather than a proven cursor throughput win

Additional batch-get callback checkpoint:

- added `mdbx_async_get_batch_cb()` plus `MDBX_get_batch_func` so callers can
  consume or validate a completed get batch on the executor worker before the
  async operation completes
- kept `mdbx_async_get_batch()` behavior unchanged by sharing the submit path
  with a null callback; per-item result storage and returned value lifetimes
  remain the same as the existing batch-get wrapper
- smoke coverage now verifies the callback sees all successful items and can
  validate the returned key/value payloads on the worker thread
- `mdbx_async_api_bench` now reports `async batch callback get`,
  `async-batch-cb/blocking par`, and `async-batch-cb/blocking ser` alongside
  the existing batch-get measurement
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; average ratios
  were async/blocking-parallel 1.027, async-batch/blocking-parallel 1.079,
  async-batch-callback/blocking-parallel 1.079, typed
  async-cursor/blocking-parallel 0.717, and async-loop-cursor/blocking-parallel
  0.845
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  average ratios were async/blocking-parallel 1.004,
  async-batch/blocking-parallel 1.031,
  async-batch-callback/blocking-parallel 0.987, typed
  async-cursor/blocking-parallel 1.184, and async-loop-cursor/blocking-parallel
  1.236
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- the callback path is API-surface progress, not a proven throughput
  improvement yet; default samples matched plain batch-get on average, while
  forced no-map/tiny-cache samples were lower and noisy

Additional batch wait-release checkpoint:

- added `mdbx_async_wait_release_all()` so callers with many submitted async
  operations can wait, collect per-operation results, release handles, and null
  the handle slots in one public API call
- refactored batch release internals so `mdbx_async_op_release_all()` and the
  new combined wait-release API share the same duplicate-handle detection,
  payload release, spare-list reuse, and free-list behavior
- switched the per-operation GET path in `mdbx_async_api_bench` from separate
  `mdbx_async_wait_all()` and `mdbx_async_op_release_all()` calls to
  `mdbx_async_wait_release_all()`, while leaving batch-get measurements
  unchanged
- smoke batch wait coverage now uses the combined wait-release API for the
  parallel async GET checks
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; GET-path average
  ratios were async/blocking-parallel 0.988, async-batch/blocking-parallel
  1.112, and async-batch-callback/blocking-parallel 1.084
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  GET-path average ratios were async/blocking-parallel 1.014,
  async-batch/blocking-parallel 1.035, and
  async-batch-callback/blocking-parallel 1.016
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- the combined wait-release API reduces public-call and lock surface for
  parallel GET windows, but the local benchmark sample remained noisy and did
  not prove a stable per-operation GET throughput improvement

Additional threaded GET benchmark checkpoint:

- added `async threaded get` and `async threaded batch get` paths to
  `mdbx_async_api_bench`; these use multiple application pthreads, each with
  its own async executor and read transaction, so submission itself now happens
  concurrently instead of from one benchmark thread
- the per-operation threaded path submits windows of `mdbx_async_get()` calls
  and uses `mdbx_async_wait_release_all()` for each window; the batch threaded
  path submits one `mdbx_async_get_batch()` operation per window
- this benchmark slice is measurement coverage for the updated goal of parallel
  GET operations from multiple threads; it does not add new public API surface
- compared with the pre-async ioarena baseline at the top of this log, the
  explicit `io_uring` backend still remains below the earlier mapped/no-map
  lazy-mode numbers for ioarena GET throughput, so the async API work has not
  yet recovered end-to-end storage-backend performance there
- within the public async benchmark, the original first async read sample was
  async/blocking-parallel 1.139 by submitting many GETs through the async API
  from one benchmark thread; current threaded samples show that concurrent
  submission does not automatically improve the per-operation GET path
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; GET-path average
  ratios were async/blocking-parallel 0.954, async-thread/blocking-parallel
  0.900, async-thread-batch/blocking-parallel 1.036,
  async-batch/blocking-parallel 0.993, and
  async-batch-callback/blocking-parallel 1.072
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  GET-path average ratios were async/blocking-parallel 0.992,
  async-thread/blocking-parallel 0.926,
  async-thread-batch/blocking-parallel 0.952,
  async-batch/blocking-parallel 0.999, and
  async-batch-callback/blocking-parallel 0.961
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed; small ASAN ratios were async/blocking-parallel 1.142,
  async-thread/blocking-parallel 0.891,
  async-thread-batch/blocking-parallel 1.234,
  async-batch/blocking-parallel 1.232, and
  async-batch-callback/blocking-parallel 1.028
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- conclusion: the threaded single-op async GET path is slower on average; the
  threaded batch GET path can reach parity or better in default samples but is
  not stable in forced no-map/tiny-cache samples, so this is benchmark coverage
  and diagnosis rather than a final throughput success

Additional worker-side GET-loop checkpoint:

- added `mdbx_async_get_loop()` plus `MDBX_get_loop_key_func` and
  `MDBX_get_loop_result_func`; callers can submit one async operation that
  prepares keys, runs many `mdbx_get()` calls, and consumes results entirely on
  the executor worker thread
- this is the point-GET analogue of the existing cursor batch-loop helper: it
  avoids one operation handle, enqueue, wait, and release cycle per key while
  keeping the existing blocking API unchanged
- smoke coverage verifies that the key callback and result callback run for all
  items and that returned values are valid on the worker thread
- `mdbx_async_api_bench` now reports `async get loop`,
  `async-loop/blocking par`, and `async-loop/blocking ser`; the existing
  single-op, threaded, batch, and batch-callback GET paths remain in the report
  so their small-window overhead stays visible
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; GET-path average
  ratios were async/blocking-parallel 1.085, async-thread/blocking-parallel
  1.062, async-thread-batch/blocking-parallel 1.064,
  async-batch/blocking-parallel 1.079,
  async-batch-callback/blocking-parallel 1.108, and
  async-loop/blocking-parallel 1.098
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  GET-path average ratios were async/blocking-parallel 1.074,
  async-thread/blocking-parallel 1.036,
  async-thread-batch/blocking-parallel 1.023,
  async-batch/blocking-parallel 1.019,
  async-batch-callback/blocking-parallel 0.972, and
  async-loop/blocking-parallel 1.032
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed; small ASAN ratios included
  async-loop/blocking-parallel 1.234 and async-loop/blocking-serial 2.540
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- conclusion: worker-side GET loops give the async API a stable coarse
  parallel-read shape above blocking-parallel parity in these samples; the
  small-window per-operation API path still needs further work before it can be
  considered consistently faster

Additional threaded GET-loop benchmark checkpoint:

- added `async threaded get loop` to `mdbx_async_api_bench`; it starts multiple
  application pthreads and each thread submits one `mdbx_async_get_loop()`
  operation through its own async executor and read transaction
- this measures the worker-side GET-loop API under the same multi-application
  thread shape as `async threaded get` and `async threaded batch get`, without
  changing the public async API
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- three clean default `mdbx_async_api_bench` samples passed; GET-path average
  ratios were async/blocking-parallel 1.106, async-thread/blocking-parallel
  1.049, async-thread-batch/blocking-parallel 1.092,
  async-batch/blocking-parallel 1.121,
  async-batch-callback/blocking-parallel 1.077,
  async-loop/blocking-parallel 1.079, and
  async-thread-loop/blocking-parallel 1.098
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  GET-path average ratios were async/blocking-parallel 1.067,
  async-thread/blocking-parallel 1.028,
  async-thread-batch/blocking-parallel 1.029,
  async-batch/blocking-parallel 1.001,
  async-batch-callback/blocking-parallel 0.989,
  async-loop/blocking-parallel 1.052, and
  async-thread-loop/blocking-parallel 1.050
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed; small ASAN ratios included
  async-thread-loop/blocking-parallel 1.042 and
  async-thread-loop/blocking-serial 2.205
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- conclusion: the new benchmark confirms that `mdbx_async_get_loop()` stays
  above blocking-parallel parity when submitted concurrently by multiple
  application threads; the result remains scheduler-noisy, but it is aligned
  with the target workload shape

Additional extended GET-loop API checkpoint:

- added `mdbx_async_get_ex_loop()` plus `MDBX_get_ex_loop_result_func` so
  callers can run many `mdbx_get_ex()` operations as one worker-side async
  operation while observing returned key/value descriptors and duplicate counts
  in a worker-thread callback
- added `mdbx_async_get_equal_or_great_loop()` plus
  `MDBX_get_loop_data_func` and
  `MDBX_get_equal_or_great_loop_result_func`; the data preparation callback is
  optional for key-only lower-bound use, and the result callback receives the
  exact `mdbx_get_equal_or_great()` result code, including
  `MDBX_RESULT_TRUE`
- this extends the coarse worker-side loop pattern from plain `mdbx_get()` to
  the remaining GET-family public wrappers without changing the blocking API or
  changing the existing benchmark labels
- smoke coverage now checks completed counts, key callbacks, result callbacks,
  `mdbx_get_ex()` duplicate counts, and mixed exact/greater lower-bound results
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- default benchmark spot check passed with GET-path ratios
  async/blocking-parallel 1.062, async-thread/blocking-parallel 1.079,
  async-thread-batch/blocking-parallel 1.073,
  async-batch/blocking-parallel 1.094,
  async-batch-callback/blocking-parallel 1.082,
  async-loop/blocking-parallel 1.090, and
  async-thread-loop/blocking-parallel 1.067
- three forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` samples passed;
  GET-path average ratios were async/blocking-parallel 1.081,
  async-thread/blocking-parallel 1.073,
  async-thread-batch/blocking-parallel 1.057,
  async-batch/blocking-parallel 1.067,
  async-batch-callback/blocking-parallel 1.104,
  async-loop/blocking-parallel 0.996, and
  async-thread-loop/blocking-parallel 1.047
- conclusion: this slice improves async GET-family API coverage for coarse
  parallel read workloads; it does not change the storage backend or prove a new
  throughput step, and the forced no-map GET-loop samples remain scheduler-noisy

Additional extended GET-batch API checkpoint:

- added `mdbx_async_get_ex_batch()` and `mdbx_async_get_ex_batch_cb()` plus
  `MDBX_get_ex_batch_func`; these let callers submit an array of `mdbx_get_ex()`
  lookups as one async operation and optionally consume the filled key/data,
  duplicate-count, and result arrays on the executor worker
- added `mdbx_async_get_equal_or_great_batch()` and
  `mdbx_async_get_equal_or_great_batch_cb()` plus
  `MDBX_get_equal_or_great_batch_func`; per-item results preserve
  `MDBX_SUCCESS` versus `MDBX_RESULT_TRUE`, and successful items update the
  caller's key/data descriptors with the lower-bound result
- this brings the extended GET wrappers closer to the plain `mdbx_get()` async
  surface by providing single-operation, batch, callback-batch, and worker-loop
  forms without changing the blocking API
- smoke coverage now checks direct and callback batch paths for `get_ex` value
  counts and mixed exact/greater lower-bound results
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 2/2
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 10/10
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 2/2
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 17/17
- default benchmark spot check passed with GET-path ratios
  async/blocking-parallel 1.071, async-thread/blocking-parallel 1.067,
  async-thread-batch/blocking-parallel 1.092,
  async-batch/blocking-parallel 1.088,
  async-batch-callback/blocking-parallel 1.094,
  async-loop/blocking-parallel 1.098, and
  async-thread-loop/blocking-parallel 1.076
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check passed with
  GET-path ratios async/blocking-parallel 1.138,
  async-thread/blocking-parallel 1.096,
  async-thread-batch/blocking-parallel 1.033,
  async-batch/blocking-parallel 1.044,
  async-batch-callback/blocking-parallel 1.114,
  async-loop/blocking-parallel 1.135, and
  async-thread-loop/blocking-parallel 1.084
- conclusion: this slice improves GET-family API symmetry for callers that
  already hold arrays of lookup work; it does not change storage internals, and
  existing benchmark labels remain a guard against regressions in the parallel
  plain-GET paths

Additional async API coverage audit checkpoint:

- added `mdbx_async_api_audit`, a small public-header audit tool that scans
  exported `LIBMDBX_API` blocking C functions in `mdbx.h`, maps exported
  `mdbx_async_*` declarations back to their blocking names, and fails when a
  blocking API has neither a name-compatible async wrapper nor an explicit
  exemption
- wired the audit into CMake/CTest as `async_api_audit`, so the public CTest
  gate now protects the additive async surface from silently losing coverage
- wired the audit into the GNUmake test target list as `mdbx_async_api_audit`
- the current audit result is `blocking=171 async-covered=132 exempt=39
  missing=0`
- exemptions are limited to pure conversion/limits helpers, error-string and
  formatting helpers, global debug/panic setup, assertion/module hooks,
  post-fork recovery, and integrity-check helper functions where async executor
  offload would not add useful behavior
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_audit mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_audit mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: this slice does not change libmdbx runtime behavior or storage
  internals, but it makes the "async API should cover the blocking API" part of
  the migration objective mechanically checkable in the normal public test gate

Benchmark comparison checkpoint:

- committed the async API coverage audit as `a7fa8c0`
- re-read the machine-local ioarena lazy-mode logs and compared the current
  explicit-I/O runs against the pre-migration mapped/no-map baselines
- using all currently available lazy logs, current explicit default remains
  below the earlier mapped baseline on every ioarena phase:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 910.524 ops/s | 0.881 |
| crud | 55.675 Kops/s | 46.191 Kops/s | 0.830 |
| iterate | 26.143 Mops/s | 24.567 Mops/s | 0.940 |
| get | 279.409 Kops/s | 230.557 Kops/s | 0.825 |
| delete | 66.398 Kops/s | 54.803 Kops/s | 0.825 |

- current explicit forced no-map also remains below the earlier no-map baseline:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 944.646 ops/s | 0.867 |
| crud | 59.786 Kops/s | 48.615 Kops/s | 0.813 |
| iterate | 25.827 Mops/s | 23.311 Mops/s | 0.902 |
| get | 274.039 Kops/s | 242.864 Kops/s | 0.886 |
| delete | 69.067 Kops/s | 56.264 Kops/s | 0.815 |

- fresh `mdbx_async_api_bench` default sample:
  async/blocking-parallel 1.053, async-thread/blocking-parallel 0.905,
  async-thread-batch/blocking-parallel 0.855,
  async-batch/blocking-parallel 0.968,
  async-batch-callback/blocking-parallel 1.043,
  async-loop/blocking-parallel 1.095, and
  async-thread-loop/blocking-parallel 1.099
- fresh forced no-map/tiny-cache sample with `MDBX_ASYNC_BENCH_OPS=300000`:
  async/blocking-parallel 0.944, async-thread/blocking-parallel 0.900,
  async-thread-batch/blocking-parallel 0.981,
  async-batch/blocking-parallel 1.025,
  async-batch-callback/blocking-parallel 0.995,
  async-loop/blocking-parallel 1.047, and
  async-thread-loop/blocking-parallel 1.056
- conclusion: the additive async public API can beat the benchmark's blocking
  pthread-parallel GET path when work is coalesced into worker-side loop
  operations, but the explicit-I/O storage backend has not recovered the
  pre-migration ioarena throughput baseline yet

Additional async executor queue-drain checkpoint:

- changed the async executor worker so it detaches the currently queued FIFO
  segment under the condition-pair lock, then executes that local segment in
  order instead of re-locking the shared queue to pop every operation
- each completed operation is still marked under the condition-pair lock, so
  `mdbx_async_poll()`, `mdbx_async_wait()`, and sequence-based
  `mdbx_async_wait_all()` semantics remain unchanged
- `async->active` remains true while the detached segment is running; new
  submissions enqueue normally and are picked up on the next worker loop without
  an unnecessary wakeup
- this targets the per-operation async GET/window paths by removing one shared
  queue pop per already-submitted operation; it does not change the storage
  backend or make explicit I/O faster by itself
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with GET-path ratios
  async/blocking-parallel 1.293, async-thread/blocking-parallel 1.320,
  async-thread-batch/blocking-parallel 1.428,
  async-batch/blocking-parallel 1.393,
  async-batch-callback/blocking-parallel 1.386,
  async-loop/blocking-parallel 1.405, and
  async-thread-loop/blocking-parallel 1.396
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default `mdbx_async_api_bench` spot check passed with GET-path ratios
  async/blocking-parallel 1.130, async-thread/blocking-parallel 1.068,
  async-thread-batch/blocking-parallel 1.013,
  async-batch/blocking-parallel 1.138,
  async-batch-callback/blocking-parallel 1.109,
  async-loop/blocking-parallel 1.132, and
  async-thread-loop/blocking-parallel 1.149
- default `MDBX_ASYNC_BENCH_OPS=1000000` sample passed with GET-path ratios
  async/blocking-parallel 1.024, async-thread/blocking-parallel 0.884,
  async-thread-batch/blocking-parallel 1.019,
  async-batch/blocking-parallel 1.064,
  async-batch-callback/blocking-parallel 1.061,
  async-loop/blocking-parallel 1.068, and
  async-thread-loop/blocking-parallel 1.065
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=1000000` sample remained mixed:
  async/blocking-parallel 0.943, async-thread/blocking-parallel 0.914,
  async-thread-batch/blocking-parallel 1.011,
  async-batch/blocking-parallel 0.993,
  async-batch-callback/blocking-parallel 0.986,
  async-loop/blocking-parallel 1.065, and
  async-thread-loop/blocking-parallel 0.979
- conclusion: queue detaching reduces executor overhead for queued windows and
  keeps the coarse GET paths above blocking-parallel parity in the default
  benchmark samples, but the small per-operation threaded path and forced
  no-map/tiny-cache samples are still noisy; the next performance work should
  continue moving hot parallel read workloads toward coarse worker-side
  operations or address the explicit-I/O storage backend gap directly

Additional async waiter-target checkpoint:

- added internal wait-target bookkeeping to the async executor so waiters that
  are waiting for a known operation sequence register that target sequence
- `mdbx_async_wait()`, `mdbx_async_wait_all()`, and
  `mdbx_async_wait_release_all()` now avoid worker wakeups until the requested
  sequence has completed; untargeted drain waits used by
  `mdbx_async_destroy(..., true)` still wake when the executor becomes idle
- this removes avoidable condition-variable wakeups for windowed
  `mdbx_async_wait_release_all()` users, which is the hot path for submitted
  parallel GET windows in `mdbx_async_api_bench`
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with GET-path ratios
  async/blocking-parallel 1.340, async-thread/blocking-parallel 1.515,
  async-thread-batch/blocking-parallel 1.502,
  async-batch/blocking-parallel 1.553,
  async-batch-callback/blocking-parallel 1.526,
  async-loop/blocking-parallel 1.588, and
  async-thread-loop/blocking-parallel 1.537
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check passed with GET-path ratios
  async/blocking-parallel 1.030, async-thread/blocking-parallel 1.002,
  async-thread-batch/blocking-parallel 1.053,
  async-batch/blocking-parallel 1.012,
  async-batch-callback/blocking-parallel 1.065,
  async-loop/blocking-parallel 1.063, and
  async-thread-loop/blocking-parallel 1.086
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check remained
  mixed: async/blocking-parallel 1.065, async-thread/blocking-parallel 0.994,
  async-thread-batch/blocking-parallel 1.038,
  async-batch/blocking-parallel 0.944,
  async-batch-callback/blocking-parallel 1.055,
  async-loop/blocking-parallel 0.972, and
  async-thread-loop/blocking-parallel 0.954
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample passed with all reported
  GET paths above blocking-parallel: async/blocking-parallel 1.166,
  async-thread/blocking-parallel 1.145,
  async-thread-batch/blocking-parallel 1.214,
  async-batch/blocking-parallel 1.227,
  async-batch-callback/blocking-parallel 1.228,
  async-loop/blocking-parallel 1.232, and
  async-thread-loop/blocking-parallel 1.234
- larger forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=1000000` sample passed
  with all GET paths except simple threaded per-operation GET above
  blocking-parallel: async/blocking-parallel 1.140,
  async-thread/blocking-parallel 0.992,
  async-thread-batch/blocking-parallel 1.179,
  async-batch/blocking-parallel 1.184,
  async-batch-callback/blocking-parallel 1.158,
  async-loop/blocking-parallel 1.127, and
  async-thread-loop/blocking-parallel 1.127
- conclusion: targeted waits reduce wakeup churn for batched async windows and
  show a useful larger-sample GET benchmark improvement, but this still does
  not close the separate pre-migration ioarena storage-throughput gap

Additional async GET many-submit checkpoint:

- added `mdbx_async_get_many()`, an additive public API that submits many
  independent `mdbx_get()` async operations and returns one normal operation
  handle per item
- the new API copies all key bytes during submission, preserves normal
  per-operation wait/release semantics, and can be consumed with
  `mdbx_async_wait_release_all()`
- internally, many-submit allocation pulls a window of operation handles from
  the executor spare list under one condition-pair lock, allocates any missing
  handles outside the lock, and enqueues the prepared window with one executor
  lock/signaling pass
- `mdbx_async_api_bench` now uses `mdbx_async_get_many()` for its windowed
  per-operation GET paths; the operation handles and result validation remain
  per item, but submission no longer takes the executor lock once per key
- smoke coverage now verifies `mdbx_async_get_many()` through
  `mdbx_async_wait_release_all()` and per-item value checks
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with GET-path ratios
  async/blocking-parallel 1.174, async-thread/blocking-parallel 1.215,
  async-thread-batch/blocking-parallel 1.006,
  async-batch/blocking-parallel 1.552,
  async-batch-callback/blocking-parallel 1.122,
  async-loop/blocking-parallel 1.529, and
  async-thread-loop/blocking-parallel 1.392
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check passed with GET-path ratios
  async/blocking-parallel 1.102, async-thread/blocking-parallel 1.023,
  async-thread-batch/blocking-parallel 1.098,
  async-batch/blocking-parallel 1.107,
  async-batch-callback/blocking-parallel 1.084,
  async-loop/blocking-parallel 1.101, and
  async-thread-loop/blocking-parallel 1.081
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check passed
  near or above blocking-parallel for the GET paths: async/blocking-parallel
  1.054, async-thread/blocking-parallel 0.997,
  async-thread-batch/blocking-parallel 1.061,
  async-batch/blocking-parallel 1.099,
  async-batch-callback/blocking-parallel 1.141,
  async-loop/blocking-parallel 1.096, and
  async-thread-loop/blocking-parallel 1.114
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample passed with GET-path
  ratios async/blocking-parallel 1.127, async-thread/blocking-parallel 1.022,
  async-thread-batch/blocking-parallel 1.117,
  async-batch/blocking-parallel 1.181,
  async-batch-callback/blocking-parallel 1.185,
  async-loop/blocking-parallel 1.183, and
  async-thread-loop/blocking-parallel 1.177
- larger forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=1000000` sample
  remained mixed for the simple threaded many-submit shape:
  async/blocking-parallel 1.056, async-thread/blocking-parallel 0.901,
  async-thread-batch/blocking-parallel 1.018,
  async-batch/blocking-parallel 1.058,
  async-batch-callback/blocking-parallel 1.041,
  async-loop/blocking-parallel 0.971, and
  async-thread-loop/blocking-parallel 1.105
- conclusion: `mdbx_async_get_many()` reduces submission overhead for callers
  that still need independent operation handles, improving the default
  per-operation GET benchmark path while preserving the coarser batch/loop APIs
  for callers that can use one async operation per window

Additional extended GET many-submit checkpoint:

- added `mdbx_async_get_ex_many()` so callers can submit many independent
  `mdbx_get_ex()` operations with one allocation/enqueue window while still
  receiving one normal async operation handle per item
- added `mdbx_async_get_equal_or_great_many()` with the same independent-handle
  many-submit shape; per-operation results preserve `MDBX_SUCCESS` versus
  `MDBX_RESULT_TRUE`
- both wrappers copy submitted key bytes before enqueueing; the lower-bound
  wrapper also copies each input data descriptor before enqueueing, matching the
  single-operation wrapper semantics
- smoke coverage now verifies `get_ex` many-submit duplicate counts, returned
  key/value descriptors, and mixed exact/greater lower-bound many-submit results
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_bench mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with GET-path ratios
  async/blocking-parallel 1.141, async-thread/blocking-parallel 1.258,
  async-thread-batch/blocking-parallel 1.117,
  async-batch/blocking-parallel 1.278,
  async-batch-callback/blocking-parallel 1.270,
  async-loop/blocking-parallel 1.136, and
  async-thread-loop/blocking-parallel 1.305
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- short default and forced no-map/tiny-cache benchmark spot checks were noisy
  and below parity for several GET paths, despite this slice not changing the
  benchmarked plain-GET runtime path
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample passed with all reported
  GET paths above blocking-parallel: async/blocking-parallel 1.010,
  async-thread/blocking-parallel 1.088,
  async-thread-batch/blocking-parallel 1.063,
  async-batch/blocking-parallel 1.013,
  async-batch-callback/blocking-parallel 1.067,
  async-loop/blocking-parallel 1.066, and
  async-thread-loop/blocking-parallel 1.066
- conclusion: this slice extends the independent-handle many-submit surface
  across the GET family; it is primarily API symmetry and submission-overhead
  coverage, not a new storage-backend performance step

Additional GET many-submit benchmark split checkpoint:

- split `mdbx_async_api_bench` so the original per-operation
  `mdbx_async_get()` submission path is measured separately from the
  `mdbx_async_get_many()` submission path
- the benchmark now reports `async many parallel get`,
  `async threaded many get`, `async-many/blocking par`, and
  `async-thread-many/par` beside the existing single-submit, batch, callback,
  and loop labels
- this makes the many-submit API's submission-overhead effect visible without
  changing the public API or the blocking baseline
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with GET-path ratios
  async/blocking-parallel 0.805, async-many/blocking-parallel 1.457,
  async-thread/blocking-parallel 1.467,
  async-thread-many/blocking-parallel 1.511,
  async-thread-batch/blocking-parallel 1.492,
  async-batch/blocking-parallel 1.539,
  async-batch-callback/blocking-parallel 1.518,
  async-loop/blocking-parallel 1.546, and
  async-thread-loop/blocking-parallel 1.543
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check passed with ratios
  async/blocking-parallel 1.091, async-many/blocking-parallel 1.075,
  async-thread/blocking-parallel 1.014,
  async-thread-many/blocking-parallel 1.009,
  async-thread-batch/blocking-parallel 1.074,
  async-batch/blocking-parallel 1.055,
  async-batch-callback/blocking-parallel 1.091,
  async-loop/blocking-parallel 1.078, and
  async-thread-loop/blocking-parallel 1.058
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check showed
  many-submit above single-submit for both comparable GET shapes:
  async/blocking-parallel 1.015, async-many/blocking-parallel 1.060,
  async-thread/blocking-parallel 0.859,
  async-thread-many/blocking-parallel 0.903,
  async-thread-batch/blocking-parallel 0.932,
  async-batch/blocking-parallel 0.980,
  async-batch-callback/blocking-parallel 1.014,
  async-loop/blocking-parallel 1.014, and
  async-thread-loop/blocking-parallel 0.938
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample passed with all reported
  GET paths above blocking-parallel, including async-many/blocking-parallel
  1.146 and async-thread-many/blocking-parallel 1.116
- larger forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=1000000` sample stayed
  mixed: async/blocking-parallel 1.050, async-many/blocking-parallel 1.010,
  async-thread/blocking-parallel 1.026,
  async-thread-many/blocking-parallel 0.978,
  async-thread-batch/blocking-parallel 1.016,
  async-batch/blocking-parallel 0.910,
  async-batch-callback/blocking-parallel 0.965,
  async-loop/blocking-parallel 1.070, and
  async-thread-loop/blocking-parallel 1.090
- conclusion: the benchmark can now distinguish single-submit GET windows from
  many-submit GET windows; current samples confirm that `mdbx_async_get_many()`
  can reduce submission overhead, while forced no-map/tiny-cache remains noisy

Additional GET many-submit ratio checkpoint:

- added direct `mdbx_async_api_bench` ratios comparing many-submit GET against
  single-submit GET:
  `async-many/async` and `async-thread-many/thread`
- this makes each benchmark run show whether `mdbx_async_get_many()` actually
  improves the corresponding single-submit shape on that machine/sample,
  instead of requiring manual division of the blocking-relative ratios
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with direct ratios
  async-many/async 1.213 and async-thread-many/thread 1.039
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check reported async-many/async 0.989 and
  async-thread-many/thread 1.019
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async-many/async 0.970 and async-thread-many/thread 0.916
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample reported
  async-many/async 0.996 and async-thread-many/thread 0.969 while all reported
  GET paths remained above blocking-parallel
- conclusion: the direct ratios show `mdbx_async_get_many()` is not a universal
  per-sample win over single-submit GET; the many-submit API remains useful for
  reducing submission lock traffic, but benchmark noise and workload shape must
  be considered explicitly

Additional many-submit spare-window checkpoint:

- moved recycled operation-handle preparation in `async_ops_alloc()` outside
  the executor condition-pair lock; the lock now only detaches the requested
  spare window and updates the shared spare count
- this keeps the public async API unchanged, but reduces submission-side mutex
  hold time for `mdbx_async_get_many()`, `mdbx_async_get_ex_many()`, and
  `mdbx_async_get_equal_or_great_many()` when multiple caller threads recycle
  operation windows concurrently
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check reported async/blocking-parallel 1.085,
  async-many/blocking-parallel 1.084,
  async-thread/blocking-parallel 1.046,
  async-thread-many/blocking-parallel 1.074,
  async-many/async 0.999, and async-thread-many/thread 1.027
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.097, async-many/blocking-parallel 1.141,
  async-thread/blocking-parallel 0.988,
  async-thread-many/blocking-parallel 1.038,
  async-many/async 1.040, and async-thread-many/thread 1.050
- conclusion: the change removes avoidable work from the shared submission
  lock. The local threaded many-submit samples improved versus the immediately
  preceding spot checks, but this remains a lock-contention cleanup rather than
  proof that the storage-level pre-migration benchmark gap is closed.

Additional completion-chunk checkpoint:

- changed the async executor worker to publish completed operations in small
  chunks instead of locking the condition pair once per operation; each handle
  still has its own result and completion flag, and targeted waits are woken at
  chunk boundaries or at the end of the detached queue segment
- the chunk is intentionally bounded at 16 operations so single-handle waits
  remain responsive while GET windows avoid most per-operation completion-lock
  traffic
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `env LSAN_OPTIONS=detect_leaks=0 MDBX_ASYNC_BENCH_OPS=1000 LD_LIBRARY_PATH=@cmake-asan-build @cmake-asan-build/mdbx_async_api_bench`: passed with direct ratios
  async-many/async 1.165 and async-thread-many/thread 1.059
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- default benchmark spot check reported async/blocking-parallel 1.035,
  async-many/blocking-parallel 1.061,
  async-thread/blocking-parallel 1.046,
  async-thread-many/blocking-parallel 1.057,
  async-many/async 1.025, and async-thread-many/thread 1.010
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.005, async-many/blocking-parallel 1.063,
  async-thread/blocking-parallel 0.928,
  async-thread-many/blocking-parallel 1.066,
  async-many/async 1.058, and async-thread-many/thread 1.148
- larger default `MDBX_ASYNC_BENCH_OPS=1000000` sample reported all GET paths
  above blocking-parallel, with async-many/async 0.994 and
  async-thread-many/thread 1.008
- conclusion: completion chunking reduces executor mutex churn for dense async
  windows and improves the direct many-submit comparison in the local short
  samples, while the larger sample remains near-neutral. This is still an async
  executor throughput cleanup, not a fix for the separate pre-migration
  ioarena storage-throughput gap.

Additional explicit page-cache LRU checkpoint:

- changed the explicit page-cache policy from effectively MRU eviction to a
  simple most-recent-first list: cache hits move the entry to the head, and
  pruning now evicts the oldest unpinned entry
- this targets no-map explicit-I/O read locality without changing the blocking
  mmap API path or the public async API
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit mdbx_migration_smoke`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.155, async-many/blocking-parallel 1.181,
  async-thread/blocking-parallel 1.149,
  async-thread-many/blocking-parallel 1.127,
  async-batch/blocking-parallel 1.196, and async-loop/blocking-parallel 1.185
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed, with current
  explicit default:

| phase | earlier mapped avg | current explicit default | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 966.305 ops/s | 0.935 |
| crud | 55.675 Kops/s | 49.756 Kops/s | 0.894 |
| iterate | 26.143 Mops/s | 31.656 Mops/s | 1.211 |
| get | 279.409 Kops/s | 399.575 Kops/s | 1.430 |
| delete | 66.398 Kops/s | 58.374 Kops/s | 0.879 |

- current explicit forced no-map against the earlier no-map baseline:

| phase | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 944.749 ops/s | 0.867 |
| crud | 59.787 Kops/s | 49.784 Kops/s | 0.833 |
| iterate | 25.827 Mops/s | 31.102 Mops/s | 1.204 |
| get | 274.039 Kops/s | 412.888 Kops/s | 1.507 |
| delete | 69.067 Kops/s | 57.767 Kops/s | 0.836 |

- conclusion: the explicit page cache now beats the pre-migration read-heavy
  ioarena baseline for `iterate` and `get`, including forced no-map/tiny-cache.
  The storage migration is still not complete because batch, CRUD, and delete
  remain below the earlier mapped/no-map baselines.

Additional reusable-cache invalidation checkpoint:

- added an explicit page-cache `reusable_count` and used it to skip ordinary
  write invalidation scans when every cached entry is reusable
- ordinary CoW writes keep snapshot-keyed reusable cache entries valid, so this
  removes avoidable full-cache scans from write-heavy paths after read-heavy
  workloads populate the explicit cache; destructive invalidations still scan
  and evict reusable entries
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.114, async-many/blocking-parallel 1.084,
  async-thread/blocking-parallel 1.062,
  async-thread-many/blocking-parallel 1.083,
  async-thread-batch/blocking-parallel 1.105,
  async-batch/blocking-parallel 1.113, and
  async-batch-callback/blocking-parallel 1.127
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed, with current
  explicit default:

| phase | earlier mapped avg | current explicit default | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 942.085 ops/s | 0.912 |
| crud | 55.675 Kops/s | 50.173 Kops/s | 0.901 |
| iterate | 26.143 Mops/s | 31.920 Mops/s | 1.221 |
| get | 279.409 Kops/s | 430.834 Kops/s | 1.542 |
| delete | 66.398 Kops/s | 58.351 Kops/s | 0.879 |

- current explicit forced no-map against the earlier no-map baseline:

| phase | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 955.758 ops/s | 0.877 |
| crud | 59.787 Kops/s | 50.309 Kops/s | 0.841 |
| iterate | 25.827 Mops/s | 23.859 Mops/s | 0.924 |
| get | 274.039 Kops/s | 434.173 Kops/s | 1.584 |
| delete | 69.067 Kops/s | 58.151 Kops/s | 0.842 |

- conclusion: skipping ordinary invalidation scans preserves the read-heavy
  GET win and nudges CRUD upward in this sample, but batch/delete are still
  below the pre-migration baselines and forced iterate remains noisy.

Additional ordinary-invalidation scan checkpoint:

- ordinary CoW cache invalidation now skips reusable entries before range
  overlap checks and stops scanning once all non-reusable cache entries have
  been considered
- destructive invalidations still scan reusable entries, preserving truncate and
  remove semantics; this is a small write-path cleanup for mixed explicit-cache
  workloads rather than a public API change
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.134, async-many/blocking-parallel 1.140,
  async-thread/blocking-parallel 1.104,
  async-thread-many/blocking-parallel 1.119,
  async-batch/blocking-parallel 1.169,
  async-batch-callback/blocking-parallel 1.147, and
  async-loop/blocking-parallel 1.121
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed, with current
  explicit default:

| phase | earlier mapped avg | current explicit default | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 948.717 ops/s | 0.918 |
| crud | 55.675 Kops/s | 49.691 Kops/s | 0.893 |
| iterate | 26.143 Mops/s | 31.743 Mops/s | 1.214 |
| get | 279.409 Kops/s | 402.623 Kops/s | 1.441 |
| delete | 66.398 Kops/s | 58.319 Kops/s | 0.878 |

- current explicit forced no-map against the earlier no-map baseline:

| phase | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 944.933 ops/s | 0.867 |
| crud | 59.787 Kops/s | 50.467 Kops/s | 0.844 |
| iterate | 25.827 Mops/s | 31.041 Mops/s | 1.202 |
| get | 274.039 Kops/s | 432.381 Kops/s | 1.578 |
| delete | 69.067 Kops/s | 57.240 Kops/s | 0.829 |

- conclusion: the scan cleanup is correct under the validation gates and keeps
  read-heavy phases above baseline, but it does not close the batch, CRUD, or
  delete gaps. The remaining work is still in the write-heavy explicit-I/O path.

Additional dirty-write enqueue validation checkpoint:

- collapsed redundant validation in the dirty-page enqueue hot path: `iov_page()`
  now builds and validates the queued write submit object once, then passes it
  to an explicitly validated enqueue helper
- queue-full retry reuses that already-validated submit object after flushing
  the existing queue; the low-level queue add path still validates the concrete
  data write range it receives before merging/enqueuing it
- this targets per-dirty-page CPU overhead in batch/CRUD/delete workloads
  without changing the blocking public API or the async public API surface
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.167, async-many/blocking-parallel 1.150,
  async-thread/blocking-parallel 1.051,
  async-thread-many/blocking-parallel 1.095,
  async-batch/blocking-parallel 1.154,
  async-batch-callback/blocking-parallel 1.179, and
  async-loop/blocking-parallel 1.124
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed, with current
  explicit default:

| phase | earlier mapped avg | current explicit default | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 944.644 ops/s | 0.914 |
| crud | 55.675 Kops/s | 50.191 Kops/s | 0.901 |
| iterate | 26.143 Mops/s | 31.544 Mops/s | 1.207 |
| get | 279.409 Kops/s | 438.219 Kops/s | 1.568 |
| delete | 66.398 Kops/s | 58.715 Kops/s | 0.884 |

- current explicit forced no-map against the earlier no-map baseline:

| phase | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 961.688 ops/s | 0.882 |
| crud | 59.787 Kops/s | 50.896 Kops/s | 0.851 |
| iterate | 25.827 Mops/s | 30.715 Mops/s | 1.189 |
| get | 274.039 Kops/s | 433.664 Kops/s | 1.582 |
| delete | 69.067 Kops/s | 59.105 Kops/s | 0.856 |

- conclusion: reducing redundant dirty-write enqueue validation preserves the
  read-heavy wins and slightly improves several write-heavy samples, especially
  forced no-map batch/delete versus the previous checkpoint. It still does not
  close the pre-migration write-heavy baseline gap.

Additional dirty-write enqueue wrapper and merge-probe checkpoint:

- added an internal already-validated dirty-write submit wrapper for `iov_page()`
  so the just-built dirty queued write is copied into its submit envelope without
  rebuilding and revalidating the same storage/page range
- moved cheap adjacency checks ahead of queued-base validation in
  `ior_item_make_merged_io()`, avoiding descriptor validation when a new dirty
  write cannot merge with the previous queue item
- contiguous merges still validate the existing queued base and the merged
  descriptor; `osal_ioring_add()` still validates each incoming write range
  before merge/enqueue decisions
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.082, async-many/blocking-parallel 1.109,
  async-thread/blocking-parallel 1.003,
  async-thread-many/blocking-parallel 1.055,
  async-batch/blocking-parallel 1.118,
  async-batch-callback/blocking-parallel 1.112, and
  async-loop/blocking-parallel 1.050. The threaded-batch/threaded-loop variants
  were noisy in this sample.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed twice after the code
  change; `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed with
  three paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 955.449 ops/s | 0.925 |
| crud | 55.675 Kops/s | 50.496 Kops/s | 0.907 |
| iterate | 26.143 Mops/s | 28.262 Mops/s | 1.081 |
| get | 279.409 Kops/s | 424.746 Kops/s | 1.520 |
| delete | 66.398 Kops/s | 58.700 Kops/s | 0.884 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 954.844 ops/s | 0.876 |
| crud | 59.787 Kops/s | 50.860 Kops/s | 0.851 |
| iterate | 25.827 Mops/s | 27.845 Mops/s | 1.078 |
| get | 274.039 Kops/s | 413.607 Kops/s | 1.509 |
| delete | 69.067 Kops/s | 58.483 Kops/s | 0.847 |

- conclusion: the lower enqueue/merge validation cleanup preserves the
  read-heavy average wins and keeps write-heavy phases in the same improved band
  as the prior checkpoint, but batch, CRUD, and delete remain below the
  pre-migration baseline.

Additional write-queue accounting checkpoint:

- added cached `write_items` and `payload_bytes` counters to `osal_ioring_t`
  and maintain them in `osal_ioring_add()` and `osal_ioring_reset()`
- `dxb_storage_make_queued_write_io()`, `dxb_queue_op_result()`, and
  `osal_ioring_write()` now get write-item and payload totals without walking
  the queued dirty-write items repeatedly before submit/result construction
- successful merges update only payload bytes; new queue items update both
  counters. The existing slot accounting remains the source for `used_slots`.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.066, async-many/blocking-parallel 1.061,
  async-thread-many/blocking-parallel 1.019,
  async-thread-batch/blocking-parallel 1.039,
  async-batch-callback/blocking-parallel 1.099, and
  async-loop/blocking-parallel 1.041. The simple threaded and batch samples were
  below blocking-parallel in this run, matching the existing benchmark noise.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed. A subsequent
  `mdbx_migration_bench_lazy_repeat` attempt failed on the first pair because
  forced/default `iterate` was 0.669 against the 0.700 gate; two following
  paired lazy samples passed with forced/default `iterate` at 0.720 and 0.728.
  This appears to be the same read-phase noise seen in earlier checkpoints, but
  it is recorded as a failed repeat gate, not hidden.
- latest passing explicit default sample:

| phase | earlier mapped avg | current explicit default | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 959.275 ops/s | 0.928 |
| crud | 55.675 Kops/s | 50.531 Kops/s | 0.908 |
| iterate | 26.143 Mops/s | 31.176 Mops/s | 1.193 |
| get | 279.409 Kops/s | 479.021 Kops/s | 1.714 |
| delete | 66.398 Kops/s | 57.790 Kops/s | 0.870 |

- latest passing explicit forced no-map sample:

| phase | earlier no-map avg | current explicit forced | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 973.712 ops/s | 0.894 |
| crud | 59.787 Kops/s | 50.193 Kops/s | 0.840 |
| iterate | 25.827 Mops/s | 22.702 Mops/s | 0.879 |
| get | 274.039 Kops/s | 367.657 Kops/s | 1.342 |
| delete | 69.067 Kops/s | 58.540 Kops/s | 0.848 |

- conclusion: cached queue accounting removes repeated dirty-queue scans from
  the write submit path and keeps the normal migration gates passing. It does
  not close the write-heavy pre-migration baseline gap, and the forced no-map
  read phases remain noisy enough that the repeat benchmark gate can still fail.

Additional dirty-write completion validation checkpoint:

- removed the redundant base queued-write validation from
  `dxb_data_write_subrange_io()`. The completion walker only builds subranges
  from queue items already validated on add/merge and before write submit, so
  this keeps subrange-local alignment and bounds checks without revalidating the
  entire queued descriptor for every completion slice.
- removed the duplicate caller-side cache-invalidate validation from
  `iov_callback4dirtypages()`. The submit boundary still validates
  `dxb_cache_invalidate_io_t`, and the callback keeps the page-equality and
  reusable-cache sanity checks before submit.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.147, async-many/blocking-parallel 1.133,
  async-thread/blocking-parallel 1.066,
  async-thread-many/blocking-parallel 1.085,
  async-thread-batch/blocking-parallel 1.113,
  async-batch/blocking-parallel 1.137,
  async-batch-callback/blocking-parallel 1.147,
  async-loop/blocking-parallel 1.112,
  async-thread-loop/blocking-parallel 1.103,
  async-cursor/blocking-parallel 1.795, and
  async-loop-cursor/blocking-parallel 1.579.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 940.906 ops/s, crud 50.589 Kops/s, iterate 32.024 Mops/s,
  get 468.460 Kops/s, delete 58.526 Kops/s; forced no-map batch
  958.986 ops/s, crud 49.348 Kops/s, iterate 32.183 Mops/s,
  get 474.799 Kops/s, delete 58.395 Kops/s. Forced/default ratios were batch
  1.019, crud 0.975, iterate 1.005, get 1.014, delete 0.998.
- `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed all three
  paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 956.101 ops/s | 0.925 |
| crud | 55.675 Kops/s | 50.017 Kops/s | 0.898 |
| iterate | 26.143 Mops/s | 31.484 Mops/s | 1.204 |
| get | 279.409 Kops/s | 475.203 Kops/s | 1.701 |
| delete | 66.398 Kops/s | 58.544 Kops/s | 0.882 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 964.819 ops/s | 0.885 |
| crud | 59.787 Kops/s | 50.127 Kops/s | 0.838 |
| iterate | 25.827 Mops/s | 30.958 Mops/s | 1.199 |
| get | 274.039 Kops/s | 470.939 Kops/s | 1.719 |
| delete | 69.067 Kops/s | 58.293 Kops/s | 0.844 |

- conclusion: trimming duplicate completion-path validation preserves the
  strong read-heavy average wins, and the forced no-map read noise improved in
  this repeat. Batch, CRUD, and delete remain below the pre-migration baseline.

Additional dirty-write completion batching checkpoint:

- added per-queue dirty-write cache invalidation tracking in `iov_ctx`. Dirty
  queue completion now invalidates the non-reusable page-cache span once before
  walking written pages, while the existing per-segment invalidation remains as
  a fallback if the batched range is not available.
- removed additional caller-side dirty-write queue validation in `iov_write()`,
  `iov_complete()`, and `iov_page()`. Constructors and submit boundaries still
  validate queued writes before enqueue, write submit, queue walk, and cache
  invalidation.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.109, async-many/blocking-parallel 1.101,
  async-thread/blocking-parallel 0.868,
  async-thread-many/blocking-parallel 1.019,
  async-thread-batch/blocking-parallel 1.015,
  async-batch/blocking-parallel 1.090,
  async-batch-callback/blocking-parallel 1.090,
  async-loop/blocking-parallel 1.070,
  async-thread-loop/blocking-parallel 1.019,
  async-cursor/blocking-parallel 1.146, and
  async-loop-cursor/blocking-parallel 1.364. The simple threaded async sample
  was below blocking-parallel in this run.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 962.592 ops/s, crud 50.431 Kops/s, iterate 31.785 Mops/s,
  get 479.428 Kops/s, delete 58.347 Kops/s; forced no-map batch
  961.043 ops/s, crud 50.967 Kops/s, iterate 32.363 Mops/s,
  get 405.170 Kops/s, delete 58.900 Kops/s. Forced/default ratios were batch
  0.998, crud 1.011, iterate 1.018, get 0.845, delete 1.009.
- `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed all three
  paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 964.873 ops/s | 0.934 |
| crud | 55.675 Kops/s | 50.718 Kops/s | 0.911 |
| iterate | 26.143 Mops/s | 25.159 Mops/s | 0.962 |
| get | 279.409 Kops/s | 454.421 Kops/s | 1.626 |
| delete | 66.398 Kops/s | 59.080 Kops/s | 0.890 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 976.631 ops/s | 0.896 |
| crud | 59.787 Kops/s | 50.794 Kops/s | 0.850 |
| iterate | 25.827 Mops/s | 32.121 Mops/s | 1.244 |
| get | 274.039 Kops/s | 476.292 Kops/s | 1.738 |
| delete | 69.067 Kops/s | 59.024 Kops/s | 0.855 |

- conclusion: batched cache invalidation and validation trimming moved the
  write-heavy repeat averages modestly upward compared with the prior
  checkpoint, especially in forced no-map mode. They do not close the
  pre-migration write-heavy gap, and the default iterate samples remain noisy.

Additional dirty-write backend validation checkpoint:

- split `osal_ioring_add()` into a validating wrapper and a trusted
  `osal_ioring_add_validated()` path used by
  `dxb_storage_submit_add_validated_queued_write()`. The validated path skips
  repeated queued data-write validation during enqueue and merge after the
  storage constructor has produced the descriptor.
- removed the immediate storage-level revalidation of the just-constructed
  `dxb_queued_write_io_t` in `dxb_storage_write_queued()`. The OSAL write
  boundary still validates the submit descriptor and queue counters.
- moved queued write-item descriptor validation in the sync and io_uring write
  backends behind `MDBX_CHECKING > 0 || MDBX_DEBUG > 0`, so release builds trust
  the internally constructed queue items while debug/checking builds still
  verify them.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.109, async-many/blocking-parallel 1.054,
  async-thread/blocking-parallel 1.053,
  async-thread-many/blocking-parallel 1.026,
  async-thread-batch/blocking-parallel 1.025,
  async-batch/blocking-parallel 0.924,
  async-batch-callback/blocking-parallel 0.832,
  async-loop/blocking-parallel 1.054,
  async-thread-loop/blocking-parallel 1.049,
  async-cursor/blocking-parallel 1.168, and
  async-loop-cursor/blocking-parallel 1.013. Batch and callback read samples
  were below blocking-parallel in this run.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 974.029 ops/s, crud 50.479 Kops/s, iterate 30.243 Mops/s,
  get 477.230 Kops/s, delete 58.727 Kops/s; forced no-map batch
  956.454 ops/s, crud 50.570 Kops/s, iterate 27.451 Mops/s,
  get 474.441 Kops/s, delete 59.069 Kops/s. Forced/default ratios were batch
  0.982, crud 1.002, iterate 0.908, get 0.994, delete 1.006.
- `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed all three
  paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 953.109 ops/s | 0.922 |
| crud | 55.675 Kops/s | 50.379 Kops/s | 0.905 |
| iterate | 26.143 Mops/s | 28.810 Mops/s | 1.102 |
| get | 279.409 Kops/s | 468.403 Kops/s | 1.676 |
| delete | 66.398 Kops/s | 58.813 Kops/s | 0.886 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 967.739 ops/s | 0.888 |
| crud | 59.787 Kops/s | 50.293 Kops/s | 0.841 |
| iterate | 25.827 Mops/s | 30.636 Mops/s | 1.186 |
| get | 274.039 Kops/s | 468.368 Kops/s | 1.709 |
| delete | 69.067 Kops/s | 58.620 Kops/s | 0.849 |

- conclusion: release-build validation overhead is lower in the trusted
  dirty-write queue path and all gates pass, but the repeat averages remain in
  the same band as the prior checkpoint rather than proving a write-heavy
  benchmark win. Batch, CRUD, and delete are still below the pre-migration
  baseline.

Additional dirty-write queue walk fast-path checkpoint:

- dirty-write queue prepare now uses the validated descriptor produced by
  `dxb_storage_make_dirty_write_queue_io()` directly, instead of immediately
  revalidating the queue and its submit wrapper before `osal_ioring_prepare()`.
- the dirty-write completion walker now passes full-item descriptors directly
  to the callback when the walked segment covers the whole queued write item,
  avoiding subrange descriptor reconstruction for the common one-item path.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.095, async-many/blocking-parallel 1.082,
  async-thread/blocking-parallel 1.048,
  async-thread-many/blocking-parallel 1.065,
  async-thread-batch/blocking-parallel 1.073,
  async-batch/blocking-parallel 1.128,
  async-batch-callback/blocking-parallel 1.076,
  async-loop/blocking-parallel 1.085,
  async-thread-loop/blocking-parallel 1.033,
  async-cursor/blocking-parallel 1.126, and
  async-loop-cursor/blocking-parallel 1.180.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 961.859 ops/s, crud 50.334 Kops/s, iterate 22.974 Mops/s,
  get 470.282 Kops/s, delete 59.502 Kops/s; forced no-map batch
  972.569 ops/s, crud 51.608 Kops/s, iterate 21.940 Mops/s,
  get 463.462 Kops/s, delete 59.602 Kops/s. Forced/default ratios were batch
  1.011, crud 1.025, iterate 0.955, get 0.985, delete 1.002.
- `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed all three
  paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 945.895 ops/s | 0.915 |
| crud | 55.675 Kops/s | 51.041 Kops/s | 0.917 |
| iterate | 26.143 Mops/s | 27.504 Mops/s | 1.052 |
| get | 279.409 Kops/s | 457.695 Kops/s | 1.638 |
| delete | 66.398 Kops/s | 59.590 Kops/s | 0.897 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 942.945 ops/s | 0.865 |
| crud | 59.787 Kops/s | 51.196 Kops/s | 0.856 |
| iterate | 25.827 Mops/s | 31.312 Mops/s | 1.212 |
| get | 274.039 Kops/s | 475.739 Kops/s | 1.736 |
| delete | 69.067 Kops/s | 59.414 Kops/s | 0.860 |

- conclusion: the completion-walk fast path and queue-prepare validation trim
  keep the gates passing and improve CRUD/delete averages versus the prior
  checkpoint. Batch remains below the pre-migration baseline and regressed in
  this repeat sample, so the write-heavy migration gap is still open.

Additional sync-submit validation checkpoint:

- added a trusted sync-submit path for `dxb_sync_io_t` descriptors that were
  just built by `dxb_storage_make_sync_io()` or
  `dxb_storage_make_meta_sync_io()`.
- commit, pre-sync, and meta-sync paths now avoid immediately revalidating the
  freshly constructed sync descriptor and its submit wrapper before issuing
  `osal_ioring_fsync()`.
- the old validating submit wrapper was removed after all internal callers were
  converted to the trusted path.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.086, async-many/blocking-parallel 1.077,
  async-thread/blocking-parallel 1.039,
  async-thread-many/blocking-parallel 1.054,
  async-thread-batch/blocking-parallel 1.082,
  async-batch/blocking-parallel 1.088,
  async-batch-callback/blocking-parallel 1.088,
  async-loop/blocking-parallel 1.049, and
  async-thread-loop/blocking-parallel 1.070. Cursor samples were below
  blocking-parallel in this run.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 961.813 ops/s, crud 50.920 Kops/s, iterate 31.169 Mops/s,
  get 468.513 Kops/s, delete 59.141 Kops/s; forced no-map batch
  959.590 ops/s, crud 50.388 Kops/s, iterate 31.603 Mops/s,
  get 471.354 Kops/s, delete 59.238 Kops/s. Forced/default ratios were batch
  0.998, crud 0.990, iterate 1.014, get 1.006, delete 1.002.
- two `make -f GNUmakefile mdbx_migration_bench_lazy_repeat` attempts failed
  first on forced/default iterate 0.693, then on forced/default iterate 0.696,
  both just below the 0.700 gate. A third repeat run passed all three paired
  samples. The passing repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 939.804 ops/s | 0.910 |
| crud | 55.675 Kops/s | 50.782 Kops/s | 0.912 |
| iterate | 26.143 Mops/s | 28.618 Mops/s | 1.095 |
| get | 279.409 Kops/s | 468.312 Kops/s | 1.676 |
| delete | 66.398 Kops/s | 59.014 Kops/s | 0.889 |

- passing repeat averages for current explicit forced no-map against the
  earlier no-map baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 968.925 ops/s | 0.889 |
| crud | 59.787 Kops/s | 50.989 Kops/s | 0.853 |
| iterate | 25.827 Mops/s | 29.178 Mops/s | 1.130 |
| get | 274.039 Kops/s | 462.668 Kops/s | 1.688 |
| delete | 69.067 Kops/s | 59.142 Kops/s | 0.856 |

- conclusion: sync-submit validation overhead is lower and all functional gates
  pass. The write-heavy benchmark remains in the same noisy band: forced batch
  improved versus the prior checkpoint, but CRUD/delete remain below the
  pre-migration baseline and the repeat gate showed near-threshold iterate
  noise before a passing run.

Additional meta-write submit validation checkpoint:

- added a trusted meta-write submit path for descriptors just produced by
  `dxb_storage_make_meta_payload_write_io()` or
  `dxb_storage_make_meta_page_write_io()`.
- commit meta update, undo meta rewrite, steady-meta wipe, and meta override
  now submit those freshly built descriptors without immediately rebuilding and
  revalidating their submit wrappers.
- the validating meta-write descriptor helper remains in use for meta-shadow
  copy assertions, while the write submit hot path trusts constructor output.
- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench mdbx_migration_smoke`: passed
- `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
- `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000` spot check reported
  async/blocking-parallel 1.116, async-many/blocking-parallel 1.128,
  async-thread/blocking-parallel 1.103,
  async-thread-many/blocking-parallel 1.084,
  async-thread-batch/blocking-parallel 1.090,
  async-batch/blocking-parallel 1.148,
  async-batch-callback/blocking-parallel 1.139,
  async-loop/blocking-parallel 1.111,
  async-thread-loop/blocking-parallel 1.091,
  async-cursor/blocking-parallel 2.273, and
  async-loop-cursor/blocking-parallel 1.353.
- `make -f GNUmakefile mdbx_migration_bench_lazy`: passed with default
  explicit batch 959.729 ops/s, crud 50.344 Kops/s, iterate 22.798 Mops/s,
  get 445.080 Kops/s, delete 58.744 Kops/s; forced no-map batch
  959.358 ops/s, crud 50.866 Kops/s, iterate 29.058 Mops/s,
  get 467.963 Kops/s, delete 59.050 Kops/s. Forced/default ratios were batch
  1.000, crud 1.010, iterate 1.275, get 1.051, delete 1.005.
- `make -f GNUmakefile mdbx_migration_bench_lazy_repeat`: passed all three
  paired samples. The repeat averages for current explicit default were:

| phase | earlier mapped avg | current explicit default avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1033.245 ops/s | 947.369 ops/s | 0.917 |
| crud | 55.675 Kops/s | 50.910 Kops/s | 0.914 |
| iterate | 26.143 Mops/s | 25.404 Mops/s | 0.972 |
| get | 279.409 Kops/s | 451.442 Kops/s | 1.616 |
| delete | 66.398 Kops/s | 59.080 Kops/s | 0.890 |

- repeat averages for current explicit forced no-map against the earlier no-map
  baseline were:

| phase | earlier no-map avg | current explicit forced avg | ratio |
| --- | ---: | ---: | ---: |
| batch | 1089.750 ops/s | 966.513 ops/s | 0.887 |
| crud | 59.787 Kops/s | 50.784 Kops/s | 0.849 |
| iterate | 25.827 Mops/s | 31.281 Mops/s | 1.211 |
| get | 274.039 Kops/s | 435.297 Kops/s | 1.588 |
| delete | 69.067 Kops/s | 59.155 Kops/s | 0.856 |

- conclusion: meta-write submit validation overhead is lower and all gates pass.
  The write-heavy result remains below the pre-migration baseline for batch,
  CRUD, and delete. This checkpoint slightly improves default batch versus the
  previous passing repeat but does not close the migration gap.

Additional async write benchmark coverage checkpoint:

- profiled the remaining forced no-map CRUD gap with `perf record` against
  `ioarena -D mdbx -B crud -m lazy -n 10000`. The hot path is dominated by
  kernel buffered write calls from `dxb_storage_write_queued()`:
  `txn_basal_commit()` -> `txn_write()` -> `iov_write()` ->
  `dxb_storage_write_queued()` accounted for about 25% of samples, with
  `pwrite`/`pwritev` syscall paths accounting for most of that subtree.
- a direct `MDBX_EXPLICIT_IO_BACKEND=io_uring` spot comparison on the same
  small transactional CRUD pattern was slower on this host, so the next change
  did not make io_uring automatic for the default backend.
- extended `ut_and_examples/async-api-bench.c` beyond read throughput: it now
  accepts `MDBX_ASYNC_BENCH_WRITE_OPS` and reports a bounded single-writer
  transaction benchmark comparing blocking `mdbx_put()` with windowed
  `mdbx_async_put()` submissions on one async executor. This keeps the blocking
  API unchanged and makes write-operation async API overhead visible in the
  same public benchmark harness as GET and cursor scans.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-covered=132 exempt=39 missing=0`
- reduced benchmark sanity check:
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking write put
  2.628 Mops/s, async write put 2.288 Mops/s, async-put/blocking-put 0.871.
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000
  MDBX_ASYNC_BENCH_WRITE_OPS=20000` spot check reported
  async/blocking-parallel 1.183, async-many/blocking-parallel 1.117,
  async-thread/blocking-parallel 1.123,
  async-thread-many/blocking-parallel 1.119,
  async-thread-batch/blocking-parallel 1.159,
  async-batch/blocking-parallel 1.186,
  async-batch-callback/blocking-parallel 1.190,
  async-loop/blocking-parallel 1.138,
  async-thread-loop/blocking-parallel 1.151,
  async-cursor/blocking-parallel 1.399,
  async-loop-cursor/blocking-parallel 1.766, and
  async-put/blocking-put 0.736.
- conclusion: read and cursor async benchmarks remain above blocking-parallel
  in the forced no-map spot check. The new write benchmark confirms that
  single-writer async `put` is currently slower than direct blocking `put`,
  which matches the serialized write model and the profile showing kernel
  write submission as the remaining cost rather than public API coverage.

Additional async put-batch benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` to exercise the existing
  `mdbx_async_put_batch()` public API separately from windowed single-item
  `mdbx_async_put()` submissions.
- added `MDBX_ASYNC_BENCH_WRITE_BATCH`, defaulting to 1024, so write batching
  can be tuned independently from the read submission window.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking write put
  2.400 Mops/s, async write put 2.279 Mops/s, async batch write put
  2.462 Mops/s, async-put/blocking-put 0.950,
  async-put-batch/blocking 1.026, and async-put-batch/async-put 1.080.
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000
  MDBX_ASYNC_BENCH_WRITE_OPS=20000` spot check reported
  async/blocking-parallel 1.129, async-many/blocking-parallel 1.127,
  async-thread/blocking-parallel 1.098,
  async-thread-many/blocking-parallel 1.056,
  async-thread-batch/blocking-parallel 1.065,
  async-batch/blocking-parallel 1.126,
  async-batch-callback/blocking-parallel 1.119,
  async-loop/blocking-parallel 1.109,
  async-thread-loop/blocking-parallel 1.090,
  async-cursor/blocking-parallel 1.200,
  async-loop-cursor/blocking-parallel 1.385,
  async-put/blocking-put 0.816,
  async-put-batch/blocking 0.922, and async-put-batch/async-put 1.130.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-covered=132 exempt=39 missing=0`
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: `mdbx_async_put_batch()` materially reduces async write API
  overhead versus per-item async submission. The smaller reduced run can edge
  ahead of blocking writes, while the larger forced no-map write sample remains
  below blocking but improves from 0.816 to 0.922 of blocking throughput.

Additional async delete benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` to measure delete operations
  alongside write puts: blocking `mdbx_del()`, windowed per-item
  `mdbx_async_del()`, and batched `mdbx_async_del_batch()`.
- delete benchmarks reseed the default table before each variant and delete
  sequential keys, avoiding randomized read-key repeats that can make delete
  workloads hit already-removed keys.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking delete 2.029 Mops/s,
  async delete 1.522 Mops/s, async batch delete 2.144 Mops/s,
  async-del/blocking-del 0.750, async-del-batch/blocking 1.057, and
  async-del-batch/async-del 1.408.
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000
  MDBX_ASYNC_BENCH_WRITE_OPS=20000` spot check reported
  async/blocking-parallel 1.080, async-many/blocking-parallel 1.081,
  async-thread/blocking-parallel 1.041,
  async-thread-many/blocking-parallel 0.983,
  async-thread-batch/blocking-parallel 1.092,
  async-batch/blocking-parallel 1.107,
  async-batch-callback/blocking-parallel 1.104,
  async-loop/blocking-parallel 1.072,
  async-thread-loop/blocking-parallel 1.027,
  async-put/blocking-put 0.854,
  async-put-batch/blocking 0.946,
  async-put-batch/async-put 1.108,
  async-del/blocking-del 0.832,
  async-del-batch/blocking 0.970, and async-del-batch/async-del 1.166.
  Cursor samples were noisy in this spot run, with async cursor ratios below
  blocking-parallel, so this remains a benchmark observation rather than a
  pass/fail gate.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-covered=132 exempt=39 missing=0`
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: batched async delete materially reduces async delete API
  overhead versus per-item async submission. The reduced run beats blocking,
  and the larger forced no-map sample nearly reaches blocking throughput
  (0.970) while improving 16.6% over per-item async delete.

Additional async replace-batch API checkpoint:

- added `mdbx_async_replace_batch()` as a public async-only batch helper for
  repeated `mdbx_replace()` operations. The blocking API is unchanged. The new
  helper follows the existing put/delete batch contract: caller-owned key,
  new-data, old-data, and result arrays must remain valid until completion,
  each `results` slot receives the corresponding `mdbx_replace()` result, and
  the async operation result is `MDBX_SUCCESS` after the batch has run.
- wired the new operation through the async executor (`async_op_replace_batch`)
  and added smoke coverage that replaces two existing records, verifies the
  previous values, then restores the original values before the existing final
  readback assertions.
- extended `ut_and_examples/async-api-bench.c` to report blocking replace,
  per-item async replace, and batched async replace.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace 2.552 Mops/s,
  async replace 1.778 Mops/s, async batch replace 2.515 Mops/s,
  async-replace/blocking 0.697, async-repl-batch/blocking 0.985, and
  async-repl-batch/async 1.415.
- forced no-map/tiny-cache `MDBX_ASYNC_BENCH_OPS=300000
  MDBX_ASYNC_BENCH_WRITE_OPS=20000` spot check reported
  async/blocking-parallel 1.188, async-batch/blocking-parallel 1.189,
  async-put/blocking-put 0.759, async-put-batch/blocking 0.992,
  async-put-batch/async-put 1.308, async-replace/blocking 0.748,
  async-repl-batch/blocking 0.943, async-repl-batch/async 1.261,
  async-del/blocking-del 0.765, async-del-batch/blocking 0.970, and
  async-del-batch/async-del 1.268. Cursor samples were again below
  blocking-parallel in this spot run and remain noisy.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-covered=132 exempt=39 missing=0`
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: batched async replace materially reduces async replace overhead
  versus per-item async submission. The reduced run nearly matches blocking
  replace, and the larger forced no-map sample improves async replace throughput
  by 26.1% over per-item async replace.

Additional async API audit accounting checkpoint:

- updated `mdbx_async_api_audit` to track actual public async declarations
  separately from their mapped blocking counterparts. This keeps the existing
  pass/fail rule for blocking API coverage while making async-only public
  helpers visible in the report.
- the audit now reports async-only additions such as the batch helpers instead
  of hiding them behind the blocking-name coverage count.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-declared=177 async-covered=132 async-only=45 exempt=39 missing=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the public async surface now has an explicit audit signal for
  both blocking counterparts and async-only API growth, which makes future
  coverage work less ambiguous.

Additional async replace-ex batch API checkpoint:

- added `mdbx_async_replace_ex_batch()` as a public async-only batch helper for
  repeated `mdbx_replace_ex()` operations with a shared preservation callback
  and callback context. The blocking API is unchanged.
- reused the replace-batch executor storage and dispatch path, selecting
  `mdbx_replace()` or `mdbx_replace_ex()` by async opcode. Per-item result
  slots receive the corresponding replace result, while the async operation
  result remains `MDBX_SUCCESS` after the batch has run.
- added smoke coverage that dirties two records, submits
  `mdbx_async_replace_ex_batch()`, verifies the preservation callback ran once
  per item, checks the retrieved dirty values, and restores the original values
  before the later readback/deletion checks.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace 2.562 Mops/s,
  async replace 1.732 Mops/s, async batch replace 2.427 Mops/s,
  async-replace/blocking 0.676, async-repl-batch/blocking 0.947, and
  async-repl-batch/async 1.402.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h`: `blocking=171 async-declared=178 async-covered=132 async-only=46 exempt=39 missing=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|migration_smoke)'`: passed 9/9
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_smoke mdbx_async_api_audit mdbx_async_api_bench`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the async API now has a batched preservation-callback form for
  the remaining replace variant, and the existing replace-batch benchmark stays
  in the same near-parity band after sharing the dispatch path.

Additional async API implementation audit checkpoint:

- extended `mdbx_async_api_audit` with an optional source-file check. When
  `mdbx.c` is provided, every public `mdbx_async_*` declaration in `mdbx.h`
  must have a matching function definition in the source, not just a mapped
  blocking counterpart.
- wired the CMake `async_api_audit` test to pass both `mdbx.h` and `mdbx.c`,
  so normal CTest coverage now fails on declared-but-unimplemented async APIs.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=178 async-covered=132 async-only=46 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api_audit$'`: passed 1/1
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the public async API audit now verifies declaration coverage and
  implementation presence, reducing the chance that future async API expansion
  leaves a header-only stub behind.

Additional async cursor-put batch API checkpoint:

- added `mdbx_async_cursor_put_batch()` as a public async-only batch helper for
  repeated `mdbx_cursor_put()` operations through one cursor. The blocking API
  is unchanged.
- the helper follows the existing batch contract: caller-owned `keys`, `data`,
  and `results` arrays and pointed-to bytes must remain valid until
  completion, each `results` slot receives the corresponding cursor-put result,
  and the async operation result is `MDBX_SUCCESS` after the batch has run. It
  rejects `MDBX_RESERVE` and `MDBX_MULTIPLE`, matching the single async cursor
  put and DBI put-batch constraints.
- added smoke coverage that inserts two new records with
  `mdbx_async_cursor_put_batch()` and reads them back through the same cursor.
- extended `ut_and_examples/async-api-bench.c` with blocking cursor put,
  per-item async cursor put, and async cursor-put batch measurements.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor put 3.905 Mops/s,
  async cursor put 2.380 Mops/s, async cursor batch put 3.703 Mops/s,
  async-cursor-put-batch/block 0.948, and
  async-cursor-put-batch/async 1.556.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cursor writes now have a batched async API. The reduced benchmark
  shows the batch form nearly reaches blocking cursor-put throughput and
  improves throughput by 55.6% versus per-item async cursor puts.

Additional cursor range-delete benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with blocking and async cursor
  range-delete measurements using `mdbx_cursor_delete_range()` and
  `mdbx_async_cursor_delete_range()`.
- the benchmark positions the end cursor before timing and uses the public
  `begin == NULL` contract to delete from the first item through the positioned
  end cursor. This measures the range-delete operation itself rather than the
  setup scan used to choose the endpoint.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor range delete
  33.923 Mops/s, async cursor range delete 50.039 Mops/s, and
  async-cursor-range/block 1.475.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the existing async cursor range-delete API now has benchmark
  coverage, and the reduced sample shows the async wrapper path above the
  blocking range-delete measurement for this range workload.

Additional cursor bunch-delete benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with blocking and async cursor
  bunch-delete measurements using `mdbx_cursor_bunch_delete()` and
  `mdbx_async_cursor_bunch_delete()` with `MDBX_DELETE_AFTER_INCLUDING`.
- the benchmark positions the cursor before timing and normalizes throughput by
  the returned affected-item count. This keeps the benchmark aligned with the
  bunch-delete API contract instead of assuming a fixed affected count for all
  actions and build configurations.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor bunch delete
  150.091 Mops/s, async cursor bunch delete 81.125 Mops/s, and
  async-cursor-bunch/block 0.541.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the existing async cursor bunch-delete API now has benchmark
  coverage. This reduced sample shows the async wrapper below blocking for this
  single-operation bunch-delete case, which gives a concrete follow-up target
  for cursor mutation overhead work.

Additional replace-ex benchmark coverage checkpoint:

- extended `ut_and_examples/async-api-bench.c` with blocking
  `mdbx_replace_ex()`, per-item `mdbx_async_replace_ex()`, and batched
  `mdbx_async_replace_ex_batch()` measurements using the same preservation
  callback semantics as the async smoke test.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace_ex
  2.561 Mops/s, async replace_ex 1.774 Mops/s, async batch replace_ex
  2.408 Mops/s, async-replace-ex/blocking 0.693,
  async-repl-ex-batch/block 0.940, and async-repl-ex-batch/async 1.357.
- comparison against the pre-migration ioarena lazy baselines remains
  workload-specific. The latest logged repeat benchmark has current explicit
  default get at 451.442 Kops/s versus earlier mapped get at 279.409 Kops/s
  (1.616x), and current explicit forced no-map get at 435.297 Kops/s versus
  earlier no-map get at 274.039 Kops/s (1.588x). Iterate is mixed to better:
  default 25.404 Mops/s versus 26.143 Mops/s (0.972x), forced no-map
  31.281 Mops/s versus 25.827 Mops/s (1.211x). Write-heavy ioarena phases
  remain below the old baseline: default batch/crud/delete are 0.917x,
  0.914x, and 0.890x; forced no-map batch/crud/delete are 0.887x, 0.849x,
  and 0.856x.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the async benchmark now covers both replace preservation paths.
  Batch `replace_ex` recovers most of the per-item async overhead in the reduced
  sample, but write-heavy end-to-end ioarena results still trail the
  pre-migration baseline and remain the main performance gap.

Additional GET-family batch benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with parallel batch measurements
  for `mdbx_async_get_ex_batch()` and
  `mdbx_async_get_equal_or_great_batch()`. These use the same multi-executor
  windowing shape as the existing plain `mdbx_async_get_batch()` benchmark, so
  the output now covers the extended GET variants without changing the blocking
  API.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  554.779 Kops/s, async batch parallel get 891.385 Kops/s,
  async get_ex batch 1.091 Mops/s, and async lowerbound batch
  989.825 Kops/s. Ratios were async-batch/blocking-parallel 1.607,
  async-get-ex-batch/par 1.966, async-lower-batch/par 1.784,
  async-get-ex-batch/batch 1.224, and async-lower-batch/batch 1.110.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the public benchmark now exercises the extended async GET family
  under the same parallel-heavy shape as plain GET. In this reduced sample,
  both extended batch paths beat the benchmark's blocking-parallel GET baseline
  and the plain async batch GET line.

Additional extended GET many-submit benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with parallel many-submit
  measurements for `mdbx_async_get_ex_many()` and
  `mdbx_async_get_equal_or_great_many()`. The benchmark now distinguishes the
  independent-operation many-submit shape from the single-operation batch shape
  for both extended GET variants.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  992.247 Kops/s, async get_ex batch 743.130 Kops/s, async get_ex many
  613.682 Kops/s, async lowerbound batch 953.348 Kops/s, and async lowerbound
  many 725.086 Kops/s. This sample was noisy against blocking-parallel GET,
  but the within-shape comparisons were clear: async-get-ex-batch/many 1.211
  and async-lower-batch/many 1.315.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: extended GET benchmark coverage now includes both public async
  submission styles. In this reduced sample the single-operation batch APIs
  remain faster than the many-submit APIs for the extended GET variants, which
  gives a concrete direction for callers that can keep arrays live until batch
  completion.

Additional extended GET worker-loop benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with parallel worker-side loop
  measurements for `mdbx_async_get_ex_loop()` and
  `mdbx_async_get_equal_or_great_loop()`, using the same key generation and
  worker split as the existing `mdbx_async_get_loop()` benchmark.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  624.083 Kops/s, async get loop 927.023 Kops/s, async get_ex loop
  919.046 Kops/s, and async lowerbound loop 735.121 Kops/s. Ratios were
  async-loop/blocking-parallel 1.485, async-get-ex-loop/par 1.473, and
  async-lower-loop/par 1.178.
- in this sample, the worker-side extended loops were the strongest extended
  GET shape: async-get-ex-batch/loop 0.491 and async-lower-batch/loop 0.840.
  That keeps the benchmark evidence aligned with the original parallel-heavy
  GET goal while documenting the expected caller tradeoff: callback-driven loop
  APIs avoid per-item handle traffic but require worker-thread callbacks.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the benchmark now covers all public async GET submission shapes
  for plain, extended, and lower-bound reads: per-item, many-submit, batch,
  callback batch, and worker-side loop. The extended loop paths are above the
  blocking-parallel GET sample here, but full goal completion remains open
  because write-heavy end-to-end migration benchmarks still trail the old
  baseline in prior logged runs.

Additional cursor-delete benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with blocking
  `mdbx_cursor_del()` and async `mdbx_async_cursor_del()` measurements. The
  benchmark positions a cursor at the first record, deletes the current record
  with `MDBX_CURRENT`, and uses the documented post-delete `MDBX_GET_CURRENT`
  cursor contract to continue over the next effective record.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor delete
  4.002 Mops/s, async cursor delete 158.862 Kops/s,
  async-cursor-del/block 0.040, and async-cursor-del/async-del 0.107.
- this is intentionally a sequential cursor-mutation shape. It shows the
  current per-step executor round trip is expensive for basic cursor deletion,
  unlike the coarser cursor range-delete and bunch-delete APIs that perform
  more work per submitted async operation.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=179 async-covered=132 async-only=47 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: basic cursor deletion now has direct benchmark coverage. The
  result highlights another concrete optimization/API-design target for
  cursor-heavy write workloads: callers need coarser submitted cursor mutation
  shapes, or the per-operation cursor executor path needs lower latency.
