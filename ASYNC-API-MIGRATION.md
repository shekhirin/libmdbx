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
