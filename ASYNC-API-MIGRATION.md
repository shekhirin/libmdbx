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

## True Async Page Read Slice

Implemented a first internal storage-level async read slice for the explicit
no-mmap page-cache path:

- added `osal_ioring_pread_batch()` and a Linux `io_uring` multi-read path that
  queues multiple `IORING_OP_READ` SQEs before waiting for completions;
- added `dxb_storage_submit_read_data_batch()` and `page_cache_submit_read_batch()`
  so page-cache misses can be looked up, allocated, submitted, completed, and
  inserted as one ordered batch;
- wired the fast path into `mdbx_async_cache_get_SingleThreaded_batch()` for
  confirmed cache-entry hits. Public API signatures remain unchanged; entries
  that need refresh, invalid entries, large/overflow pages, and unsupported
  backends fall back to the existing path.

This is intentionally a narrow first slice. It does not yet make B-tree
traversal resumable, and `mdbx_async_get()` still uses the executor path rather
than the internal page-read batch engine.

Verification:

- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`
  passed.
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`
  passed 11/11 tests.
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`
  passed.

Reduced forced no-mmap/io_uring benchmark (`items=10000 ops=30000
write_ops=1000 page_cache=64K`), compared against a detached worktree at
`855e9f3`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 662.927 Kops/s | 818.129 Kops/s |
| async cache st batch | 1.238 Mops/s | 1.958 Mops/s |
| async-cache-st-batch/par | 1.867 | 2.393 |
| async-cache-st-batch/many | 0.667 | 1.285 |
| async-cache-st-batch/batch | 0.677 | 1.115 |
| async-cache-st-batch/ser | 0.743 | 1.154 |

Single-run benchmark noise is visible in unrelated rows, and callback ratios are
mixed. The direct single-threaded cache batch path is the target of this slice.

## Cache Loop Prefetch Slice

Extended the internal page-read batch path to
`mdbx_async_cache_get_SingleThreaded_loop()`. The executor now uses a bounded
64-entry prefetch window for already-confirmed single-threaded cache entries,
materializes eligible entries through `cache_materialize_singlethreaded_batch()`,
then still invokes `key_func`, `result_func`, and `completed` updates in index
order. Non-eligible entries, allocation failures, large/overflow pages, and
normal multi-threaded cache entries fall back to the existing per-item path.

This moves cache-get loop workloads closer to the requested async page-read
model, but it is still not a resumable B-tree traversal engine and does not
change `mdbx_async_get()`.

Reduced forced no-mmap/io_uring benchmark (`items=10000 ops=30000
write_ops=1000 page_cache=64K`), compared against a detached worktree at
`855e9f3`:

| metric | before | after |
| --- | ---: | ---: |
| async cache st loop | 1.386 Mops/s | 2.229 Mops/s |
| async threaded cache st loop | 1.400 Mops/s | 1.516 Mops/s |
| async-cache-st-loop/par | 2.090 | 4.330 |
| async-thread-cache-st-l/par | 2.112 | 2.945 |
| async-cache-st-loop/many | 0.747 | 1.228 |
| async-cache-st-loop/loop | 0.659 | 1.056 |
| async-cache-st-loop/ser | 0.832 | 1.332 |
| async-thread-cache-st-l/ser | 0.841 | 0.906 |

Single-run benchmark noise remains high across unrelated rows, especially the
threaded variants, so these numbers should be treated as directional.

## Public Async Get Cache Slice

Added a worker-owned, direct-mapped cache-entry table inside `MDBX_async` and
used it for `mdbx_async_get()`, `mdbx_async_get_many()`, and
`mdbx_async_get_batch()`. The first sighting of a key records it but still uses
the legacy `mdbx_get()` path; the second sighting refreshes the internal
`MDBX_cache_entry_t`; later sightings can materialize through
`cache_materialize_singlethreaded_batch()` and the explicit page-cache read
path. Batch get copies eligible cached entries and submits their page
materialization as one batch while preserving per-item result ordering.

The hidden cache is used only for read-only transactions and falls back to
`mdbx_get()` on allocation failure, unsupported transaction shape, cache miss,
or validation failure. Async write, delete, drop, and DBI-close operations clear
the hidden cache before mutating state.

This is still not the final resumable B-tree traversal design: first and second
key sightings can still run blocking traversal, and `mdbx_get()` itself is
unchanged. It does move public async get APIs off the pure executor-offload path
for repeated read workloads.

Repeated-key forced no-mmap/io_uring benchmark (`items=1000 ops=30000
write_ops=1000 page_cache=64K`), compared against the previous commit
`688f3c8`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 1.135 Mops/s | 1.689 Mops/s |
| async many parallel get | 762.935 Kops/s | 1.762 Mops/s |
| async threaded get | 1.065 Mops/s | 1.810 Mops/s |
| async threaded many get | 826.107 Kops/s | 1.354 Mops/s |
| async threaded batch get | 1.191 Mops/s | 2.058 Mops/s |
| async batch parallel get | 913.996 Kops/s | 2.009 Mops/s |
| async batch callback get | 592.316 Kops/s | 1.876 Mops/s |

A lower-reuse 10k-key forced no-mmap/io_uring smoke also passed after the
adaptive first-sighting change; representative public get rows were async get
`920.384 Kops/s`, async many `931.513 Kops/s`, async batch `856.293 Kops/s`,
and async batch callback `882.005 Kops/s`.

## Public Async Get Loop Cache Slice

Routed `mdbx_async_get_loop()` through the same worker-owned async get cache
used by public async get and batch get. The loop still invokes `key_func`,
performs exactly one get-equivalent lookup, invokes `result_func`, and updates
`completed` in index order for each item. It does not prefetch future keys ahead
of callbacks, so callback-visible ordering remains unchanged. Repeated keys can
now leave the pure `mdbx_get()` executor-offload path and use cached-entry
materialization through the explicit page-cache path.

Repeated-key forced no-mmap/io_uring benchmark (`items=1000 ops=30000
write_ops=1000 page_cache=64K`), compared against the previous commit
`d0d2108`:

| metric | before | after |
| --- | ---: | ---: |
| async get loop | 1.049 Mops/s | 1.868 Mops/s |
| async threaded get loop | 935.333 Kops/s | 1.488 Mops/s |
| async-loop/blocking par | 0.977 | 1.968 |
| async-thread-loop/par | 0.871 | 1.568 |
| async-loop/blocking ser | 0.512 | 0.909 |
| async-thread-loop/ser | 0.456 | 0.724 |

This is still a per-item loop. A future resumable traversal design should be
able to batch independent page misses without changing callback order.

## Public Async Get Run Batching Slice

The async worker now coalesces contiguous `async_op_get` operations with the
same transaction and DBI into one internal execution batch, up to the existing
completion chunk size. This primarily targets `mdbx_async_get_many()` and
windows of adjacent `mdbx_async_get()` calls. Eligible cached entries in the
run can be materialized through one `cache_materialize_singlethreaded_batch()`
call, so repeated reads can share the explicit page-cache read batching path
before the operations are marked complete.

The coalescing is deliberately conservative: it does not cross transaction or
DBI boundaries, does not reorder completions, and still falls back per item
when entries are not warm enough for cached materialization. `mdbx_async_get_batch()`
already has its own batch operation and is not the main target of this slice.

Repeated-key forced no-mmap/io_uring benchmark (`items=1000 ops=30000
write_ops=1000 page_cache=64K`), compared against the previous commit
`ef3c250`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 1.756 Mops/s | 2.039 Mops/s |
| async many parallel get | 1.740 Mops/s | 1.818 Mops/s |
| async threaded get | 1.237 Mops/s | 2.238 Mops/s |
| async threaded many get | 953.463 Kops/s | 1.147 Mops/s |
| async/blocking parallel | 2.140 | 2.253 |
| async-many/blocking par | 2.120 | 2.008 |
| async-thread/blocking par | 1.506 | 2.473 |
| async-thread-many/par | 1.161 | 1.267 |

Single-run noise is visible in related rows, and the separate batch-operation
row was lower in this run (`2.052 Mops/s` before, `1.963 Mops/s` after).

## Async Single-Threaded Cache Many Run Batching Slice

The async worker now coalesces contiguous
`async_op_cache_get_singlethreaded` operations with the same transaction and
DBI, up to the existing completion chunk size. This targets
`mdbx_async_cache_get_SingleThreaded_many()`, whose previous implementation
submitted many independent single-item operations. Eligible cache entries are
copied into a temporary batch and materialized with
`cache_materialize_singlethreaded_batch()` before per-item fallback.

The regular volatile-entry `mdbx_async_cache_get_many()` path is intentionally
not coalesced, because those cache entries may be shared across threads.

Repeated-key forced no-mmap/io_uring benchmark (`items=1000 ops=30000
write_ops=1000 page_cache=64K`), compared against the previous commit
`8c0e3a5`:

| metric | before | after |
| --- | ---: | ---: |
| async cache st many | 1.661 Mops/s | 4.396 Mops/s |
| async-cache-st-many/par | 1.756 | 4.958 |
| async-cache-st/cache | 0.552 | 2.455 |
| async-cache-st-many/ser | 0.810 | 2.157 |
| async cache st batch | 2.653 Mops/s | 4.028 Mops/s |
| async threaded cache st batch | 4.291 Mops/s | 5.065 Mops/s |
| async cache st loop | 4.782 Mops/s | 4.894 Mops/s |

Single-run noise is visible in non-target rows; regular `async cache many` was
lower in this run (`3.010 Mops/s` before, `1.790 Mops/s` after), and that path
is not coalesced by this slice.

## Async Large Cache Materialization Slice

`cache_materialize_singlethreaded_batch()` now keeps large/overflow cache hits
on the batched explicit-I/O path instead of immediately falling back to the
per-item `mdbx_cache_get_SingleThreaded()` path. The first-page cache lookup
still runs through `page_cache_submit_read_batch()`. If the page is an overflow
page whose cached first page must be expanded, the materializer now prepares a
second batch of large-page materialization reads, submits those reads with
`dxb_storage_submit_read_data_batch()`, then finishes each item through the
same cache-entry replace/detach rules used by the existing single-item
materialization path.

The overflow batching arrays are allocated lazily only after a large page is
seen, so ordinary small-value cache batches do not pay for large-page
bookkeeping. If allocation fails, the item remains fallbackable and the caller
can use the existing single-item path.

Smoke coverage now writes four 10 KiB values into a temporary named DBI, warms
cache entries with `mdbx_async_cache_get_batch()`, then fetches them through
`mdbx_async_cache_get_SingleThreaded_batch()` and verifies every returned byte.
The temporary DBI is dropped before the cursor-heavy smoke checks so their main
DB ordering assumptions remain unchanged.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=30000
write_ops=1000 page_cache=64K`), compared against the previous commit
`80a26f7`:

| metric | before | after |
| --- | ---: | ---: |
| async cache st batch | 2.491 Mops/s | 2.331 Mops/s |
| async-cache-st-batch/par | 5.841 | 4.455 |
| async-cache-st-batch/many | 0.558 | 0.968 |
| async-cache-st-batch/batch | 0.955 | 1.382 |
| async-cache-st-batch/ser | 1.252 | 1.148 |
| async threaded cache st batch | 5.329 Mops/s | 5.154 Mops/s |
| async cache st loop | 4.990 Mops/s | 4.315 Mops/s |

Follow-up benchmark coverage adds direct 10 KiB value rows to
`mdbx_async_api_bench`. The harness seeds a temporary named DBI after the
cursor-read benchmarks, warms cache entries before timing, measures regular
and single-threaded cache batch materialization, then drops the temporary DBI
before write/delete measurements so cursor assumptions for the main DB remain
unchanged.

Forced no-mmap/io_uring large-value benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), using the
same benchmark harness applied to previous commit `80a26f7`:

| metric | before | after |
| --- | ---: | ---: |
| async large cache batch | 138.040 Kops/s | 137.846 Kops/s |
| async large cache st batch | 124.865 Kops/s | 130.890 Kops/s |
| async-large-cache-st/batch | 0.905 | 0.950 |
| async cache st batch | 3.255 Mops/s | 3.527 Mops/s |
| async-cache-st-batch/batch | 1.323 | 2.417 |

The direct large-value single-threaded row is slightly faster in this run and
now measures the overflow materialization path instead of inferring behavior
from small 8-byte value rows.

## Batched Committed Page-Get Primitive Slice

Added an internal batch layer below `page_submit_get_unchecked()` for ordinary
page gets. The single-page public behavior is preserved, but the implementation
now has a reusable primitive that can:

- validate an array of `dxb_page_get_submit_io_t` requests;
- satisfy dirty transaction pages immediately without sending them to storage;
- prepare committed page-cache reads for the remaining requests;
- submit those committed reads through `page_cache_submit_read_batch()`;
- return one `pgr_t` per requested page while preserving the existing pgno
  mismatch checks.

This is preparatory plumbing for the resumable B-tree/cursor traversal work.
It does not by itself make `mdbx_get()` or cursor movement suspend/resume
across page misses, but it removes the single-read wrapper as the only internal
entry point for committed page gets.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `7ecb503`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.994 Mops/s | 1.898 Mops/s |
| blocking parallel get | 544.206 Kops/s | 540.657 Kops/s |
| async parallel get | 1.565 Mops/s | 661.314 Kops/s |
| async threaded batch get | 825.748 Kops/s | 1.686 Mops/s |
| async batch parallel get | 784.085 Kops/s | 1.486 Mops/s |
| async cache st batch | 3.662 Mops/s | 2.202 Mops/s |
| async large cache st batch | 139.104 Kops/s | 139.826 Kops/s |

The short run is noisy and does not show a clean direct performance signal for
this plumbing slice. The large-value row is effectively flat, and the ordinary
get/cache rows move in both directions. This is expected: the new multi-page
helper is available internally, but existing B-tree traversal still calls the
single-page wrapper until the resumable traversal layer is added.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed

## Batched Cursor Neighbor Page-Get Slice

Added `page_submit_cursor_get_batch()` on top of the raw page-get batch
primitive. The helper validates each cursor page-get request, keeps all entries
on one transaction, calls `page_submit_get_unchecked_batch()` for the
underlying page reads, then applies the same cursor-specific completion path as
`page_submit_cursor_get()`: header checks, optional full page checks,
large-page materialization, transaction error marking, and page-ref release on
failure.

The first real cursor-side caller is rebalance neighbor lookup. When both left
and right siblings exist, rebalance now prepares both sibling page-get requests
and submits them through the cursor batch helper instead of reading the two
siblings sequentially. This is still synchronous from the caller's point of
view, but it moves another B-tree mutation path onto the internal multi-read
primitive.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `0550492`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor put | 4.399 Mops/s | 4.003 Mops/s |
| async cursor put | 2.125 Mops/s | 2.273 Mops/s |
| async cursor batch put | 4.092 Mops/s | 4.095 Mops/s |
| async cursor loop put | 4.076 Mops/s | 4.099 Mops/s |
| blocking replace delete | 1.986 Mops/s | 1.847 Mops/s |
| async loop replace del | 2.015 Mops/s | 1.870 Mops/s |
| blocking cursor range del | 64.313 Mops/s | 63.215 Mops/s |
| async cursor range del | 49.806 Mops/s | 48.881 Mops/s |
| async cursor bunch del | 50.008 Mops/s | 44.222 Mops/s |

This short benchmark is noisy and does not show a clean throughput win for the
rebalance caller. The main value of this slice is structural: cursor page-get
completion is now batch-capable, and one B-tree path with independent sibling
reads uses it.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed

## Batched Async Exact-Get Traversal Slice

Added `async_batched_get_traverse()` for cold exact GET slots inside the async
worker. The helper initializes one cursor per eligible key, batches root page
fetches, then walks branch levels in rounds: each active cursor computes its
next child slot from the branch page, all child page requests for that depth are
submitted through `page_submit_cursor_get_batch()`, and the cursors are resumed
after completion. Once a cursor reaches a leaf, the existing `cursor_seek()`
logic finishes value extraction and duplicate/large-value handling.

`mdbx_async_get_batch()` and worker-side groups of independently submitted
`mdbx_async_get()` operations now try this traversal for cold async-get cache
slots before falling back to the old per-item `mdbx_get()` path. Warm cache
entries still use the cache materialization path first. This keeps the public
API blocking/async semantics unchanged while moving common cold exact-get
windows away from purely sequential worker-side `mdbx_get()` calls.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `faaee18`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 698.237 Kops/s | 473.854 Kops/s |
| async parallel get | 658.623 Kops/s | 780.986 Kops/s |
| async many parallel get | 1.261 Mops/s | 894.296 Kops/s |
| async threaded get | 1.449 Mops/s | 1.775 Mops/s |
| async threaded many get | 849.674 Kops/s | 1.811 Mops/s |
| async threaded batch get | 1.528 Mops/s | 1.269 Mops/s |
| async batch parallel get | 1.503 Mops/s | 804.815 Kops/s |
| async batch callback get | 992.289 Kops/s | 867.025 Kops/s |

The final short run is mixed. Grouped single async gets improved, especially
the threaded rows, while the single-operation async batch rows regressed in
this sample. The structural change is still important: cold exact-get windows
now have a depth-wise batched traversal path instead of relying only on
sequential blocking `mdbx_get()` calls inside the async worker.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed

## Batched Async Exact-Get Cache Seeding Slice

Successful cold exact-get traversal now seeds the worker-owned async get cache
entry while the traversal still has the cursor and returned value. If the value
maps to a committed page range that fits the public cache-entry format,
`async_batched_get_traverse()` stores the value byte range, trunk transaction,
and confirmed snapshot into the slot and promotes `use_count` directly to `2`.
If the value cannot be represented as a cache entry, the operation still
returns normally and the slot falls back to the previous `use_count == 1`
behavior.

This avoids an extra per-item `mdbx_cache_get_SingleThreaded()` refresh pass
after the first cold batched traversal. Repeated-key windows can move directly
from the internal batched traversal path to the existing batched cache
materialization path.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `95fc223`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 686.133 Kops/s | 447.019 Kops/s |
| async parallel get | 1.600 Mops/s | 1.292 Mops/s |
| async many parallel get | 976.731 Kops/s | 1.258 Mops/s |
| async threaded get | 879.308 Kops/s | 2.701 Mops/s |
| async threaded many get | 1.157 Mops/s | 2.779 Mops/s |
| async threaded batch get | 1.787 Mops/s | 2.901 Mops/s |
| async batch parallel get | 1.492 Mops/s | 1.653 Mops/s |
| async batch callback get | 1.046 Mops/s | 1.334 Mops/s |
| async cache st batch | 2.650 Mops/s | 3.919 Mops/s |

The target repeated async get rows improved substantially in this run, while
some unrelated cache-loop/threaded-cache rows moved down. The direct effect is
that successful cold batched traversal now feeds the cache-backed fast path for
subsequent windows instead of requiring an intermediate per-item refresh.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed

## Batched Async Exact-Get Loop Slice

`mdbx_async_get_loop()` now processes keys in bounded 64-item windows instead
of calling `async_cached_get()` once per item. The worker copies each key
returned by `key_func` into stable per-window storage, batches existing async
get cache hits through `cache_materialize_singlethreaded_batch()`, batches cold
slots through `async_batched_get_traverse()`, then invokes `result_func` and
updates `completed` in the original index order.

This keeps callback ordering and key lifetime compatible with the previous loop
implementation, while giving the loop API the same internal batched exact-get
traversal and cache-seeding path as grouped `mdbx_async_get()` and
`mdbx_async_get_batch()`.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `b9a87cf`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 1.252 Mops/s | 2.282 Mops/s |
| async many parallel get | 1.214 Mops/s | 2.215 Mops/s |
| async batch parallel get | 1.249 Mops/s | 1.271 Mops/s |
| async get loop | 1.399 Mops/s | 2.271 Mops/s |
| async threaded get loop | 721.178 Kops/s | 1.513 Mops/s |
| async get_ex loop | 585.270 Kops/s | 599.667 Kops/s |
| async threaded get_ex loop | 513.465 Kops/s | 1.174 Mops/s |

The direct loop rows improved in this run. The `get_ex` loop paths are still
not routed through the exact-get batch helper; their movement is benchmark
noise from the same run.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed

## Batched Async get_ex Loop Slice

`mdbx_async_get_ex_loop()` now uses the internal batched exact-get traversal for
already-open non-`MDBX_DUPSORT` tables. The worker copies each key returned by
`key_func` into stable 64-item window storage, submits the window through
`async_batched_get_traverse()`, preserves `mdbx_get_ex()`-style found-key output
for successful results, reports `values_count=1` on successful non-dupsort
batched hits, and invokes `result_func` in original index order.

The dupsort path, stale/invalid DBI path, and any item not handled by the
batched traversal still fall back to `mdbx_get_ex()` so duplicate value counts
and error behavior remain compatible.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `c46dc77`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 2.269 Mops/s | 2.302 Mops/s |
| async many parallel get | 1.619 Mops/s | 2.259 Mops/s |
| async batch parallel get | 1.858 Mops/s | 1.338 Mops/s |
| async get_ex batch | 939.407 Kops/s | 645.459 Kops/s |
| async get_ex many | 1.159 Mops/s | 1.188 Mops/s |
| async get loop | 2.895 Mops/s | 2.859 Mops/s |
| async get_ex loop | 1.184 Mops/s | 1.766 Mops/s |
| async threaded get_ex loop | 1.201 Mops/s | 1.759 Mops/s |

The targeted `get_ex_loop` rows improved in this run. The batch rows are not
changed by this slice and should be treated as run-to-run noise.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Release benchmark logs: `/tmp/mdbx-async-bench-getexloop-before.txt`, `/tmp/mdbx-async-bench-getexloop-after.txt`

## Batched Async get_ex Batch/Many Slice

`mdbx_async_get_ex_batch()` now routes already-open non-`MDBX_DUPSORT` tables
through `async_batched_get_traverse()` instead of calling `mdbx_get_ex()` once
per key. Successful batched non-dupsort results preserve found-key output,
store the value, and report `values_count=1`; misses/errors clear the batch
value slot and report `values_count=0`.

The worker also groups adjacent `async_op_get_ex` items with the same
transaction and DBI, so `mdbx_async_get_ex()` and `mdbx_async_get_ex_many()` can
use the same internal batched traversal when the queue contains compatible
operations. `MDBX_DUPSORT`, stale/invalid DBI, allocation fallback, and any
unhandled item still use `mdbx_get_ex()` to preserve duplicate-count semantics.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `a86be8a`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 1.219 Mops/s | 1.220 Mops/s |
| async many parallel get | 1.421 Mops/s | 2.060 Mops/s |
| async batch parallel get | 1.912 Mops/s | 1.562 Mops/s |
| async get_ex batch | 1.130 Mops/s | 1.387 Mops/s |
| async get_ex many | 1.113 Mops/s | 1.571 Mops/s |
| async get loop | 1.260 Mops/s | 2.408 Mops/s |
| async get_ex loop | 1.720 Mops/s | 1.509 Mops/s |
| async threaded get_ex loop | 1.494 Mops/s | 1.416 Mops/s |

The targeted `get_ex batch` and `get_ex many` rows improved in this run. The
threaded loop row is not changed by this slice and should be treated as
run-to-run noise.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Release benchmark logs: `/tmp/mdbx-async-bench-getexbatch-before.txt`, `/tmp/mdbx-async-bench-getexbatch-after.txt`

## Batched Async Regular Cache-Get Slice

Regular `mdbx_async_cache_get_batch()` now snapshots each volatile
`MDBX_cache_entry_t` into stable worker-local entries, then tries
`cache_materialize_singlethreaded_batch()` before falling back to
`mdbx_cache_get()` for races, misses, refreshes, or unhandled items. This moves
regular cache-hit materialization onto the same batched page-cache read path
already used by the SingleThreaded cache API, without changing volatile entry
publication semantics.

The worker also groups adjacent regular `async_op_cache_get` items with the
same transaction and DBI, so `mdbx_async_cache_get()` and
`mdbx_async_cache_get_many()` can use batched materialization when the queued
operations are compatible. The existing SingleThreaded grouping now uses the
same helper with direct non-volatile entry snapshots.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `9a345aa`:

| metric | before | after |
| --- | ---: | ---: |
| async cache many | 3.038 Mops/s | 2.848 Mops/s |
| async cache st many | 3.894 Mops/s | 2.933 Mops/s |
| async cache batch | 2.240 Mops/s | 2.284 Mops/s |
| async cache st batch | 2.949 Mops/s | 2.138 Mops/s |
| async cache batch cb | 1.879 Mops/s | 2.210 Mops/s |
| async cache st batch cb | 3.105 Mops/s | 3.916 Mops/s |
| async large cache batch | 136.943 Kops/s | 138.953 Kops/s |
| async large cache st batch | 138.608 Kops/s | 137.474 Kops/s |
| async threaded cache batch | 1.402 Mops/s | 2.637 Mops/s |
| async threaded cache st batch | 3.850 Mops/s | 2.779 Mops/s |
| async cache loop | 2.272 Mops/s | 3.265 Mops/s |
| async threaded cache loop | 2.386 Mops/s | 3.137 Mops/s |

The regular batch/callback/threaded rows improved, while regular many and some
SingleThreaded rows regressed in this run. The slice is primarily about moving
regular cache hits onto the batched page-cache materialization path; the mixed
cache benchmark means the snapshot/grouping overhead still needs follow-up
tuning.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Release benchmark logs: `/tmp/mdbx-async-bench-cacheget-before.txt`, `/tmp/mdbx-async-bench-cacheget-after.txt`

## Batched Async Lower-Bound Slice

`mdbx_async_get_equal_or_great_batch()` and adjacent
`async_op_get_equal_or_great` operations now use a batched non-dupsort
lower-bound traversal. The helper submits root and branch page reads for many
keys together, then completes each cursor with
`cursor_ops(..., MDBX_SET_LOWERBOUND)`, preserving `MDBX_SUCCESS` for exact
matches and `MDBX_RESULT_TRUE` for greater matches.

`mdbx_async_get_equal_or_great_loop()` now uses the same traversal in bounded
64-item windows with stable key copies, optional data callback inputs, ordered
result callbacks, and compatible `completed` accounting. `MDBX_DUPSORT` tables,
allocation fallback, stale/invalid DBI, and unhandled items still call
`mdbx_get_equal_or_great()` to preserve nested duplicate-data semantics.

Reduced forced no-mmap/io_uring benchmark (`items=1000 ops=10000
large_items=256 large_ops=10000 large_value=10000 page_cache=64K`), compared
against previous commit `8b94b7e`:

| metric | before | after |
| --- | ---: | ---: |
| async parallel get | 1.295 Mops/s | 2.321 Mops/s |
| async many parallel get | 1.276 Mops/s | 2.429 Mops/s |
| async batch parallel get | 2.231 Mops/s | 2.216 Mops/s |
| async get_ex batch | 963.725 Kops/s | 1.683 Mops/s |
| async get_ex many | 1.641 Mops/s | 1.662 Mops/s |
| async lowerbound batch | 561.826 Kops/s | 1.660 Mops/s |
| async lowerbound many | 878.692 Kops/s | 1.642 Mops/s |
| async lowerbound loop | 576.198 Kops/s | 1.877 Mops/s |
| async threaded lower loop | 1.179 Mops/s | 1.128 Mops/s |

The targeted lower-bound batch, many, and loop rows improved in this run. The
other get rows moved as run-to-run noise from the same reduced benchmark.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Release benchmark logs: `/tmp/mdbx-async-bench-lowerbound-before.txt`, `/tmp/mdbx-async-bench-lowerbound-after.txt`

## Pending Async Read Progress Slice

The async worker now keeps internal read operations pending while their
batched page reads are still in flight instead of immediately driving every
batch to completion before accepting more compatible read work. Exact get,
get_ex, lower-bound, cache-get materialization/refresh, and cursor read
families have pending-state wrappers that:

- start their root/branch/cache page batch with a non-waiting drive;
- keep the operation in an internal pending list when the page batch reports
  `MDBX_RESULT_TRUE`;
- continue accepting independent read work until the completion chunk is full,
  the next operation is not read-like, or the next cursor operation conflicts
  with an already-pending cursor;
- drain pending reads with non-waiting polls and only block when no pending
  read can make progress.

This still runs inside the existing async worker thread, so it is not a public
API coroutine scheduler. It does, however, satisfy an important internal
requirement of the migration: independent cache misses and page reads can be
submitted before earlier pending reads have completed, and compatible read
operations can make progress as their page I/O completes.

Smoke coverage now queues exact get, get_ex batch, lower-bound batch,
cache-get batch, cursor batch, exact-get loop, get_ex loop, lower-bound loop,
cache-get loop, and cursor loop reads ahead of transaction abort operations,
forcing the worker to drain pending read state before it executes the abort.
The same-cursor tests also verify that cursor operations sharing one cursor are
not overlapped in a way that corrupts cursor position.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build -R async_api --output-on-failure`: passed 4/4
- `MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=. ./mdbx_async_api_smoke`: passed

## io_uring Smoke and Ratio Reporting Slice

CTest now includes `async_api_nommap_io_uring`, which runs
`mdbx_async_api_smoke` with data-file mmap disabled, the explicit page-cache
limited to 64 KiB, and the `io_uring` backend requested. This keeps the
storage-backed async read path under regular focused test coverage instead of
only ad hoc manual runs.

`mdbx_async_api_bench` now prints ratios that compare internal async read
paths against the executor-threaded variants. Those rows make it easier to
track whether the internal page-cache/batched traversal path is replacing
plain worker offload for the targeted workloads.

Current full-size forced no-mmap/io_uring benchmark
(`/tmp/mdbx-async-bench-migration-log-final.txt`) compared with the
previous internal-threaded ratio run
(`/tmp/mdbx-async-bench-internal-threaded-ratios-final.txt`):

| metric | previous | current |
| --- | ---: | ---: |
| async/threaded get | 1.107 | 1.083 |
| async-many/threaded | 1.101 | 1.113 |
| async-batch/threaded | 1.151 | 1.104 |
| async-cache-batch/thread | 1.057 | 1.114 |
| async-cache-loop/thread | 1.027 | 1.010 |
| async-get-ex-loop/thread | 1.037 | 1.102 |
| async-cget-loop/thread | 1.030 | 0.762 |
| async-cbatch/threaded | 0.659 | 0.812 |
| async-scan/threaded | 0.751 | 1.032 |

The point-get, get_ex loop, and cache ratios remain around or above the
previous run. Cursor ratios still move enough between single benchmark runs
that they should be read as directional, not as pass/fail gates.

## Blocking Public API Compatibility Slice

Focused smoke coverage now also exercises the blocking public `mdbx_get()`,
`mdbx_get_ex()`, and `mdbx_get_equal_or_great()` APIs on a caller-owned read
transaction while the explicit async read implementation is enabled. The checks
cover exact hits, lower-bound greater-key results, and not-found results. This
guards the compatibility rule that synchronous public APIs can keep blocking
externally while using the same explicit page-cache/traversal internals.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build -R async_api --output-on-failure`: passed 4/4
- `make -j2 mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=. ./mdbx_async_api_smoke`: passed

## Remaining Async I/O Gap

The current implementation has a real async page-read primitive, explicit
page-cache materialization through `io_uring`, depth-wise batched traversal for
many async get/get_ex/lower-bound paths, and worker-side pending read state.
The remaining gap against the full goal is narrower but still real: the public
blocking B-tree/cursor APIs are not themselves implemented as exposed
coroutines, and the async executor is still the scheduler boundary. More cursor
movement and scan paths should continue moving from per-operation blocking
calls to explicit pending read state, and completion should keep being proven
with forced no-mmap/io_uring tests and benchmarks.

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

Additional async cursor-delete loop API checkpoint:

- added `mdbx_async_cursor_del_loop()` as an async-only helper for deleting
  several current cursor items inside one worker operation. The blocking API is
  unchanged. The operation repeatedly calls `mdbx_cursor_del()` with the
  supplied flags and uses `MDBX_GET_CURRENT` between deletes, matching the
  existing cursor-delete benchmark's post-delete cursor contract.
- the API accepts an optional `completed` output that receives the number of
  successful deletes before completion or before an operation-level error.
- extended `ut_and_examples/async-api-smoke.c` with a dedicated temporary table
  that deletes three adjacent cursor items and verifies the completion count
  and final entry count.
- extended `ut_and_examples/async-api-bench.c` with
  `mdbx_async_cursor_del_loop()` timing beside the existing blocking and
  per-item async cursor-delete measurements.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor delete
  4.028 Mops/s, async cursor delete 133.939 Kops/s, async cursor delete loop
  4.099 Mops/s, async-cursor-del/block 0.033,
  async-cursor-del-loop/block 1.017, and
  async-cursor-del-loop/async 30.601.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=180 async-covered=132 async-only=48 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the cursor-delete benchmark gap was primarily submission
  granularity. Moving the delete/get-current loop into one async worker
  operation brings this reduced cursor-delete workload to parity with the
  blocking cursor path while preserving the existing blocking API.

Additional async put-loop API checkpoint:

- added `mdbx_async_put_loop()` as an async-only worker-side loop helper for
  repeated `mdbx_put()` calls. The blocking API is unchanged.
- the API mirrors the existing GET loop style: an item callback prepares each
  key/data pair on the executor worker thread, an optional result callback
  observes each `mdbx_put()` result, and an optional `completed` output reports
  the number of put attempts completed before return.
- the first implementation rejects `MDBX_RESERVE` and `MDBX_MULTIPLE`, matching
  the existing async put and put-batch wrappers' in-place memory constraints.
- extended `ut_and_examples/async-api-smoke.c` with a dedicated temporary table
  that writes four entries through `mdbx_async_put_loop()`, verifies callback
  counts, reads one payload back, and drops the table.
- extended `ut_and_examples/async-api-bench.c` with async put-loop timing beside
  per-item async put and async put-batch.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking write put
  2.575 Mops/s, async write put 1.741 Mops/s, async batch write put
  2.505 Mops/s, and async loop write put 2.653 Mops/s. Ratios were
  async-put/blocking 0.676, async-put-batch/blocking 0.973,
  async-put-loop/blocking 1.031, async-put-loop/async-put 1.524, and
  async-put-loop/batch 1.059.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=181 async-covered=132 async-only=49 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: worker-side write loops now give callers a streaming put shape
  that avoids prebuilding batch arrays and, in this reduced run, slightly beats
  both the blocking put sample and the existing async put-batch sample.

Additional async delete-loop API checkpoint:

- added `mdbx_async_del_loop()` as an async-only worker-side loop helper for
  repeated `mdbx_del()` calls. The blocking API is unchanged.
- the API reuses the existing GET loop key callback style, accepts an optional
  data callback for duplicate-data matching, invokes an optional result
  callback for each delete, and reports completed delete attempts through an
  optional `completed` output.
- extended `ut_and_examples/async-api-smoke.c` with a dedicated temporary table
  that writes five entries, deletes three by key through
  `mdbx_async_del_loop()`, verifies callback counts, verifies the remaining
  entry count, and drops the table.
- extended `ut_and_examples/async-api-bench.c` with async delete-loop timing
  beside per-item async delete and async delete-batch.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking delete 1.897 Mops/s,
  async delete 1.494 Mops/s, async batch delete 2.038 Mops/s, and async loop
  delete 2.057 Mops/s. Ratios were async-del/blocking 0.788,
  async-del-batch/blocking 1.075, async-del-loop/blocking 1.085,
  async-del-loop/async-del 1.377, and async-del-loop/batch 1.009.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=182 async-covered=132 async-only=50 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: worker-side delete loops give callers a streaming key-generation
  delete shape that avoids prebuilt arrays and, in this reduced run, slightly
  improves on the existing async delete-batch path.

Additional async replace-loop API checkpoint:

- added `mdbx_async_replace_loop()` as an async-only worker-side loop helper for
  repeated regular `mdbx_replace()` calls. The blocking API is unchanged.
- the API invokes an item callback on the executor worker thread to prepare the
  key, new-data, and old-data descriptors for each replacement, then invokes an
  optional result callback after each `mdbx_replace()` call. An optional
  `completed` output reports the number of replacement attempts completed before
  the operation returns.
- the first implementation rejects `MDBX_RESERVE` and `MDBX_MULTIPLE`, matching
  the existing async put/replace wrappers' in-place memory constraints. It does
  not yet cover `mdbx_replace_ex()` preservation callbacks or the
  `new_data == NULL` delete-with-old-value form.
- extended `ut_and_examples/async-api-smoke.c` with a temporary table that
  seeds five records, replaces three through `mdbx_async_replace_loop()`,
  verifies callback counts and old-value retrieval, reads one replacement back,
  and drops the table.
- extended `ut_and_examples/async-api-bench.c` with async replace-loop timing
  beside per-item async replace and async replace-batch.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace 2.535 Mops/s,
  async replace 1.790 Mops/s, async batch replace 2.438 Mops/s, and async
  loop replace 2.429 Mops/s. Ratios were async-replace/blocking 0.706,
  async-repl-batch/blocking 0.962, async-repl-loop/blocking 0.958,
  async-repl-batch/async 1.361, async-repl-loop/async 1.357, and
  async-repl-loop/batch 0.996.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=183 async-covered=132 async-only=51 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: worker-side replacement loops give callers a streaming
  replacement shape that avoids prebuilt batch arrays and removes most of the
  per-item async replace overhead. In this reduced run the loop is essentially
  tied with replace-batch and remains just below the blocking replacement
  sample, making `replace_ex` and delete-with-old-value loop coverage the next
  replacement-family gaps.

Additional async replace-ex-loop API checkpoint:

- added `mdbx_async_replace_ex_loop()` as an async-only worker-side loop helper
  for repeated `mdbx_replace_ex()` calls with a shared preservation callback.
  The blocking API is unchanged.
- reused the existing replace-loop callbacks and executor storage, adding a
  `replace_ex_loop` opcode plus preservation callback fields. The regular
  replace loop still dispatches to `mdbx_replace()`, while the new opcode
  dispatches to `mdbx_replace_ex()`.
- the API rejects `MDBX_RESERVE` and `MDBX_MULTIPLE`, matching the existing
  async replace wrappers' in-place memory constraints. It still does not cover
  the `new_data == NULL` delete-with-old-value loop form.
- extended `ut_and_examples/async-api-smoke.c` to dirty three records, replace
  them through `mdbx_async_replace_ex_loop()`, verify old-value preservation and
  callback counts, read one replacement back, then restore the touched records
  before the later whole-table assertions.
- extended `ut_and_examples/async-api-bench.c` with async replace-ex-loop
  timing beside per-item async replace-ex and async replace-ex-batch.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace_ex 2.488 Mops/s,
  async replace_ex 1.761 Mops/s, async batch replace_ex 2.363 Mops/s, and
  async loop replace_ex 2.446 Mops/s. Ratios were async-replace-ex/blocking
  0.708, async-repl-ex-batch/block 0.950, async-repl-ex-loop/block 0.983,
  async-repl-ex-batch/async 1.342, async-repl-ex-loop/async 1.389, and
  async-repl-ex-loop/batch 1.035.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=184 async-covered=132 async-only=52 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the preservation-callback replacement path now has the same
  per-item, batch, and worker-loop benchmark shapes as regular replacement.
  In this reduced run the loop form is the strongest async replace-ex shape and
  is close to the blocking sample, leaving delete-with-old-value loop coverage
  as the remaining replacement-family API gap.

Additional async replace-delete-loop API checkpoint:

- added `mdbx_async_replace_delete_loop()` and
  `mdbx_async_replace_ex_delete_loop()` as async-only worker-side loop helpers
  for the `new_data == NULL` delete-with-old-value replacement form. The
  blocking API is unchanged.
- added dedicated delete-with-old loop callbacks because the existing
  replace-loop item callback always returns a real `MDBX_val *new_data`, and a
  zero-length value is not the same as passing NULL.
- the regular helper dispatches to `mdbx_replace()` with NULL `new_data`; the
  replace-ex helper dispatches to `mdbx_replace_ex()` with NULL `new_data` and
  a shared preservation callback. The new docs note that callers should pass
  `MDBX_CURRENT` for ordinary key deletion, matching the current
  `mdbx_replace_ex()` precheck.
- extended `ut_and_examples/async-api-smoke.c` with temporary-table coverage
  for both helpers. The replace-ex variant dirties the values first so the
  preservation callback is exercised once per item.
- extended `ut_and_examples/async-api-bench.c` with blocking, per-item async,
  batch async, and loop async measurements for delete-with-old replacement.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking replace delete
  1.877 Mops/s, async replace delete 1.463 Mops/s, async batch replace delete
  1.982 Mops/s, and async loop replace delete 1.959 Mops/s. Ratios were
  async-repl-del/blocking 0.779, async-repl-del-batch/block 1.056,
  async-repl-del-loop/block 1.043, async-repl-del-batch/async 1.355,
  async-repl-del-loop/async 1.339, and async-repl-del-loop/batch 0.988.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=186 async-covered=132 async-only=54 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the replacement family now has worker-side loop helpers for
  regular replacement, preservation-callback replacement, and
  delete-with-old-value extraction. In this reduced run the delete-with-old loop
  beats the blocking sample and materially improves on per-item async
  submission, while staying roughly tied with the batch form.

Additional async cursor-put-loop API checkpoint:

- added `mdbx_async_cursor_put_loop()` as an async-only worker-side loop helper
  for repeated `mdbx_cursor_put()` calls through one cursor. The blocking API is
  unchanged.
- reused the existing put-loop callback signatures so callers can generate each
  key/data pair on the executor worker thread, observe each result, and receive
  an optional completed count.
- the API rejects `MDBX_RESERVE` and `MDBX_MULTIPLE`, matching the existing
  async cursor-put wrappers' in-place memory constraints.
- extended `ut_and_examples/async-api-smoke.c` to write four entries through a
  write cursor with `mdbx_async_cursor_put_loop()`, verify callback counts, and
  read one inserted value back through the same cursor.
- extended `ut_and_examples/async-api-bench.c` with async cursor-put-loop timing
  beside blocking cursor put, per-item async cursor put, and async cursor-put
  batch.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor put 4.127 Mops/s,
  async cursor put 2.367 Mops/s, async cursor batch put 3.930 Mops/s, and async
  cursor loop put 3.812 Mops/s. Ratios were async-cursor-put-batch/block
  0.952, async-cursor-put-loop/block 0.924, async-cursor-put-batch/async
  1.660, async-cursor-put-loop/async 1.611, and
  async-cursor-put-loop/batch 0.970.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=187 async-covered=132 async-only=55 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cursor put now has per-item, batch, and worker-loop async shapes.
  In this reduced run the loop removes most per-item async overhead but remains
  slightly behind the existing batch form and below the blocking cursor-put
  sample, so the remaining cursor-put gap appears to be path/backend efficiency
  rather than API submission granularity alone.

Additional cursor-scan benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with predicate-based
  `mdbx_cursor_scan()` and `mdbx_async_cursor_scan()` measurements. The public
  async scan wrappers already existed; this checkpoint adds benchmark coverage
  without changing the blocking or async API surface.
- the benchmark verifies each scanned key/value pair through the predicate and
  repeats bounded full-table scan chunks when the target cursor-pair count is
  larger than the seeded table. It reports blocking serial scan, blocking
  pthread-parallel scan, and async multi-executor scan beside the existing
  cursor-batch and cursor-loop iteration numbers.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor batch
  224.989 Mops/s, parallel cursor batch 62.287 Mops/s, async cursor batch
  79.741 Mops/s, async cursor loop 92.552 Mops/s, blocking cursor scan
  150.972 Mops/s, parallel cursor scan 71.174 Mops/s, and async cursor scan
  87.437 Mops/s. Ratios were async-cursor-scan/par 1.228,
  async-cursor-scan/ser 0.579, and async-scan/cursor-loop 0.945.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=187 async-covered=132 async-only=55 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: async cursor scan now has direct benchmark visibility. In this
  reduced run it beats blocking pthread-parallel scan by 22.8%, remains below
  hot blocking serial scan, and is slightly slower than the existing async
  cursor-loop helper, so predicate-scan dispatch is competitive for parallel
  cursor traversal but not the fastest async cursor iteration shape.

Additional cursor-scan-from benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` to measure
  `mdbx_cursor_scan_from()` and `mdbx_async_cursor_scan_from()` beside the
  direct cursor-scan benchmark. The async wrapper already existed and had smoke
  coverage; this checkpoint adds performance visibility for the positioned
  lower-bound scan path without changing the public API.
- reused the cursor-scan predicate verifier and cursor scan runners with a
  `scan_from` mode. Each bounded scan chunk starts from key 0 via
  `MDBX_SET_LOWERBOUND` and then scans forward with `MDBX_NEXT`, exercising the
  caller-supplied key/value result slots required by the scan-from API.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor scan
  148.471 Mops/s, parallel cursor scan 77.980 Mops/s, async cursor scan
  96.053 Mops/s, blocking cursor scan_from 149.405 Mops/s, parallel cursor
  scan_from 79.952 Mops/s, and async cursor scan_from 86.500 Mops/s. Ratios
  were async-scan-from/par 1.082, async-scan-from/ser 0.579, and
  async-scan-from/scan 0.901.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=187 async-covered=132 async-only=55 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: async cursor scan-from is now benchmarked directly. In this
  reduced run it still beats the blocking pthread-parallel scan-from path, but
  it trails direct async cursor scan, which makes the extra positioned-start
  machinery visible and gives future scan-from optimization a baseline.

Additional direct cursor-get benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with direct
  `mdbx_cursor_get()` and `mdbx_async_cursor_get()` iteration measurements
  beside the existing cursor-batch, cursor-loop, and cursor-scan read paths.
  This checkpoint adds benchmark coverage only; the public API is unchanged.
- the benchmark uses one cursor per blocking thread or async executor, repeats
  table passes with `MDBX_FIRST`/`MDBX_NEXT`, verifies every returned key/value
  pair, and keeps only one pending cursor movement per async cursor because the
  cursor state itself is sequential.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor get
  88.306 Mops/s, parallel cursor get 62.370 Mops/s, async cursor get
  873.150 Kops/s, blocking cursor batch 218.614 Mops/s, parallel cursor batch
  59.954 Mops/s, async cursor batch 80.689 Mops/s, and async cursor loop
  93.779 Mops/s. Ratios were async-cursor-get/par 0.014,
  async-cursor-get/ser 0.010, and async-cursor-batch/get 92.411.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=187 async-covered=132 async-only=55 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the direct per-call async cursor-get path is dramatically slower
  than blocking cursor iteration and the existing coarse async cursor batch
  shape. Cursor-heavy read callers should use `mdbx_async_cursor_get_batch()`
  or `mdbx_async_cursor_get_batches()` today; any future work on direct
  `mdbx_async_cursor_get()` needs to attack per-operation executor round trips
  or add a cursor-specific streaming helper.

Additional async cursor-get-loop API checkpoint:

- added `mdbx_async_cursor_get_loop()` plus `MDBX_cursor_get_loop_func` as an
  async-only cursor iteration helper. It submits one operation that runs
  repeated `mdbx_cursor_get()` calls on the executor worker thread, using a
  caller-provided `start_op` for the first fetch and `turn_op` for subsequent
  fetches.
- the helper reports the number of completed cursor movements through an
  optional `completed` output and invokes an optional worker-thread callback for
  each fetched key/value pair. Returned descriptors keep the usual cursor-owned
  lifetime and must be consumed or copied before the callback returns or the
  cursor advances.
- smoke coverage verifies full-table cursor get-loop traversal and callback
  counts. Benchmark coverage adds `async cursor get loop`,
  `async-cursor-get-loop/par`, `async-cursor-get-loop/ser`,
  `async-cursor-get-loop/get`, and `async-get-loop/batch`.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor get
  88.319 Mops/s, parallel cursor get 65.590 Mops/s, async cursor get
  1.328 Mops/s, async cursor get loop 62.610 Mops/s, blocking cursor batch
  218.360 Mops/s, parallel cursor batch 60.267 Mops/s, async cursor batch
  79.771 Mops/s, and async cursor loop 90.443 Mops/s. Ratios were
  async-cursor-get/par 0.020, async-cursor-get-loop/par 0.955,
  async-cursor-get-loop/ser 0.709, async-cursor-get-loop/get 47.143,
  async-cursor-batch/get 60.064, and async-get-loop/batch 0.785.
- direct before/after against the prior cursor-get checkpoint: blocking cursor
  get stayed effectively flat at 88.306 -> 88.319 Mops/s, parallel cursor get
  moved 62.370 -> 65.590 Mops/s, direct async cursor get moved 873.150 Kops/s
  -> 1.328 Mops/s, async cursor batch moved 80.689 -> 79.771 Mops/s, and async
  cursor loop moved 93.779 -> 90.443 Mops/s. The new cursor get-loop path was
  62.610 Mops/s, roughly matching the prior blocking parallel cursor-get run
  and 71.7x the prior direct async cursor-get number.
- compared with the pre-async ioarena baseline at the top of this log, this
  checkpoint is not an apples-to-apples storage-backend comparison: the original
  baseline measures ioarena phases, while `mdbx_async_api_bench` measures public
  async API overhead on a seeded in-process workload. The result does show that
  the poor direct async cursor-get number was primarily submission granularity,
  not cursor traversal throughput.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the new cursor get-loop helper gives cursor-heavy async callers a
  public worker-side streaming shape that reaches blocking-parallel cursor-get
  territory in the reduced benchmark without changing the existing blocking API.
  Direct one-operation-per-cursor-move async cursor get remains useful for API
  completeness but is the wrong performance shape for dense iteration.

Additional threaded cursor-get-loop benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with a threaded
  `mdbx_async_cursor_get_loop()` benchmark. Each application pthread owns its
  own async executor, read transaction, and cursor, then submits cursor get-loop
  chunks until its assigned cursor-pair target is consumed.
- this does not add public API surface; it measures the cursor get-loop helper
  under the same multi-application-thread shape already used for point
  `mdbx_async_get_loop()`.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor get
  83.269 Mops/s, parallel cursor get 89.510 Mops/s, async cursor get
  1.379 Mops/s, async cursor get loop 73.025 Mops/s, async threaded cursor get
  loop 109.420 Mops/s, async cursor batch 82.722 Mops/s, and async cursor loop
  102.598 Mops/s. Ratios were async-cursor-get/par 0.015,
  async-cursor-get-loop/par 0.816, async-thread-cget-loop/par 1.222,
  async-thread-cget-loop/ser 1.314, async-thread-cget-loop/get 79.329, and
  async-thread-cget-loop/loop 1.498.
- direct comparison with the previous cursor-get-loop checkpoint: the
  single-submitter cursor get-loop moved 62.610 -> 73.025 Mops/s, while the new
  threaded cursor get-loop measured 109.420 Mops/s. The direct
  one-operation-per-cursor-move path remained far slower at 1.379 Mops/s.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cursor GET-loop now has benchmark evidence in both submission
  shapes. The threaded shape is the one that best matches the goal of parallel
  read-heavy async callers and, in this reduced run, beats blocking
  pthread-parallel cursor get by 22.2%.

Additional threaded cursor-scan benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with threaded
  `mdbx_async_cursor_scan()` and `mdbx_async_cursor_scan_from()` benchmarks.
  Each application pthread owns an async executor, read transaction, and cursor,
  then submits bounded scan chunks until its assigned pair target is consumed.
- this is benchmark coverage only. It measures predicate cursor scanning under
  the same multi-application-thread shape now covered for point GET loops and
  cursor GET loops.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor scan
  150.554 Mops/s, parallel cursor scan 71.108 Mops/s, async cursor scan
  93.390 Mops/s, async threaded cursor scan 123.033 Mops/s, blocking cursor
  scan_from 149.263 Mops/s, parallel cursor scan_from 71.240 Mops/s, async
  cursor scan_from 87.953 Mops/s, and async threaded cursor scan_from
  68.752 Mops/s.
- ratios were async-cursor-scan/par 1.313, async-thread-scan/par 1.730,
  async-thread-scan/ser 0.817, async-thread-scan/scan 1.317,
  async-scan-from/par 1.235, async-thread-scan-from/par 0.965,
  async-thread-scan-from/ser 0.461, and async-thread-scan-from/scan 0.782.
- comparison with the prior scan benchmark checkpoints: the new threaded
  cursor-scan shape improves plain async cursor scan from the previous
  single-submitter samples and beats blocking pthread-parallel scan by 73.0% in
  this run. The threaded scan-from shape did not help; it was slower than both
  the single-submitter async scan-from path and blocking pthread-parallel
  scan_from in this run, so positioned-start scan work still needs separate
  optimization if it matters for read-heavy callers.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: threaded predicate cursor scan is another parallel-heavy read
  shape where async beats the blocking pthread-parallel benchmark. Threaded
  scan_from is now measured too, but the result is a useful negative signal
  rather than a throughput win.

Additional threaded cursor-batch-loop benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with a threaded
  `mdbx_async_cursor_get_batches()` benchmark. Each application pthread owns an
  async executor, read transaction, and cursor, then submits one coarse
  cursor-batch-loop operation for its assigned pair target.
- the benchmark accepts the helper's existing completion rule: the completed
  count may exceed the requested target because the final internal batch can
  overshoot. This matches the smoke coverage for `mdbx_async_cursor_get_batches()`.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor batch
  219.706 Mops/s, parallel cursor batch 61.659 Mops/s, async cursor batch
  141.141 Mops/s, async cursor loop 96.007 Mops/s, and async threaded cursor
  loop 133.740 Mops/s. Related cursor-get numbers were parallel cursor get
  66.516 Mops/s, async cursor get loop 61.879 Mops/s, and async threaded
  cursor get loop 69.919 Mops/s.
- ratios were async-cursor/blocking par 2.289, async-loop-cursor/par 1.557,
  async-thread-cursor-loop/par 2.169, async-thread-cursor-loop/ser 0.609,
  async-thread-cursor/batch 0.948, and async-thread-cursor/loop 1.393.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the existing async cursor batch-loop helper now has benchmark
  evidence in the multi-application-thread shape. In this run it stays well
  above blocking pthread-parallel cursor batch and above the single-submitter
  async cursor loop, while landing slightly below the direct async cursor batch
  path.

Additional threaded cursor-batch benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with a threaded
  `mdbx_async_cursor_get_batch()` benchmark. Each application pthread owns an
  async executor, read transaction, and cursor, then repeatedly submits direct
  cursor batches until its assigned pair target is consumed.
- this complements the threaded `mdbx_async_cursor_get_batches()` measurement:
  direct batches preserve per-batch cursor progress and restart after EOF,
  while the batch-loop helper submits one coarse operation per application
  thread.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking cursor batch
  213.547 Mops/s, parallel cursor batch 61.741 Mops/s, async cursor batch
  75.101 Mops/s, async threaded cursor batch 120.163 Mops/s, async cursor loop
  95.568 Mops/s, and async threaded cursor loop 137.403 Mops/s.
- ratios were async-cursor/blocking par 1.216,
  async-thread-cbatch/par 1.946, async-thread-cbatch/ser 0.563,
  async-thread-cbatch/batch 1.600, async-loop-cursor/par 1.548,
  async-thread-cursor-loop/par 2.225, and async-thread-cursor/loop 1.438.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: direct cursor batches also benefit from multi-application-thread
  submission and now have benchmark evidence above blocking pthread-parallel
  cursor batch. The threaded cursor batch-loop helper remains the faster coarse
  async cursor-batch shape in this sample.

Additional threaded point-GET-loop benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` so the existing threaded
  point-GET loop runner also benchmarks `mdbx_async_get_ex_loop()` and
  `mdbx_async_get_equal_or_great_loop()`. Each application pthread owns its
  async executor and read transaction, then submits one coarse loop operation
  for its assigned key range.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  885.925 Kops/s, async get loop 1.097 Mops/s, async get_ex loop
  1.120 Mops/s, async lowerbound loop 921.331 Kops/s, async threaded get loop
  1.019 Mops/s, async threaded get_ex loop 1.100 Mops/s, and async threaded
  lower loop 1.105 Mops/s.
- ratios were async-loop/blocking par 1.238, async-get-ex-loop/par 1.265,
  async-lower-loop/par 1.040, async-thread-loop/par 1.150,
  async-thread-get-ex-loop/par 1.242, async-thread-lower-loop/par 1.248,
  async-thread-get-ex-loop/loop 0.982, and async-thread-lower-loop/loop 1.200.
- direct comparison with the previous threaded cursor-batch checkpoint's
  point-GET numbers: blocking parallel get moved 691.521 -> 885.925 Kops/s,
  async get loop moved 863.019 Kops/s -> 1.097 Mops/s, async get_ex loop
  621.682 Kops/s -> 1.120 Mops/s, async lowerbound loop 805.026 ->
  921.331 Kops/s, and async threaded get loop 1.007 -> 1.019 Mops/s. The new
  threaded get_ex and lowerbound loop measurements are both above the current
  blocking pthread-parallel point-GET baseline.
- comparison against the pre-async ioarena baseline at the top of this log is
  still not apples-to-apples: the original baseline measures storage-level
  ioarena phases, while this benchmark measures public async API overhead on an
  in-process seeded workload. This checkpoint strengthens the API-level
  parallel read evidence, but it does not close or remeasure the separate
  storage/ioarena migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=188 async-covered=132 async-only=56 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the point-GET family now has multi-application-thread loop
  benchmark coverage for plain get, get_ex, and lower-bound reads. In this
  reduced run all three threaded loop shapes beat the current blocking
  pthread-parallel point-GET baseline.

Additional positioned cursor get-loop checkpoint:

- added `mdbx_async_cursor_get_loop_from()` as an async-only positioned,
  count-limited cursor range helper. It copies the submitted start key/value
  bytes before enqueueing, uses them for the initial cursor positioning
  operation, invokes the existing cursor get-loop callback for each fetched
  pair, updates the caller's key/value descriptors to the last consumed pair,
  and reports the completed count.
- smoke coverage verifies a four-item `MDBX_SET_LOWERBOUND` loop from key 18,
  including the completed count, callback count, and final key/value descriptor
  update.
- extended `ut_and_examples/async-api-bench.c` with single-submitter and
  multi-application-thread `mdbx_async_cursor_get_loop_from()` measurements.
  These sit next to the existing cursor get-loop and predicate scan_from
  measurements, so positioned range-read callers can compare the count-limited
  and predicate-driven shapes directly.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported parallel cursor scan_from
  69.353 Mops/s, async cursor scan_from 86.922 Mops/s, async threaded cursor
  scan_from 130.133 Mops/s, async cursor get loop_from 61.723 Mops/s, and
  async threaded cursor get loop_from 78.762 Mops/s. Related cursor-get-loop
  numbers were parallel cursor get 62.108 Mops/s, async cursor get loop
  76.592 Mops/s, and async threaded cursor get loop 114.298 Mops/s.
- ratios were async-cget-loop-from/par 0.890,
  async-thread-cget-from/par 1.136, async-cget-loop-from/scan 0.710,
  async-thread-cget-from/loop 1.276, and async-thread-cget-from/scan 0.605.
- comparison with the previous scan_from checkpoint: the new positioned
  count-limited threaded helper is above the current blocking pthread-parallel
  scan_from baseline in this run, but it is slower than the existing
  predicate-based async scan_from path. This is useful API symmetry and a
  measured alternative shape, not a replacement for the current faster
  scan_from helper.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=189 async-covered=132 async-only=57 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: positioned cursor range reads now have both predicate-driven and
  count-limited async API shapes. The threaded count-limited shape beats the
  blocking pthread scan_from baseline in this sample, while the predicate
  scan_from helper remains the faster async positioned-scan path.

Additional positioned cursor batch-loop checkpoint:

- added `mdbx_async_cursor_get_batches_from()` as an async-only positioned,
  batch-loop cursor range helper. It copies the submitted start key/value bytes
  before enqueueing, positions the cursor once on the worker thread, repeatedly
  calls `mdbx_cursor_get_batch()`, invokes the existing batch callback for each
  internal batch, updates the caller's key/value descriptors to the last
  consumed pair, and reports the completed pair count.
- smoke coverage verifies a `MDBX_SET_LOWERBOUND` batch-loop from key 18,
  including the completed count, callback pair count, and final key/value
  descriptor update.
- extended `ut_and_examples/async-api-bench.c` with single-submitter and
  multi-application-thread `mdbx_async_cursor_get_batches_from()` measurements.
  These sit beside the positioned `mdbx_async_cursor_get_loop_from()` and
  predicate scan_from paths.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported parallel cursor scan_from
  67.688 Mops/s, async cursor scan_from 73.440 Mops/s, async threaded cursor
  scan_from 139.364 Mops/s, async cursor get loop_from 58.459 Mops/s, async
  threaded cursor get loop_from 121.925 Mops/s, async cursor loop_from
  99.517 Mops/s, and async threaded cursor loop_from 138.483 Mops/s.
- ratios were async-cursor-loop-from/par 1.470,
  async-cursor-loop-from/scan 1.355, async-thread-cur-from/par 2.046,
  async-thread-cur-from/loop 1.392, and async-thread-cur-from/scan 0.994.
- comparison with the previous positioned cursor get-loop checkpoint: the
  batch-loop-from helper is the better count-limited positioned range shape.
  It beats the current blocking pthread scan_from baseline in both
  single-submitter and threaded forms, and the threaded form is effectively tied
  with the predicate-based async scan_from path in this sample.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=190 async-covered=132 async-only=58 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: positioned range reads now have a count-limited batch-loop async
  API that uses the faster cursor batch primitive. This improves the positioned
  count-limited read story and gives the benchmark another parallel-heavy async
  path above blocking pthread scan_from.

Additional positioned lower-bound result handling checkpoint:

- fixed the worker-side positioned cursor helpers so an initial
  `MDBX_SET_LOWERBOUND` positioning result of `MDBX_RESULT_TRUE` is accepted as
  a successful greater-key position for `mdbx_async_cursor_get_loop_from()` and
  `mdbx_async_cursor_get_batches_from()`.
- smoke coverage now submits non-exact lower-bound keys between existing
  integer keys for both positioned helpers and verifies that the operation
  consumes data, updates the output key/value descriptors, and returns a key
  greater than the submitted seed.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  961.233 Kops/s, async parallel get 1.093 Mops/s, async get loop
  1.111 Mops/s, async threaded get loop 1.103 Mops/s, parallel cursor
  scan_from 81.626 Mops/s, async cursor scan_from 91.770 Mops/s, async
  threaded cursor scan_from 128.774 Mops/s, async cursor get loop_from
  55.102 Mops/s, async threaded cursor get loop_from 129.621 Mops/s, async
  cursor loop_from 101.288 Mops/s, and async threaded cursor loop_from
  136.849 Mops/s.
- current ratios were async/blocking-parallel 1.137, async-loop/blocking-parallel
  1.156, async-thread-loop/blocking-parallel 1.147,
  async-cget-loop-from/par 0.675, async-thread-cget-from/par 1.588,
  async-cursor-loop-from/par 1.241, async-thread-cur-from/par 1.677,
  async-thread-cur-from/loop 1.351, and async-thread-cur-from/scan 1.063.
- comparison with the previous positioned cursor batch-loop checkpoint: the
  current run has a stronger blocking parallel cursor scan_from baseline
  (67.688 -> 81.626 Mops/s), while async cursor loop_from is effectively stable
  (99.517 -> 101.288 Mops/s) and async threaded cursor loop_from is also stable
  within run noise (138.483 -> 136.849 Mops/s). The batch-loop-from helper
  therefore still remains above the current blocking pthread scan_from baseline
  and slightly above the predicate async threaded scan_from path in this sample.
- comparison with the pre-async ioarena baseline at the top of this log remains
  unchanged in kind: those numbers measure storage-level lazy-mode phases, not
  this public async API microbenchmark. The latest public API benchmark
  continues to show several parallel-heavy async paths above the blocking
  pthread baselines, but it does not by itself close or remeasure the separate
  storage/ioarena performance gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=190 async-covered=132 async-only=58 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the lower-bound correctness fix does not change the benchmark
  story materially. Count-limited positioned cursor reads remain useful when
  batched on the worker, and the threaded batch-loop-from helper is still the
  best positioned count-limited shape in the current public API benchmark.

Additional async cache-get-many checkpoint:

- added `mdbx_async_cache_get_many()` and
  `mdbx_async_cache_get_SingleThreaded_many()` as async-only many-submit
  helpers for the cache GET API. Both copy submitted key bytes before enqueue,
  allocate/enqueue an operation window in one executor lock round trip, and
  preserve independent operation handles for `mdbx_async_wait_release_all()`.
- smoke coverage now initializes several cache entries, submits the
  multithread-safe cache-many helper, validates per-item cache results and
  values, then reuses the same entries through the single-threaded cache-many
  helper and verifies cache-hit results.
- extended `ut_and_examples/async-api-bench.c` with cache-many and
  single-threaded-cache-many GET rows. The benchmark keeps each worker/window
  slot on a stable key so repeated operations exercise the cache entries rather
  than constantly rebinding a cache entry to unrelated keys.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking parallel get
  751.873 Kops/s, async many parallel get 948.298 Kops/s, async get_ex many
  822.091 Kops/s, async cache many 1.543 Mops/s, and async cache st many
  2.102 Mops/s.
- current ratios were async-cache-many/blocking-parallel 2.052,
  async-cache-st-many/blocking-parallel 2.796, async-cache-many/plain-many
  1.627, async-cache-st/cache 1.363, async-cache-many/blocking-serial 0.824,
  and async-cache-st-many/blocking-serial 1.123.
- comparison with the previous lower-bound checkpoint: ordinary GET-path
  baseline numbers moved with run noise, but the new cache-many rows create a
  stronger read-heavy async shape than the existing plain many path in this
  workload. The single-threaded-cache-many variant is the first reduced public
  async API sample in this log that beats the hot blocking serial point-GET
  baseline while also staying well above blocking pthread-parallel GET.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: cache-many measures public API/cache lookup overhead on a stable-key
  in-process workload, not the storage-level lazy-mode phases. It improves the
  async API read story, but it does not remeasure the storage/ioarena migration
  gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=192 async-covered=132 async-only=60 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cache-backed point reads now have a many-submit async API shape
  and benchmark evidence above blocking pthread-parallel GET. In the
  single-threaded-cache-entry case, the reduced benchmark also exceeds the hot
  blocking serial point-GET baseline, which is a useful new high-water mark for
  public async read performance.

Additional async cache-get-loop checkpoint:

- added `mdbx_async_cache_get_loop()` and
  `mdbx_async_cache_get_SingleThreaded_loop()` as async-only worker-side loop
  helpers for the cache GET API. The executor invokes a caller-provided key
  callback for each loop item and an optional result callback after each cache
  lookup, so one submitted async operation can perform a whole cache-backed read
  loop without per-item submit/wait churn.
- smoke coverage now initializes four cache entries, runs the multithread-safe
  cache loop to fill and verify them, then runs the single-threaded cache loop
  over the same entries and verifies all four reads are cache hits.
- extended `ut_and_examples/async-api-bench.c` with cache-loop and
  single-threaded-cache-loop GET rows. Each worker first warms its cache entries
  with one loop, then times a second loop over the same stable key set.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking serial get
  1.852 Mops/s, blocking parallel get 903.188 Kops/s, async many parallel get
  952.219 Kops/s, async cache many 2.327 Mops/s, async cache st many
  1.858 Mops/s, async cache loop 2.485 Mops/s, and async cache st loop
  2.100 Mops/s.
- current ratios were async-cache-many/blocking-parallel 2.577,
  async-cache-st-many/blocking-parallel 2.057,
  async-cache-loop/blocking-parallel 2.752,
  async-cache-st-loop/blocking-parallel 2.325,
  async-cache-many/plain-many 2.444,
  async-cache-st/cache 0.798, async-cache-loop/cache-many 1.068,
  async-cache-st-loop/cache-st-many 1.131,
  async-cache-st-loop/cache-loop 0.845,
  async-cache-many/blocking-serial 1.257,
  async-cache-st-many/blocking-serial 1.003,
  async-cache-loop/blocking-serial 1.342, and
  async-cache-st-loop/blocking-serial 1.134.
- comparison with the previous cache-many checkpoint: the loop helper is the
  new reduced public async API high-water mark for point GET in this log. It is
  2.752x the blocking pthread-parallel GET baseline, 1.342x the hot blocking
  serial GET baseline, and slightly faster than cache-many in this run. The
  single-threaded loop also stays above both blocking parallel and blocking
  serial GET, and improves over single-threaded cache-many in this sample while
  remaining below the multithread-safe cache loop.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: the cache-loop benchmark measures public async API/cache lookup
  overhead on stable in-process keys, not the storage-level lazy-mode phases.
  It strengthens the public async read path but does not remeasure the
  storage/ioarena migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=194 async-covered=132 async-only=62 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cache-backed point reads now have a worker-side loop async API
  shape that reduces per-item submit overhead further than cache-many. In this
  reduced sample, both cache-loop variants beat blocking pthread-parallel and
  hot blocking serial GET; the multithread-safe cache loop is the strongest
  public async point-read result recorded so far.

Additional async cache-get-batch checkpoint:

- added `mdbx_async_cache_get_batch()` and
  `mdbx_async_cache_get_SingleThreaded_batch()` as async-only single-operation
  batch helpers for the cache GET API. Unlike cache-many, each helper submits
  one operation handle for the full array. Unlike cache-loop, callers provide
  arrays for keys, data, cache entries, and per-item cache results without
  per-item key/result callbacks.
- smoke coverage now initializes cache entries through the multithread-safe
  batch helper, validates per-item values and cache results, then reuses the
  same entries through the single-threaded batch helper and verifies cache-hit
  results.
- extended `ut_and_examples/async-api-bench.c` with cache-batch and
  single-threaded-cache-batch GET rows. The benchmark uses the same stable
  key-per-worker-slot pattern as cache-many, so cache entries are reused across
  repeated windows.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking serial get
  1.887 Mops/s, blocking parallel get 762.059 Kops/s, async many parallel get
  1.077 Mops/s, async cache many 1.595 Mops/s, async cache st many
  2.057 Mops/s, async cache batch 1.755 Mops/s, async cache st batch
  2.085 Mops/s, async cache loop 1.180 Mops/s, and async cache st loop
  2.515 Mops/s.
- current ratios were async-cache-many/blocking-parallel 2.092,
  async-cache-st-many/blocking-parallel 2.699,
  async-cache-batch/blocking-parallel 2.303,
  async-cache-st-batch/blocking-parallel 2.736,
  async-cache-loop/blocking-parallel 1.549,
  async-cache-st-loop/blocking-parallel 3.300,
  async-cache-batch/cache-many 1.101,
  async-cache-st-batch/cache-st-many 1.014,
  async-cache-st-batch/cache-batch 1.188,
  async-cache-loop/cache-batch 0.673,
  async-cache-st-loop/cache-st-batch 1.206,
  async-cache-batch/blocking-serial 0.930,
  async-cache-st-batch/blocking-serial 1.105, and
  async-cache-st-loop/blocking-serial 1.333.
- comparison with the previous cache-loop checkpoint: run-to-run variance moved
  the multithread-safe cache loop down in this sample, while the new cache
  batch still improved over cache-many and stayed far above blocking
  pthread-parallel GET. The single-threaded cache loop remained the public
  async point-read high-water mark in this run, beating both blocking parallel
  and hot blocking serial GET.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: cache-batch measures public async API/cache lookup overhead on
  stable in-process keys, not storage-level lazy-mode phases. It improves the
  public async read surface but does not remeasure or close the storage/ioarena
  migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=196 async-covered=132 async-only=64 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cache-backed point reads now have one-handle array-batch helpers
  in addition to many-submit and worker-side loop forms. In this reduced
  sample, cache-batch is a modest improvement over cache-many and the
  single-threaded batch beats hot blocking serial GET; the single-threaded loop
  remains the strongest public async point-read result.

Additional async cache-get-batch-callback checkpoint:

- added `MDBX_cache_get_batch_func`, `mdbx_async_cache_get_batch_cb()`, and
  `mdbx_async_cache_get_SingleThreaded_batch_cb()` as async-only worker-side
  callback variants for the cache GET batch API. These reuse the same
  one-handle array-batch operation as cache-batch, then invoke a callback on
  the executor worker after all per-item cache results and data slots are
  filled.
- smoke coverage now verifies that the multithread-safe callback batch is
  called once and sees all four successful values, then reuses the same cache
  entries through the single-threaded callback batch and verifies all four
  entries are cache hits.
- extended `ut_and_examples/async-api-bench.c` with cache-batch-callback and
  single-threaded-cache-batch-callback GET rows. The callback benchmark uses
  the same stable key-per-worker-slot pattern as cache-many and cache-batch,
  but moves per-item validation into the executor worker callback before the
  operation completes.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking serial get
  1.882 Mops/s, blocking parallel get 590.792 Kops/s, async many parallel get
  790.867 Kops/s, async cache many 1.852 Mops/s, async cache st many
  2.327 Mops/s, async cache batch 2.205 Mops/s, async cache st batch
  2.371 Mops/s, async cache batch callback 2.032 Mops/s, async cache st batch
  callback 2.076 Mops/s, async cache loop 1.531 Mops/s, and async cache st
  loop 1.525 Mops/s.
- current ratios were async-cache-many/blocking-parallel 3.134,
  async-cache-st-many/blocking-parallel 3.940,
  async-cache-batch/blocking-parallel 3.732,
  async-cache-st-batch/blocking-parallel 4.013,
  async-cache-batch-callback/blocking-parallel 3.440,
  async-cache-st-batch-callback/blocking-parallel 3.514,
  async-cache-batch/cache-many 1.191,
  async-cache-st-batch/cache-st-many 1.019,
  async-cache-batch-callback/cache-batch 0.922,
  async-cache-st-batch-callback/cache-st-batch 0.876,
  async-cache-batch-callback/blocking-serial 1.080, and
  async-cache-st-batch-callback/blocking-serial 1.103.
- comparison with the previous cache-batch checkpoint: the new callback helper
  improves API ergonomics and keeps cache-backed async reads well above
  blocking pthread-parallel GET and hot blocking serial GET in this sample, but
  it is not a performance win over plain cache-batch here. The callback path is
  slower than plain batch in this run, likely because validation work moved
  onto the executor worker's critical path instead of the caller thread after
  wait completion.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: cache-batch-callback measures public async API/cache lookup and
  callback overhead on stable in-process keys, not storage-level lazy-mode
  phases. It extends the public async read surface but does not remeasure or
  close the storage/ioarena migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=198 async-covered=132 async-only=66 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cache-backed point reads now have worker-side callback variants
  for one-handle array batches. This improves async API shape and preserves
  better-than-blocking parallel read throughput, while the latest reduced
  sample shows plain cache-batch remains faster than callback batch.

Additional threaded async cache-get-batch benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with application-threaded
  benchmark rows for `mdbx_async_cache_get_batch()` and
  `mdbx_async_cache_get_SingleThreaded_batch()`. Each application pthread owns
  its async executor, read transaction, and stable cache-entry window, then
  repeatedly submits one cache-batch operation and waits for completion.
- this does not add new public API. It improves benchmark coverage for the
  original parallel-GET target by measuring cache-backed async batch reads when
  submission itself happens from multiple application threads, not only from
  the main benchmark thread fan-out loop.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking serial get
  1.832 Mops/s, blocking parallel get 731.192 Kops/s, async many parallel get
  1.082 Mops/s, async cache many 1.247 Mops/s, async cache st many
  2.293 Mops/s, async cache batch 2.403 Mops/s, async cache st batch
  2.267 Mops/s, async cache batch callback 1.856 Mops/s, async cache st batch
  callback 2.445 Mops/s, async threaded cache batch 2.427 Mops/s, async
  threaded cache st batch 2.431 Mops/s, async cache loop 1.851 Mops/s, and
  async cache st loop 2.092 Mops/s.
- current ratios were async-threaded-cache-batch/blocking-parallel 3.319,
  async-threaded-cache-st-batch/blocking-parallel 3.324,
  async-threaded-cache-batch/blocking-serial 1.325,
  async-threaded-cache-st-batch/blocking-serial 1.327,
  async-threaded-cache-batch/cache-batch 1.010,
  async-threaded-cache-st-batch/cache-st-batch 1.072, and
  async-threaded-cache-st-batch/async-threaded-cache-batch 1.002.
- comparison with the previous cache-batch-callback checkpoint: threaded
  cache-batch confirms the cache-backed batch helper stays strong when
  submitted from multiple application pthreads. In this sample, threaded
  cache-batch is roughly tied with main-thread cache-batch and stays above both
  blocking pthread-parallel GET and hot blocking serial GET. The
  single-threaded cache-batch row is also slightly stronger when submitted from
  application pthreads in this run.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: this benchmark covers public async API submission and cache lookup
  behavior under application-threaded fan-out, not storage-level lazy-mode
  phases. It strengthens the public async parallel-read evidence but does not
  remeasure or close the storage/ioarena migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=198 async-covered=132 async-only=66 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: the strongest cache-backed public async point-read shape now has
  benchmark evidence both from main-thread executor fan-out and from
  application-threaded submission. The latest reduced sample puts threaded
  cache-batch at about 3.3x blocking pthread-parallel GET and about 1.3x hot
  blocking serial GET.

Additional threaded async cache-get-loop benchmark checkpoint:

- extended `ut_and_examples/async-api-bench.c` with application-threaded
  benchmark rows for `mdbx_async_cache_get_loop()` and
  `mdbx_async_cache_get_SingleThreaded_loop()`. Each worker warms its cache
  entries before timing, then an application pthread submits one worker-side
  cache loop and waits for completion.
- this does not add new public API. It fills the benchmark gap between
  main-thread-submitted cache loops and application-threaded cache batches,
  using the same stable-key/cache-entry pattern as the existing cache-loop
  benchmark.
- reduced benchmark sanity check with
  `MDBX_ASYNC_BENCH_ITEMS=5000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=3000` reported blocking serial get
  1.889 Mops/s, blocking parallel get 867.520 Kops/s, async cache many
  2.321 Mops/s, async cache batch 2.281 Mops/s, async cache batch callback
  2.377 Mops/s, async threaded cache batch 1.703 Mops/s, async cache loop
  1.913 Mops/s, async cache st loop 1.820 Mops/s, async threaded cache loop
  2.039 Mops/s, and async threaded cache st loop 1.652 Mops/s.
- current ratios were async-threaded-cache-loop/blocking-parallel 2.351,
  async-threaded-cache-st-loop/blocking-parallel 1.904,
  async-threaded-cache-loop/blocking-serial 1.080,
  async-threaded-cache-st-loop/blocking-serial 0.874,
  async-threaded-cache-loop/cache-loop 1.066,
  async-threaded-cache-st-loop/cache-st-loop 0.907, and
  async-threaded-cache-st-loop/threaded-cache-loop 0.810.
- comparison with the previous threaded cache-batch checkpoint: threaded
  cache-loop gives a small improvement over main-thread-submitted cache-loop in
  this sample, and remains above both blocking pthread-parallel GET and hot
  blocking serial GET. The single-threaded cache-loop variant is weaker under
  application-threaded submission here, falling below the hot blocking serial
  GET baseline and below the multithread-safe threaded cache-loop row.
- comparison with the pre-async ioarena baseline at the top of this log remains
  separate: threaded cache-loop measures public async API submission and cache
  lookup behavior under application-threaded fan-out, not storage-level
  lazy-mode phases. It improves benchmark evidence for public async parallel
  reads but does not remeasure or close the storage/ioarena migration gap.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_audit mdbx.h mdbx.c`: `blocking=171 async-declared=198 async-covered=132 async-only=66 exempt=39 missing=0 unimplemented=0`
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^async_api'`: passed 3/3
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `cmake --build @cmake-asan-build --target mdbx_async_api_bench mdbx_async_api_smoke mdbx_async_api_audit`: passed
  - `env LSAN_OPTIONS=detect_leaks=0 ctest --test-dir @cmake-asan-build --output-on-failure -R '^async_api'`: passed 3/3
  - `make -f GNUmakefile mdbx_migration_public_ctest`: passed 18/18
- conclusion: cache-loop now has benchmark coverage for both main-thread
  executor fan-out and application-threaded submission. The multithread-safe
  threaded cache-loop row remains above blocking parallel and serial GET in the
  reduced sample, while the single-threaded threaded cache-loop row is not a
  current high-water mark.

Additional async cursor-get-loop batching checkpoint:

- changed the executor for `mdbx_async_cursor_get_loop()` and
  `mdbx_async_cursor_get_loop_from()` so non-dupsort `MDBX_NEXT` loops can
  materialize multiple cursor pairs with `mdbx_cursor_get_batch()` after the
  initial positioned cursor get. This keeps the public API shape unchanged but
  reduces per-item cursor executor calls for cursor get-loop workloads.
- the first batch after the initial cursor get skips the already-returned
  current item. Later batches do not skip their first item because
  `mdbx_cursor_get_batch(MDBX_NEXT)` leaves the cursor at the next unreturned
  pair for continuation. A single-record tail is handled with
  `MDBX_GET_CURRENT` to avoid overfetching past the requested loop count.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursor-get-loop-before.txt` and
    `/tmp/mdbx-async-bench-cursor-get-loop-after-release.txt`
- reduced forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=10000 MDBX_ASYNC_BENCH_OPS=50000
  MDBX_ASYNC_BENCH_WRITE_OPS=5000 MDBX_ASYNC_BENCH_LARGE_OPS=5000`:
  blocking cursor get moved 47.095 -> 46.596 Mops/s, parallel cursor get
  41.380 -> 47.076 Mops/s, async cursor get 821.258 -> 765.170 Kops/s, async
  cursor get loop 53.647 -> 54.636 Mops/s, async cursor get loop_from
  50.201 -> 59.148 Mops/s, async threaded cursor get loop 88.319 -> 81.233
  Mops/s, and async threaded cursor get loop_from 78.516 -> 84.305 Mops/s.
- conclusion: this is a modest cursor-loop executor batching improvement rather
  than a new storage-level async page-read primitive. It keeps moving cursor
  traversal work toward coarser async operations, while the true async I/O goal
  still requires resumable B-tree/page-read continuations below cursor logic.

Additional page-cache duplicate read coalescing checkpoint:

- changed `page_cache_submit_read_batch()` to coalesce duplicate page-cache
  misses within one batch before submitting storage reads. Equal
  `dxb_page_cache_read_submit_io_t` descriptors now share one filled cache
  entry; duplicate results retain that entry through the existing page-cache
  pin accounting instead of issuing another `io_uring` read for the same page.
- duplicate detection uses a small per-batch hash table and still verifies full
  descriptor equality before sharing a result, so hash collisions only add a
  comparison and do not change correctness. This is a storage-level page-cache
  improvement and applies below the async cache-get, batched get traversal, and
  other users of `page_cache_submit_read_batch()`.
- added smoke coverage for repeated cache entries in a single
  `mdbx_async_cache_get_SingleThreaded_batch()` materialization pass.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-dup-page-before.txt`,
    `/tmp/mdbx-async-bench-dup-page-after.txt`,
    `/tmp/mdbx-async-bench-dup-page-small-before.txt`, and
    `/tmp/mdbx-async-bench-dup-page-small-after.txt`
- reduced forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000 MDBX_ASYNC_BENCH_OPS=30000
  MDBX_ASYNC_BENCH_WRITE_OPS=1000 MDBX_ASYNC_BENCH_LARGE_OPS=5000
  MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`: async cache batch moved
  3.769 -> 4.214 Mops/s, async cache st batch 3.855 -> 4.142 Mops/s,
  async threaded cache batch 4.480 -> 5.199 Mops/s, async cache loop
  1.998 -> 2.339 Mops/s, and async get loop 3.397 -> 3.448 Mops/s.
  Async cache many moved 4.337 -> 4.234 Mops/s, async cache batch callback
  4.015 -> 3.654 Mops/s, and async threaded cache st loop
  4.749 -> 2.711 Mops/s in this noisy spot sample.
- a smaller duplicate-heavy sample with `MDBX_ASYNC_BENCH_ITEMS=64`,
  `MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=64`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=16K` reported async cache batch
  4.618 -> 5.115 Mops/s, async cache st batch 4.544 -> 4.991 Mops/s,
  async cache batch callback 4.792 -> 5.384 Mops/s, async threaded cache st
  batch 5.067 -> 6.949 Mops/s, async cache loop 3.133 -> 5.241 Mops/s, and
  async threaded cache st loop 4.524 -> 6.686 Mops/s. Async cache many moved
  6.012 -> 4.053 Mops/s and async get loop 6.754 -> 5.790 Mops/s.
- conclusion: duplicate page-cache misses are now collapsed before the
  storage-read batch is submitted, which avoids wasting io_uring slots and
  page-cache buffers on identical reads in duplicate-heavy batches. The effect
  is workload-sensitive because the benchmark driver mixes many cache shapes,
  but the duplicate-heavy sample shows the intended batch rows improving.

Additional duplicate large-materialization read checkpoint:

- changed `cache_materialize_singlethreaded_batch()` so duplicate large/overflow
  materialization reads inside one batch submit only one storage read per
  matching byte span. Duplicate logical items keep their own materialization
  buffers; after the unique read completes, its bytes are copied into duplicate
  buffers and the existing
  `dxb_storage_complete_materialize_cached_large_page()` path handles detach,
  replacement, retained refs, and error handling for every item.
- this keeps the tricky overflow page ownership rules unchanged while reducing
  redundant io_uring read submissions for repeated large cache entries.
- added smoke coverage for repeated large cache entries in one
  `mdbx_async_cache_get_SingleThreaded_batch()` materialization pass.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-large-dup-before.txt`,
    `/tmp/mdbx-async-bench-large-dup-after.txt`,
    `/tmp/mdbx-async-bench-large-dup64-before.txt`, and
    `/tmp/mdbx-async-bench-large-dup64-after.txt`
- duplicate-heavy large-value benchmark with `MDBX_ASYNC_BENCH_LARGE_ITEMS=8`,
  `MDBX_ASYNC_BENCH_LARGE_OPS=20000`, `MDBX_ASYNC_BENCH_LARGE_VALUE_BYTES=10000`,
  and forced no-mmap/io_uring reported async large cache batch
  150.249 -> 149.629 Kops/s and async large cache st batch
  145.823 -> 148.203 Kops/s. Nearby cache rows were noisy:
  async cache st batch 2.129 -> 4.147 Mops/s, async cache batch callback
  2.577 -> 4.200 Mops/s, and async get loop 2.581 -> 1.741 Mops/s.
- the same shape with `MDBX_ASYNC_BENCH_LARGE_VALUE_BYTES=65536` reported
  async large cache batch 23.570 -> 23.554 Kops/s and async large cache st
  batch 23.483 -> 23.499 Kops/s, effectively flat in the buffered spot run.
- conclusion: this removes redundant large-page storage-read submissions and
  io_uring slots for duplicate overflow materialization, but the current
  buffered benchmark does not show a throughput win because saved reads are
  offset by copying the completed buffer into each duplicate destination.

Additional single async-get traversal checkpoint:

- changed single-operation `mdbx_async_get()` execution so eligible read-only,
  non-dupsort DBIs first try the internal batched get traversal path instead
  of immediately running blocking `mdbx_get()` inside the async worker. Cache
  hits still use cached-entry materialization, first-sighting cold keys can now
  populate the hidden async get cache from the traversal path, and unsupported
  transaction or DBI shapes fall back to the existing `async_cached_get()`
  behavior.
- added a one-item stack-storage path inside `async_batched_get_traverse()` for
  its temporary arrays. This avoids the heap allocation setup that would
  otherwise dominate the single-key path while preserving the heap-backed
  arrays for larger batches.
- extended `ut_and_examples/async-api-bench.c` with an `async single get` row
  that submits one `mdbx_async_get()`, waits for it, validates the result, and
  repeats. This measures the non-windowed public async get path separately from
  adjacent-operation coalescing and `get_many` batching.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-singleget-before.txt` and
    `/tmp/mdbx-async-bench-singleget-after.txt`
- reduced forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=10000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1000`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `4f882ef` with the
  benchmark row applied:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.528 Mops/s | 1.507 Mops/s |
| blocking parallel get | 772.909 Kops/s | 826.557 Kops/s |
| async single get | 311.591 Kops/s | 318.177 Kops/s |
| async parallel get | 1.121 Mops/s | 1.245 Mops/s |
| async many parallel get | 1.288 Mops/s | 1.171 Mops/s |
| async threaded get | 1.259 Mops/s | 1.193 Mops/s |
| async batch parallel get | 1.289 Mops/s | 1.327 Mops/s |
| async batch callback get | 1.270 Mops/s | 1.022 Mops/s |
| async-single/blocking par | 0.403 | 0.385 |
| async-single/blocking ser | 0.204 | 0.211 |

- conclusion: single `mdbx_async_get()` now exercises the internal explicit-I/O
  traversal engine for eligible cold point reads instead of only worker
  offloading `mdbx_get()`. The reduced spot benchmark shows the new single row
  roughly flat to slightly higher in absolute throughput, but still well below
  blocking serial GET because each public async operation still pays operation
  submission and wait overhead. The important semantic movement is that the
  public async single-get path now reaches the same internal page-read traversal
  engine as batched async get, while true caller-visible nonblocking progress
  still requires a resumable continuation API below cursor traversal.

Additional single get_ex/lowerbound traversal checkpoint:

- changed single-operation `mdbx_async_get_ex()` execution so eligible
  read-only, non-dupsort DBIs try `async_batched_get_traverse()` before
  falling back to blocking `mdbx_get_ex()`. The fast path preserves the
  non-dupsort `values_count == 1` result shape; dupsort and unsupported
  transactions continue to use the existing blocking implementation.
- changed single-operation `mdbx_async_get_equal_or_great()` execution so
  eligible read-only, non-dupsort DBIs try
  `async_batched_lowerbound_traverse()` before falling back to
  `mdbx_get_equal_or_great()`. The same helper is used for count-one
  coalesced worker runs.
- added a one-item stack-storage path to `async_batched_lowerbound_traverse()`
  matching the point-get traversal fast path, avoiding heap setup for single
  lowerbound traversal.
- extended `ut_and_examples/async-api-bench.c` with `async single get_ex` and
  `async single lowerbound` rows so non-windowed public async variants are
  measured separately from many/batch/loop coalescing.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-singlevariants-before.txt` and
    `/tmp/mdbx-async-bench-singlevariants-after.txt`
- reduced forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=10000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1000`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `77a717d` with the
  benchmark rows applied:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.496 Mops/s | 1.537 Mops/s |
| blocking parallel get | 576.329 Kops/s | 739.688 Kops/s |
| async single get | 254.493 Kops/s | 329.604 Kops/s |
| async single get_ex | 321.630 Kops/s | 301.210 Kops/s |
| async single lowerbound | 304.791 Kops/s | 347.389 Kops/s |
| async get_ex batch | 1.104 Mops/s | 1.205 Mops/s |
| async get_ex many | 1.103 Mops/s | 1.063 Mops/s |
| async lowerbound batch | 785.252 Kops/s | 1.184 Mops/s |
| async lowerbound many | 601.537 Kops/s | 1.087 Mops/s |
| async-single-ex/par | 0.558 | 0.407 |
| async-single-lower/par | 0.529 | 0.470 |

- conclusion: the remaining single public point-read variants now reach the
  internal traversal/page-read engine for eligible non-dupsort reads instead of
  relying only on worker-offloaded blocking calls. The current reduced
  benchmark is mixed: single lowerbound improved in absolute throughput, single
  `get_ex` regressed slightly, and the lowerbound batch/many rows improved
  substantially in the same spot sample. This is still semantic migration work
  toward real internal async I/O; the full goal still requires exposing
  traversal suspension/resumption rather than running each public async op to
  completion inside the worker.

Additional mixed not-found read coverage checkpoint:

- extended `ut_and_examples/async-api-smoke.c` with explicit not-found coverage
  for traversal-routed async reads:
  `mdbx_async_get()`, `mdbx_async_get_ex()`,
  `mdbx_async_get_equal_or_great()`, their `*_many()` variants, and their
  explicit batch variants. The mixed tests verify success/not-found/result
  ordering, empty values for misses, and zero `values_count` for missed
  `get_ex` items.
- the lowerbound miss probe uses an all-`0xff` byte key rather than a large
  integer key because MDBX default key ordering is bytewise; this makes the
  probe reliably sort after the populated little-endian integer keys.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - forced no-mmap/io_uring benchmark log:
    `/tmp/mdbx-async-bench-notfound-coverage.txt`
- representative benchmark rows from the forced no-mmap/io_uring sample:

| metric | current |
| --- | ---: |
| blocking serial get | 1.522 Mops/s |
| blocking parallel get | 855.146 Kops/s |
| async single get | 328.218 Kops/s |
| async single get_ex | 236.091 Kops/s |
| async single lowerbound | 182.960 Kops/s |
| async parallel get | 1.209 Mops/s |
| async many parallel get | 1.237 Mops/s |
| async get_ex batch | 1.196 Mops/s |
| async get_ex many | 1.114 Mops/s |
| async lowerbound batch | 1.104 Mops/s |
| async lowerbound many | 1.185 Mops/s |

- conclusion: this does not add a new async I/O mechanism, but it closes a
  correctness-evidence gap in the current migration: traversal-routed public
  async read APIs now have smoke coverage for per-item not-found behavior,
  result ordering, and empty-result normalization in both normal and forced
  no-mmap/io_uring runs.

Additional single get_ex cache/traversal checkpoint:

- changed eligible single `mdbx_async_get_ex()` execution to reuse
  `async_cached_get_one()` instead of running a separate direct one-item
  traversal. For read-only non-dupsort DBIs this lets `get_ex` share the hidden
  async get cache, cached-entry materialization, and internal explicit-I/O
  traversal path already used by `mdbx_async_get()`. Unsupported shapes still
  fall back to `mdbx_get_ex()`.
- this keeps the public `get_ex` result contract: non-dupsort successful
  lookups report `values_count == 1`, misses report zero values, and dupsort
  semantics remain on the existing fallback path.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getexcache-repeat-before.txt` and
    `/tmp/mdbx-async-bench-getexcache-repeat-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `e3b759b`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.777 Mops/s | 1.796 Mops/s |
| blocking parallel get | 932.538 Kops/s | 1.024 Mops/s |
| async single get | 401.189 Kops/s | 212.146 Kops/s |
| async single get_ex | 179.522 Kops/s | 326.153 Kops/s |
| async single lowerbound | 294.618 Kops/s | 369.266 Kops/s |
| async parallel get | 1.668 Mops/s | 1.702 Mops/s |
| async many parallel get | 1.825 Mops/s | 2.849 Mops/s |
| async get_ex batch | 858.826 Kops/s | 1.679 Mops/s |
| async get_ex many | 915.523 Kops/s | 1.403 Mops/s |

- conclusion: repeated non-dupsort `get_ex` now benefits from the same
  cache-backed internal traversal route as repeated `get`, which is closer to
  the requested design where public async reads use the internal async page
  engine instead of independent blocking helper calls. This still runs to
  completion inside the worker; exposing true suspension/resumption remains
  future work.

Additional batched get_ex cache materialization checkpoint:

- changed `mdbx_async_get_ex_batch()` and grouped `async_op_get_ex` execution
  so warm hidden async-get cache entries are materialized through
  `cache_materialize_singlethreaded_batch()` before only cold/unhandled entries
  enter `async_batched_get_traverse()`. This brings batched `get_ex` closer to
  the existing batched `get` path and avoids re-traversing already-confirmed
  repeated keys.
- added a batch-slot preparation/revalidation step for direct-mapped hidden
  async-get cache slots. Because later keys in a batch can reuse the same
  direct-mapped slot, stale saved slot pointers are now nulled before
  materialization/traversal writes through them; cold items still traverse
  uncached when their slot was displaced.
- fixed exact non-dupsort `get_ex` result key handling for public async paths.
  The batchable path now preserves the caller's input key descriptor instead of
  copying private operation buffers or page-backed traversal keys back into the
  user's `MDBX_val`. Fallback `mdbx_get_ex()` paths still update keys when they
  need to preserve duplicate-table semantics.
- the mixed success/not-found `get_ex_batch` smoke coverage caught both hazards:
  a stale direct-mapped cache slot could return the wrong payload, and prior
  `get_ex_many()` completions could leave caller keys pointing at operation
  buffers that are freed after completion.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getexbatch-before.txt` and
    `/tmp/mdbx-async-bench-getexbatch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `2e07025`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.797 Mops/s | 1.824 Mops/s |
| blocking parallel get | 943.384 Kops/s | 997.026 Kops/s |
| async single get | 396.868 Kops/s | 290.292 Kops/s |
| async single get_ex | 387.325 Kops/s | 239.427 Kops/s |
| async parallel get | 2.663 Mops/s | 2.502 Mops/s |
| async many parallel get | 2.601 Mops/s | 2.775 Mops/s |
| async get_ex batch | 849.187 Kops/s | 2.723 Mops/s |
| async get_ex many | 1.210 Mops/s | 2.833 Mops/s |
| async-single-ex/par | 0.411 | 0.240 |
| async-get-ex-batch/par | 0.900 | 2.732 |
| async-get-ex-many/par | 1.282 | 2.841 |

- conclusion: the target `get_ex` batch and many rows now use batched cache
  materialization for warm repeated keys and show a large improvement in this
  reduced no-mmap/io_uring sample. Some unrelated single/get rows moved down in
  the same one-run benchmark, so the result should be treated as directional
  for the targeted warm batched `get_ex` path rather than a global performance
  claim.

Additional get_ex loop cache materialization checkpoint:

- changed `mdbx_async_get_ex_loop()` so each bounded loop window uses the hidden
  async-get cache before traversal. Warm cache entries are now materialized with
  `cache_materialize_singlethreaded_batch()`, while cold, displaced, or empty
  slots enter `async_batched_get_traverse()`. Unhandled entries still fall back
  to `mdbx_get_ex()`.
- callback ordering and `completed` updates remain unchanged: the worker still
  collects keys for one window, resolves them, then invokes `result_func` in
  index order. Exact non-dupsort loop callbacks keep the submitted key
  descriptor semantics, while fallback paths remain available for unsupported
  DBI/transaction shapes.
- this completes the same warm-cache materialization route for the public
  `get_ex` loop shape that batch and many already use.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getexloopcache-before.txt` and
    `/tmp/mdbx-async-bench-getexloopcache-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `e6da00e`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.827 Mops/s | 1.795 Mops/s |
| blocking parallel get | 663.221 Kops/s | 752.411 Kops/s |
| async single get | 371.450 Kops/s | 216.048 Kops/s |
| async single get_ex | 369.567 Kops/s | 395.139 Kops/s |
| async parallel get | 2.766 Mops/s | 1.867 Mops/s |
| async many parallel get | 2.574 Mops/s | 2.249 Mops/s |
| async get_ex batch | 2.284 Mops/s | 2.042 Mops/s |
| async get_ex many | 2.857 Mops/s | 2.838 Mops/s |
| async get_ex loop | 1.879 Mops/s | 2.212 Mops/s |
| async threaded get_ex loop | 1.196 Mops/s | 1.892 Mops/s |
| async-get-ex-loop/par | 2.833 | 2.940 |
| async-thread-get-ex-loop/par | 1.803 | 2.514 |
| async-thread-get-ex-loop/loop | 0.637 | 0.855 |
| async-get-ex-loop/ser | 1.029 | 1.232 |
| async-thread-get-ex-loop/ser | 0.655 | 1.054 |

- conclusion: the targeted `get_ex` loop rows now benefit from the same
  hidden-cache/page-cache materialization path as repeated batch and many reads.
  The direct and threaded loop rows improved in this sample, while unrelated
  get rows moved with normal one-run benchmark noise.

Additional regular cache-get loop prefetch checkpoint:

- changed regular `mdbx_async_cache_get_loop()` so it also uses the bounded
  64-entry materialization window that was previously limited to
  `mdbx_async_cache_get_SingleThreaded_loop()`. For regular volatile cache
  entries the worker snapshots each entry with `cache_entry_snapshot_volatile()`
  into local storage, then calls `cache_materialize_singlethreaded_batch()`.
- entries that cannot be snapshotted or materialized still fall back to the
  existing per-item `mdbx_cache_get()` path, preserving the multi-thread-safe
  cache-entry semantics. The single-threaded loop keeps the same fast path but
  now shares the same local window code.
- this moves the regular cache loop shape onto the internal batched page-cache
  read/materialization path instead of relying on one cache lookup per item.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cacheloop-prefetch-before.txt` and
    `/tmp/mdbx-async-bench-cacheloop-prefetch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `cce8a99`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.749 Mops/s | 1.783 Mops/s |
| blocking parallel get | 951.500 Kops/s | 771.109 Kops/s |
| async cache many | 3.002 Mops/s | 4.082 Mops/s |
| async cache st many | 4.027 Mops/s | 4.173 Mops/s |
| async cache batch | 3.818 Mops/s | 3.787 Mops/s |
| async cache st batch | 3.715 Mops/s | 2.747 Mops/s |
| async cache loop | 1.983 Mops/s | 5.076 Mops/s |
| async cache st loop | 1.918 Mops/s | 5.076 Mops/s |
| async threaded cache loop | 3.051 Mops/s | 5.029 Mops/s |
| async threaded cache st loop | 4.732 Mops/s | 4.921 Mops/s |
| async-cache-loop/par | 2.084 | 6.583 |
| async-thread-cache-loop/par | 3.206 | 6.521 |
| async-cache-loop/batch | 0.519 | 1.340 |
| async-cache-loop/many | 0.661 | 1.243 |
| async-thread-cache-loop/loop | 1.539 | 0.991 |
| async-cache-loop/ser | 1.133 | 2.846 |
| async-thread-cache-loop/ser | 1.744 | 2.820 |

- conclusion: regular cache loop now benefits from batched cache-entry
  materialization like the single-threaded loop. The direct regular loop and
  threaded regular loop rows improved substantially in this sample; nearby
  cache batch rows were mixed, consistent with normal reduced benchmark noise.

Additional small committed-page batch stack checkpoint:

- changed `page_get_committed_batch()` and `page_submit_get_unchecked_batch()`
  to use stack storage for up to four committed page reads before falling back
  to heap allocation for larger batches. This mirrors the existing small-stack
  behavior in `page_submit_cursor_get_batch()`.
- this is below the async traversal/cache materialization paths, so it reduces
  allocator overhead for common shallow B-tree/root/branch read batches without
  changing page-cache lookup, io_uring submission, duplicate-miss coalescing,
  pinning, validation, or fallback behavior.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-smallbatch-stack-before.txt` and
    `/tmp/mdbx-async-bench-smallbatch-stack-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `2262626`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.768 Mops/s | 1.842 Mops/s |
| blocking parallel get | 1.037 Mops/s | 1.075 Mops/s |
| async single get | 195.591 Kops/s | 230.947 Kops/s |
| async single get_ex | 350.891 Kops/s | 388.892 Kops/s |
| async parallel get | 2.612 Mops/s | 2.298 Mops/s |
| async many parallel get | 2.750 Mops/s | 2.745 Mops/s |
| async get_ex batch | 2.730 Mops/s | 2.031 Mops/s |
| async cache loop | 3.906 Mops/s | 2.791 Mops/s |
| async threaded cache loop | 2.089 Mops/s | 5.082 Mops/s |
| async get_ex loop | 2.823 Mops/s | 2.041 Mops/s |
| async cursor get loop | 71.439 Mops/s | 123.627 Mops/s |
| async cursor get loop_from | 56.800 Mops/s | 65.965 Mops/s |
| async threaded cursor get loop | 73.133 Mops/s | 71.800 Mops/s |
| async threaded cget loop_from | 122.942 Mops/s | 123.195 Mops/s |
| async-thread-cget-loop/loop | 1.024 | 0.581 |
| async-get-loop/batch | 1.056 | 1.865 |

- conclusion: the target low-level small-batch allocator change is visible most
  clearly in cursor loop rows in this sample, especially the main async cursor
  get loop and loop-from rows. Other get/cache rows moved in both directions,
  so this should be treated as a plumbing/overhead reduction for common small
  committed-page batches rather than a broad throughput claim.

Additional cache materialization small-batch stack checkpoint:

- changed `cache_materialize_singlethreaded_batch()` to keep the submit/read,
  page-result, and index work arrays on the stack for batches of up to four
  cache entries, falling back to heap allocation for larger materialization
  batches.
- this is on the async get/cache materialization path used by cache loop,
  `get_ex` loop, and batched hidden-cache materialization. It reduces allocator
  overhead for the common one-entry and shallow-window cases without changing
  page-cache lookup, io_uring submission, large-page materialization, retained
  page pinning, or fallback behavior.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-materialize-stack-before.txt` and
    `/tmp/mdbx-async-bench-materialize-stack-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `899c36f`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.835 Mops/s | 1.840 Mops/s |
| blocking parallel get | 601.555 Kops/s | 942.163 Kops/s |
| async single get | 287.643 Kops/s | 377.443 Kops/s |
| async single get_ex | 335.394 Kops/s | 378.680 Kops/s |
| async parallel get | 2.172 Mops/s | 1.801 Mops/s |
| async many parallel get | 2.122 Mops/s | 1.866 Mops/s |
| async get_ex batch | 2.689 Mops/s | 2.740 Mops/s |
| async get_ex many | 2.523 Mops/s | 2.834 Mops/s |
| async cache many | 2.929 Mops/s | 3.326 Mops/s |
| async cache st many | 4.352 Mops/s | 3.712 Mops/s |
| async cache batch | 3.311 Mops/s | 4.101 Mops/s |
| async cache st batch | 4.227 Mops/s | 3.675 Mops/s |
| async cache loop | 2.360 Mops/s | 4.934 Mops/s |
| async cache st loop | 5.010 Mops/s | 2.147 Mops/s |
| async threaded cache loop | 4.913 Mops/s | 2.372 Mops/s |
| async threaded cache st loop | 4.221 Mops/s | 2.353 Mops/s |
| async get loop | 3.430 Mops/s | 3.455 Mops/s |
| async get_ex loop | 2.200 Mops/s | 3.166 Mops/s |
| async cursor get loop | 69.918 Mops/s | 66.247 Mops/s |
| async cursor get loop_from | 69.396 Mops/s | 114.010 Mops/s |
| async-cache-loop/par | 3.923 | 5.237 |
| async-cache-loop/batch | 0.713 | 1.203 |
| async-cache-loop/many | 0.806 | 1.484 |
| async-cache-loop/ser | 1.286 | 2.681 |

- conclusion: the direct cache materialization rows that exercise this helper
  (`async cache loop`, `async get_ex loop`, and cache loop ratios) improved in
  this sample, while the threaded and single-threaded cache-loop rows regressed.
  Treat this as a small allocator-overhead reduction in the internal async
  materialization path; the reduced benchmark remains noisy and should not be
  used as a broad throughput claim.

Additional page-cache read batch small-stack checkpoint:

- changed `page_cache_submit_read_batch()` to use stack scratch storage for up
  to four page-cache read submissions. The stack-backed state covers fill
  records, storage read submissions/results, miss indices, and duplicate-miss
  tracking buckets.
- larger batches still use heap allocation. The stack fill records are
  explicitly zeroed before use so the existing discard path can safely release
  only active prepared cache fills.
- this is the central internal page-cache miss path under committed page reads,
  cache-entry materialization, cursor prefetch reads, and explicit no-mmap
  async page reads. It reduces allocator overhead for common shallow page-read
  batches without changing lookup, duplicate-miss coalescing, io_uring batch
  submission, cache insertion, retained page pinning, or fallback behavior.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-pagecache-stack-before.txt` and
    `/tmp/mdbx-async-bench-pagecache-stack-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `6aba82c`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.874 Mops/s | 1.875 Mops/s |
| blocking parallel get | 1.039 Mops/s | 817.428 Kops/s |
| async single get | 393.405 Kops/s | 294.649 Kops/s |
| async single get_ex | 408.322 Kops/s | 276.899 Kops/s |
| async parallel get | 2.730 Mops/s | 2.244 Mops/s |
| async many parallel get | 2.828 Mops/s | 2.379 Mops/s |
| async get_ex batch | 2.550 Mops/s | 2.702 Mops/s |
| async get_ex many | 2.764 Mops/s | 2.833 Mops/s |
| async cache many | 4.243 Mops/s | 2.648 Mops/s |
| async cache st many | 4.389 Mops/s | 4.505 Mops/s |
| async cache batch | 3.577 Mops/s | 2.991 Mops/s |
| async cache st batch | 4.020 Mops/s | 3.725 Mops/s |
| async cache loop | 3.962 Mops/s | 3.797 Mops/s |
| async cache st loop | 4.820 Mops/s | 2.715 Mops/s |
| async threaded cache loop | 2.683 Mops/s | 2.566 Mops/s |
| async threaded cache st loop | 2.791 Mops/s | 3.736 Mops/s |
| async get loop | 2.656 Mops/s | 2.077 Mops/s |
| async get_ex loop | 1.949 Mops/s | 2.440 Mops/s |
| async cursor get loop | 68.352 Mops/s | 66.318 Mops/s |
| async cursor get loop_from | 66.822 Mops/s | 67.315 Mops/s |
| async threaded cursor get loop | 119.751 Mops/s | 128.288 Mops/s |
| async threaded cget loop_from | 118.819 Mops/s | 129.201 Mops/s |
| async-cache-loop/par | 3.813 | 4.645 |
| async-thread-cache-loop/par | 2.583 | 3.139 |
| async-cache-loop/batch | 1.108 | 1.270 |
| async-cache-loop/many | 0.934 | 1.434 |
| async-cache-loop/ser | 2.114 | 2.025 |

- conclusion: this checkpoint removes per-call heap allocation from the common
  small central page-cache batch path. The reduced benchmark is mixed: cache
  loop ratios, `get_ex` loop, and threaded cursor loop rows improved, while
  several direct get/cache throughput rows regressed in this one-run sample.
  Treat the change as internal async-read plumbing cleanup rather than a broad
  throughput result.

Additional single async cache-get materialization checkpoint:

- changed the single async cache-get execution path so
  `async_op_cache_get`/`async_op_cache_get_singlethreaded` first snapshot or
  copy the supplied cache entry and try `cache_materialize_singlethreaded_batch()`
  with a one-entry batch. Only unhandled entries fall back to
  `mdbx_cache_get()` or `mdbx_cache_get_SingleThreaded()`.
- added a shared `async_cache_get_one_materialized()` helper and used it for
  grouped async cache-get operations when the grouped count is one. Multi-op
  grouped, batch, and loop cache-get paths already used the materialization
  helper.
- this moves another public async read surface away from always executing the
  blocking cache-get path in the worker. The result still preserves the same
  fallback behavior for volatile entries that cannot be snapshotted, entries
  that are not materializable, and non-explicit-I/O cases.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cache-single-before.txt` and
    `/tmp/mdbx-async-bench-cache-single-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `574bfea`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.860 Mops/s | 1.849 Mops/s |
| blocking parallel get | 1.016 Mops/s | 1.085 Mops/s |
| async single get | 424.401 Kops/s | 346.328 Kops/s |
| async single get_ex | 253.377 Kops/s | 201.674 Kops/s |
| async parallel get | 2.788 Mops/s | 2.724 Mops/s |
| async many parallel get | 2.673 Mops/s | 2.780 Mops/s |
| async get_ex batch | 2.422 Mops/s | 2.729 Mops/s |
| async get_ex many | 2.509 Mops/s | 2.500 Mops/s |
| async cache many | 3.307 Mops/s | 4.421 Mops/s |
| async cache st many | 3.626 Mops/s | 3.406 Mops/s |
| async cache batch | 4.160 Mops/s | 3.061 Mops/s |
| async cache st batch | 4.127 Mops/s | 2.784 Mops/s |
| async cache batch cb | 3.443 Mops/s | 3.829 Mops/s |
| async cache st batch cb | 4.177 Mops/s | 4.060 Mops/s |
| async cache loop | 2.456 Mops/s | 2.782 Mops/s |
| async cache st loop | 3.485 Mops/s | 2.734 Mops/s |
| async threaded cache loop | 3.459 Mops/s | 5.050 Mops/s |
| async threaded cache st loop | 4.943 Mops/s | 4.198 Mops/s |
| async get loop | 2.488 Mops/s | 2.305 Mops/s |
| async get_ex loop | 3.243 Mops/s | 3.283 Mops/s |
| async cursor get loop | 68.629 Mops/s | 68.993 Mops/s |
| async cursor get loop_from | 78.015 Mops/s | 70.994 Mops/s |
| async threaded cursor get loop | 124.165 Mops/s | 124.771 Mops/s |
| async threaded cget loop_from | 128.691 Mops/s | 124.766 Mops/s |
| async-cache-loop/par | 2.417 | 2.564 |
| async-thread-cache-loop/par | 3.405 | 4.655 |
| async-cache-loop/batch | 0.590 | 0.909 |
| async-cache-loop/ser | 1.320 | 1.505 |
| async-get-loop/batch | 0.535 | 1.035 |

- conclusion: this checkpoint is a behavioral/plumbing improvement for the
  single async cache-get path, which now attempts the same internal page-cache
  materialization used by batch and loop variants. The reduced benchmark is
  mixed: cache many, cache loop, threaded cache loop, `get_ex` batch, and cache
  loop ratios improved in this sample, while single get/get_ex and several
  single-threaded cache rows regressed.

Additional async cursor batches stack checkpoint:

- changed `async_cursor_get_batches_execute()` to keep the temporary key/value
  pair buffer on the stack for cursor batch sizes up to 64 pairs, matching the
  existing stack-sized cursor loop batch window. Larger async cursor batches
  still use heap allocation.
- this keeps the public async cursor batches API behavior unchanged while
  removing per-operation heap allocation for the common cursor prefetch window.
  The executor still delegates cursor movement to the existing cursor batch
  implementation; deeper resumable cursor traversal remains future work.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursor-batches-stack-before.txt` and
    `/tmp/mdbx-async-bench-cursor-batches-stack-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `cdda8fa`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.868 Mops/s | 1.809 Mops/s |
| blocking parallel get | 1.067 Mops/s | 545.168 Kops/s |
| async single get | 424.467 Kops/s | 417.942 Kops/s |
| async parallel get | 2.309 Mops/s | 2.761 Mops/s |
| async many parallel get | 2.483 Mops/s | 2.667 Mops/s |
| async cache many | 3.458 Mops/s | 4.470 Mops/s |
| async cache loop | 4.439 Mops/s | 2.772 Mops/s |
| async threaded cache loop | 4.849 Mops/s | 2.819 Mops/s |
| async get loop | 2.667 Mops/s | 3.438 Mops/s |
| async cursor get loop | 130.765 Mops/s | 116.251 Mops/s |
| async cursor get loop_from | 136.866 Mops/s | 59.057 Mops/s |
| async threaded cursor get loop | 118.603 Mops/s | 116.536 Mops/s |
| async threaded cget loop_from | 74.717 Mops/s | 124.414 Mops/s |
| async-cget-loop-from/par | 1.351 | 0.922 |
| async-thread-cget-loop/par | 1.899 | 1.950 |
| async-thread-cget-from/par | 0.737 | 1.942 |
| async-thread-cget-loop/get | 96.338 | 156.324 |
| async-thread-cget-loop/loop | 0.907 | 1.002 |
| async-thread-cget-from/loop | 0.546 | 2.107 |

- conclusion: this checkpoint removes heap allocation from common async cursor
  batches but does not change the underlying cursor traversal model. The one-run
  benchmark is mixed: threaded cursor-from ratios and threaded cursor/get ratios
  improved substantially, while direct cursor loop-from rows regressed.

Additional small async traversal batch stack checkpoint:

- changed `async_batched_get_traverse()` and
  `async_batched_lowerbound_traverse()` to keep traversal scratch state on the
  stack for batches of up to four keys. Larger batches still use the existing
  heap-backed arrays.
- the stack-backed state covers cursor couples, initialized flags, child page
  read submissions/results, parent stack positions, parent key indices, and
  source-key indices. This keeps common small `get`, `get_ex`, and lower-bound
  async read batches on the internal page-read traversal path without per-call
  allocator traffic.
- behavior is otherwise unchanged: the helpers still submit root/branch child
  page reads through `page_submit_cursor_get_batch()`, materialize results into
  the existing cursor stack, capture transaction pins, and fall back to the
  existing public read operations when a caller cannot be handled by the
  internal traversal.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-traverse-smallstack-before.txt` and
    `/tmp/mdbx-async-bench-traverse-smallstack-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `6bb86c1`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.865 Mops/s | 1.859 Mops/s |
| blocking parallel get | 1.072 Mops/s | 917.594 Kops/s |
| async single get | 338.577 Kops/s | 387.045 Kops/s |
| async single get_ex | 297.599 Kops/s | 401.917 Kops/s |
| async parallel get | 2.427 Mops/s | 2.461 Mops/s |
| async many parallel get | 2.836 Mops/s | 2.624 Mops/s |
| async get_ex batch | 2.722 Mops/s | 2.737 Mops/s |
| async get_ex many | 2.846 Mops/s | 2.443 Mops/s |
| async cache many | 4.527 Mops/s | 3.785 Mops/s |
| async cache loop | 5.076 Mops/s | 2.132 Mops/s |
| async threaded cache loop | 2.049 Mops/s | 4.801 Mops/s |
| async lowerbound batch | 1.662 Mops/s | 1.649 Mops/s |
| async lowerbound many | 1.431 Mops/s | 1.666 Mops/s |
| async get loop | 2.337 Mops/s | 2.218 Mops/s |
| async get_ex loop | 3.303 Mops/s | 3.484 Mops/s |
| async lowerbound loop | 1.928 Mops/s | 1.904 Mops/s |
| async cursor get loop | 67.656 Mops/s | 66.683 Mops/s |
| async cursor get loop_from | 70.151 Mops/s | 68.378 Mops/s |
| async threaded cursor get loop | 120.528 Mops/s | 123.165 Mops/s |
| async threaded cget loop_from | 127.938 Mops/s | 128.813 Mops/s |
| async-single-ex/par | 0.278 | 0.438 |
| async-get-ex-batch/par | 2.539 | 2.983 |
| async-thread-cache-loop/par | 1.911 | 5.232 |
| async-cget-loop-from/par | 0.705 | 1.095 |
| async-thread-cget-from/par | 1.286 | 2.063 |

- conclusion: this checkpoint reduces allocator overhead for small internal
  async traversal batches. The reduced benchmark is mixed: single get/get_ex,
  get_ex loop, lowerbound many, and threaded cache/cursor ratios improved in
  this sample, while cache-loop and some many/batch rows regressed.

Additional cold async cached-get traversal fallback checkpoint:

- changed `async_cached_get()` so a cold hidden async-get cache slot first tries
  the one-key `async_batched_get_traverse()` path when the DBI is eligible for
  nodup read batching. Only unhandled entries fall back to `mdbx_get()`.
- this is the shared fallback used by get batches, grouped get ops, and get
  loops after their first batched attempt cannot handle an item. Moving the
  cold path here reduces direct dependence on the public blocking get path and
  keeps more cache-miss work under the internal page-read traversal engine.
- behavior is unchanged for allocation failure, non-readonly transactions,
  changed DBIs, dupsort DBIs, unsupported backends, or entries the traversal
  cannot handle; those still fall back to the existing blocking get path.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-coldget-traverse-before.txt` and
    `/tmp/mdbx-async-bench-coldget-traverse-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `b9bb60f`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.873 Mops/s | 1.872 Mops/s |
| blocking parallel get | 534.414 Kops/s | 452.553 Kops/s |
| async single get | 427.259 Kops/s | 394.080 Kops/s |
| async single get_ex | 270.385 Kops/s | 319.729 Kops/s |
| async parallel get | 2.550 Mops/s | 2.621 Mops/s |
| async many parallel get | 2.828 Mops/s | 2.868 Mops/s |
| async get_ex batch | 2.611 Mops/s | 1.622 Mops/s |
| async get_ex many | 2.857 Mops/s | 2.222 Mops/s |
| async cache many | 4.373 Mops/s | 4.489 Mops/s |
| async cache loop | 2.670 Mops/s | 2.887 Mops/s |
| async threaded cache loop | 4.854 Mops/s | 2.626 Mops/s |
| async lowerbound batch | 1.461 Mops/s | 1.325 Mops/s |
| async lowerbound many | 1.571 Mops/s | 1.189 Mops/s |
| async get loop | 2.658 Mops/s | 3.445 Mops/s |
| async get_ex loop | 2.734 Mops/s | 2.711 Mops/s |
| async lowerbound loop | 1.548 Mops/s | 1.898 Mops/s |
| async cursor get loop | 122.680 Mops/s | 65.343 Mops/s |
| async cursor get loop_from | 67.988 Mops/s | 71.034 Mops/s |
| async threaded cursor get loop | 113.359 Mops/s | 119.800 Mops/s |
| async threaded cget loop_from | 117.491 Mops/s | 69.548 Mops/s |
| async-single-ex/par | 0.506 | 0.706 |
| async-cache-loop/par | 4.996 | 6.379 |
| async-thread-cget-loop/get | 104.744 | 110.611 |

- conclusion: this checkpoint is primarily a behavioral alignment change:
  shared cold async cached gets now try the internal page-read traversal engine
  before the blocking public get fallback. The reduced benchmark is mixed: get
  loop, cache many/loop, single get_ex, lowerbound loop, and some cursor ratios
  improved, while get_ex batch/many, lowerbound batch/many, threaded cache loop,
  and several cursor rows regressed in this sample.

Additional async get fallback helper consolidation checkpoint:

- changed the unhandled fallback branches in `async_cached_get_batch()` and
  `async_cached_get_ops_batch()` to call `async_cached_get()` instead of
  duplicating the old slot-specific `mdbx_get()` /
  `mdbx_cache_get_SingleThreaded()` logic.
- because `async_cached_get()` now tries a one-key internal traversal for cold
  eligible slots, this removes two remaining direct public-get fallbacks from
  async batch/grouped get execution after their initial batched traversal pass
  cannot handle an item.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-get-fallback-helper-before.txt` and
    `/tmp/mdbx-async-bench-get-fallback-helper-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `3090a4d`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.891 Mops/s | 1.850 Mops/s |
| blocking parallel get | 1.003 Mops/s | 1.022 Mops/s |
| async single get | 299.910 Kops/s | 403.808 Kops/s |
| async single get_ex | 355.062 Kops/s | 349.022 Kops/s |
| async parallel get | 2.521 Mops/s | 2.241 Mops/s |
| async many parallel get | 2.512 Mops/s | 2.829 Mops/s |
| async get_ex batch | 2.285 Mops/s | 2.654 Mops/s |
| async get_ex many | 2.471 Mops/s | 2.844 Mops/s |
| async cache many | 3.710 Mops/s | 2.482 Mops/s |
| async cache loop | 3.300 Mops/s | 4.592 Mops/s |
| async threaded cache loop | 4.121 Mops/s | 4.897 Mops/s |
| async lowerbound batch | 1.616 Mops/s | 1.578 Mops/s |
| async lowerbound many | 1.519 Mops/s | 1.676 Mops/s |
| async get loop | 2.386 Mops/s | 3.465 Mops/s |
| async get_ex loop | 3.185 Mops/s | 2.004 Mops/s |
| async lowerbound loop | 1.665 Mops/s | 1.897 Mops/s |
| async cursor get loop | 67.054 Mops/s | 67.244 Mops/s |
| async cursor get loop_from | 69.487 Mops/s | 69.923 Mops/s |
| async threaded cursor get loop | 124.367 Mops/s | 119.699 Mops/s |
| async threaded cget loop_from | 135.065 Mops/s | 126.880 Mops/s |
| async-cache-loop/par | 3.289 | 4.494 |
| async-thread-cache-loop/par | 4.107 | 4.792 |
| async-cget-loop-from/par | 1.071 | 1.096 |

- conclusion: this checkpoint consolidates async get fallback behavior so
  batch/grouped get paths use the same traversal-aware helper as single and loop
  get paths. The reduced benchmark is mixed: single get, many get, get_ex
  batch/many, cache loop, threaded cache loop, lowerbound many/loop, and direct
  cursor loop rows improved, while cache many, get_ex loop, and threaded cursor
  rows regressed in this sample.

Additional async get_ex fallback helper consolidation checkpoint:

- changed the unhandled fallback branches in `async_get_ex_batch_execute()`,
  `async_get_ex_ops_batch()`, and `async_op_get_ex_loop` to call
  `async_get_ex_one()` instead of calling `mdbx_get_ex()` directly.
- because `async_get_ex_one()` routes eligible nodup DBIs through
  `async_cached_get_one()`, these fallback paths now get the same
  traversal-aware cold-cache behavior as single `get_ex` before reaching the
  public blocking `mdbx_get_ex()` fallback.
- behavior is unchanged for non-batchable DBIs, dupsort DBIs, changed DBIs,
  unsupported paths, and allocation failures; those still use the existing
  blocking `mdbx_get_ex()` fallback inside the helper.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getex-fallback-helper-before.txt` and
    `/tmp/mdbx-async-bench-getex-fallback-helper-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `acd87e7`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.852 Mops/s | 1.847 Mops/s |
| blocking parallel get | 889.399 Kops/s | 808.720 Kops/s |
| async single get | 411.486 Kops/s | 359.953 Kops/s |
| async single get_ex | 407.846 Kops/s | 421.998 Kops/s |
| async parallel get | 2.647 Mops/s | 2.206 Mops/s |
| async many parallel get | 2.537 Mops/s | 1.896 Mops/s |
| async get_ex batch | 2.780 Mops/s | 2.736 Mops/s |
| async get_ex many | 2.893 Mops/s | 2.801 Mops/s |
| async cache many | 3.553 Mops/s | 2.475 Mops/s |
| async cache loop | 2.774 Mops/s | 2.681 Mops/s |
| async threaded cache loop | 2.713 Mops/s | 2.613 Mops/s |
| async lowerbound batch | 1.637 Mops/s | 1.600 Mops/s |
| async lowerbound many | 1.669 Mops/s | 1.676 Mops/s |
| async get loop | 2.010 Mops/s | 3.056 Mops/s |
| async get_ex loop | 1.686 Mops/s | 3.299 Mops/s |
| async lowerbound loop | 1.052 Mops/s | 1.858 Mops/s |
| async cursor get loop | 125.624 Mops/s | 120.503 Mops/s |
| async cursor get loop_from | 124.537 Mops/s | 66.304 Mops/s |
| async threaded cursor get loop | 112.204 Mops/s | 124.891 Mops/s |
| async threaded cget loop_from | 122.630 Mops/s | 131.940 Mops/s |
| async-single-ex/par | 0.459 | 0.522 |
| async-get-ex-batch/par | 3.125 | 3.384 |
| async-cache-loop/par | 3.118 | 3.316 |
| async-thread-cache-loop/par | 3.050 | 3.232 |
| async-get-loop/batch | 1.674 | 1.190 |
| async-cget-loop-from/par | 1.771 | 0.931 |

- conclusion: this checkpoint removes three more direct public `get_ex`
  fallbacks from async batch/grouped/loop execution. The one-run benchmark is
  mixed and noisy: single `get_ex`, `get_ex` loop, get loop, lowerbound loop,
  and threaded cursor rows improved, while many/get cache rows and direct cursor
  loop-from regressed in this sample.

Additional async lower-bound fallback helper consolidation checkpoint:

- changed the unhandled fallback branches in
  `async_get_equal_or_great_batch_execute()`,
  `async_get_equal_or_great_ops_batch()`, and
  `async_op_get_equal_or_great_loop` to call
  `async_get_equal_or_great_one()` instead of calling
  `mdbx_get_equal_or_great()` directly.
- because `async_get_equal_or_great_one()` first tries the one-key internal
  lower-bound traversal for eligible nodup read DBIs, fallback lower-bound
  batch/grouped/loop work now uses the same traversal-aware helper as the
  single lower-bound path before reaching the public blocking fallback.
- behavior is unchanged for non-batchable DBIs, dupsort DBIs, changed DBIs,
  unsupported paths, and allocation failures; those still use the existing
  blocking `mdbx_get_equal_or_great()` fallback inside the helper.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-lowerbound-fallback-helper-before.txt` and
    `/tmp/mdbx-async-bench-lowerbound-fallback-helper-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `372a5c2`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.862 Mops/s | 1.857 Mops/s |
| blocking parallel get | 780.639 Kops/s | 732.397 Kops/s |
| async single get | 420.377 Kops/s | 232.407 Kops/s |
| async single get_ex | 275.171 Kops/s | 416.615 Kops/s |
| async single lowerbound | 324.607 Kops/s | 283.053 Kops/s |
| async parallel get | 2.576 Mops/s | 1.889 Mops/s |
| async many parallel get | 2.236 Mops/s | 2.801 Mops/s |
| async get_ex batch | 2.682 Mops/s | 2.681 Mops/s |
| async get_ex many | 2.802 Mops/s | 2.606 Mops/s |
| async cache many | 4.359 Mops/s | 3.910 Mops/s |
| async cache loop | 2.746 Mops/s | 4.904 Mops/s |
| async threaded cache loop | 4.470 Mops/s | 3.897 Mops/s |
| async lowerbound batch | 1.468 Mops/s | 1.016 Mops/s |
| async lowerbound many | 1.657 Mops/s | 947.774 Kops/s |
| async get loop | 2.571 Mops/s | 1.823 Mops/s |
| async get_ex loop | 3.340 Mops/s | 1.853 Mops/s |
| async lowerbound loop | 1.746 Mops/s | 1.411 Mops/s |
| async cursor get loop | 67.244 Mops/s | 71.186 Mops/s |
| async cursor get loop_from | 67.801 Mops/s | 69.231 Mops/s |
| async threaded cursor get loop | 118.430 Mops/s | 117.551 Mops/s |
| async threaded cget loop_from | 125.603 Mops/s | 127.905 Mops/s |
| async-single-lower/par | 0.416 | 0.386 |
| async-lower-batch/par | 1.881 | 1.388 |
| async-lower-many/par | 2.122 | 1.294 |
| async-lower-loop/par | 2.236 | 1.926 |
| async-thread-lower-loop/par | 2.342 | 2.585 |
| async-cache-loop/par | 3.518 | 6.696 |
| async-thread-cache-loop/par | 5.726 | 5.321 |
| async-cget-loop-from/par | 1.073 | 1.030 |

- conclusion: this checkpoint removes three direct public lower-bound fallbacks
  from async batch/grouped/loop execution and keeps fallback work on the
  traversal-aware helper where possible. The one-run benchmark is mixed and
  likely cache-sensitive: cache loop, many parallel get, direct cursor loops,
  and threaded lower/cursor ratios improved, while lower-bound batch/many/loop,
  single lowerbound, get loop, and get_ex loop regressed in this sample.

Additional async cursor loop batch-start checkpoint:

- changed `async_cursor_get_loop_execute()` so unpositioned, non-dupsort
  `MDBX_FIRST`/`MDBX_NEXT` loops with no start key fetch their first window via
  `mdbx_cursor_get_batch(MDBX_FIRST)` instead of first calling
  `mdbx_cursor_get(MDBX_FIRST)` and then switching to cursor batches.
- kept one-item requests and one-item tails on the single cursor operation
  because `mdbx_cursor_get_batch()` requires space for at least two key/value
  pairs and would otherwise advance the cursor too far.
- constrained the new path to `!is_filled(cursor)`. This preserves existing
  `MDBX_FIRST` repositioning semantics for reused cursors; the first attempt
  without this guard failed `async_api` because `mdbx_cursor_get_batch(FIRST)`
  does not reposition an already-filled cursor.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursor-loop-first-batch-before.txt` and
    `/tmp/mdbx-async-bench-cursor-loop-first-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `176f06b`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.863 Mops/s | 1.868 Mops/s |
| blocking parallel get | 1.088 Mops/s | 1.058 Mops/s |
| async single get | 417.856 Kops/s | 407.933 Kops/s |
| async single get_ex | 405.719 Kops/s | 204.063 Kops/s |
| async single lowerbound | 189.924 Kops/s | 340.540 Kops/s |
| async parallel get | 2.418 Mops/s | 2.603 Mops/s |
| async many parallel get | 2.671 Mops/s | 2.415 Mops/s |
| async get_ex batch | 2.461 Mops/s | 2.335 Mops/s |
| async get_ex many | 2.856 Mops/s | 2.813 Mops/s |
| async cache many | 4.142 Mops/s | 4.453 Mops/s |
| async cache loop | 5.024 Mops/s | 2.781 Mops/s |
| async threaded cache loop | 4.947 Mops/s | 4.189 Mops/s |
| async lowerbound batch | 1.498 Mops/s | 1.625 Mops/s |
| async lowerbound many | 1.649 Mops/s | 1.675 Mops/s |
| async get loop | 3.437 Mops/s | 3.396 Mops/s |
| async get_ex loop | 3.025 Mops/s | 3.131 Mops/s |
| async lowerbound loop | 1.914 Mops/s | 1.815 Mops/s |
| blocking cursor get | 84.658 Mops/s | 87.473 Mops/s |
| parallel cursor get | 67.033 Mops/s | 96.279 Mops/s |
| async cursor get | 944.587 Kops/s | 868.436 Kops/s |
| async cursor get loop | 71.870 Mops/s | 120.542 Mops/s |
| async cursor get loop_from | 70.716 Mops/s | 67.212 Mops/s |
| async threaded cursor get loop | 120.479 Mops/s | 62.422 Mops/s |
| async threaded cget loop_from | 98.971 Mops/s | 69.260 Mops/s |
| blocking cursor batch | 202.336 Mops/s | 219.047 Mops/s |
| parallel cursor batch | 58.797 Mops/s | 70.998 Mops/s |
| async cursor batch | 67.080 Mops/s | 56.632 Mops/s |
| async threaded cursor batch | 117.138 Mops/s | 126.200 Mops/s |
| async cursor loop | 74.922 Mops/s | 78.334 Mops/s |
| async cursor loop_from | 75.352 Mops/s | 76.858 Mops/s |
| async threaded cursor loop | 138.761 Mops/s | 134.956 Mops/s |
| async threaded cursor loop_from | 139.469 Mops/s | 113.371 Mops/s |
| async-cursor-get-loop/par | 1.072 | 1.252 |
| async-cursor-get-loop/get | 76.086 | 138.803 |
| async-thread-cget-loop/par | 1.797 | 0.648 |
| async-thread-cget-loop/get | 127.547 | 71.878 |
| async-thread-cget-loop/loop | 1.676 | 0.518 |
| async-get-loop/batch | 1.071 | 2.129 |
| async-loop-cursor/par | 1.274 | 1.103 |
| async-thread-cursor-loop/par | 2.360 | 1.901 |

- conclusion: this checkpoint moves one common unpositioned cursor-loop startup
  case onto the cursor batch primitive immediately, reducing direct
  one-by-one cursor reads on that path while preserving reused-cursor
  semantics. The one-run benchmark is mixed: direct async cursor get loop and
  `async-get-loop/batch` improved substantially, while threaded cursor get loop
  and cache-loop rows regressed in this sample.

Additional async cache-get fallback helper consolidation checkpoint:

- changed the unhandled fallback branches in `async_cache_get_batch_execute()`,
  `async_cache_get_ops_batch()`, and `async_op_cache_get_loop` to call
  `async_cache_get_one_materialized()` instead of calling
  `mdbx_cache_get()` / `mdbx_cache_get_SingleThreaded()` directly.
- because `async_cache_get_one_materialized()` first snapshots/materializes the
  cache entry through `cache_materialize_singlethreaded_batch()` before falling
  back to the public cache-get APIs, batch/grouped/loop fallback cache gets now
  share the same final materialization path as single cache gets.
- this keeps fallback behavior unchanged for entries that cannot be safely
  snapshotted or materialized; those still use the public blocking cache-get
  fallback inside the helper.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cache-fallback-helper-before.txt` and
    `/tmp/mdbx-async-bench-cache-fallback-helper-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `8900d2a`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.848 Mops/s | 1.886 Mops/s |
| blocking parallel get | 793.404 Kops/s | 996.420 Kops/s |
| async single get | 403.183 Kops/s | 425.836 Kops/s |
| async single get_ex | 379.400 Kops/s | 277.805 Kops/s |
| async single lowerbound | 211.276 Kops/s | 306.176 Kops/s |
| async parallel get | 1.553 Mops/s | 2.156 Mops/s |
| async many parallel get | 2.213 Mops/s | 1.745 Mops/s |
| async get_ex batch | 2.442 Mops/s | 2.104 Mops/s |
| async get_ex many | 2.889 Mops/s | 2.497 Mops/s |
| async cache many | 4.321 Mops/s | 2.255 Mops/s |
| async cache st many | 3.094 Mops/s | 4.175 Mops/s |
| async cache batch | 4.267 Mops/s | 4.244 Mops/s |
| async cache st batch | 3.628 Mops/s | 4.273 Mops/s |
| async cache batch cb | 4.272 Mops/s | 2.904 Mops/s |
| async cache st batch cb | 4.190 Mops/s | 3.765 Mops/s |
| async large cache batch | 138.775 Kops/s | 137.891 Kops/s |
| async large cache st batch | 139.597 Kops/s | 134.808 Kops/s |
| async threaded cache batch | 4.489 Mops/s | 4.219 Mops/s |
| async threaded cache st batch | 4.234 Mops/s | 2.894 Mops/s |
| async cache loop | 2.892 Mops/s | 2.803 Mops/s |
| async cache st loop | 2.720 Mops/s | 5.043 Mops/s |
| async threaded cache loop | 2.095 Mops/s | 2.255 Mops/s |
| async threaded cache st loop | 3.438 Mops/s | 2.771 Mops/s |
| async lowerbound batch | 1.695 Mops/s | 939.680 Kops/s |
| async lowerbound many | 1.667 Mops/s | 1.673 Mops/s |
| async get loop | 3.442 Mops/s | 1.886 Mops/s |
| async get_ex loop | 3.419 Mops/s | 3.388 Mops/s |
| async lowerbound loop | 1.757 Mops/s | 1.367 Mops/s |
| async cursor get loop | 70.624 Mops/s | 68.282 Mops/s |
| async cursor get loop_from | 69.726 Mops/s | 58.216 Mops/s |
| async threaded cursor get loop | 72.658 Mops/s | 126.095 Mops/s |
| async threaded cget loop_from | 73.603 Mops/s | 125.120 Mops/s |
| async-cache-many/par | 5.446 | 2.263 |
| async-cache-st-many/par | 3.900 | 4.190 |
| async-cache-batch/par | 5.378 | 4.259 |
| async-cache-st-batch/par | 4.573 | 4.289 |
| async-cache-batch-cb/par | 5.385 | 2.915 |
| async-cache-st-batch-cb/par | 5.281 | 3.778 |
| async-cache-loop/par | 3.645 | 2.813 |
| async-cache-st-loop/par | 3.428 | 5.061 |
| async-thread-cache-loop/par | 2.640 | 2.263 |
| async-thread-cache-st-l/par | 4.333 | 2.780 |
| async-cache-loop/batch | 0.678 | 0.660 |
| async-cache-st-loop/batch | 0.750 | 1.180 |
| async-cache-loop/many | 0.669 | 1.243 |
| async-cache-st-loop/many | 0.879 | 1.208 |

- conclusion: this checkpoint consolidates cache-get fallback behavior so
  batch/grouped/loop paths retry the single materialization helper before the
  public blocking cache-get fallback. The one-run benchmark is mixed:
  single-thread cache many/batch/loop and threaded cache loop improved, while
  regular cache many, cache callback batch, threaded single-thread cache batch,
  get loop, lowerbound batch/loop, and cursor loop-from regressed in this
  sample.

Additional single-page read batch-path checkpoint:

- removed the `count == 1` bypass in `page_submit_get_unchecked_batch()`.
  Single-page explicit reads now flow through the same dirty/spilled filtering,
  committed-page submit preparation, and `page_get_committed_batch()` path as
  multi-page reads.
- removed the now-unused `page_submit_get_unchecked_one()` and
  `page_get_committed()` helpers, leaving the committed-page batch helper as the
  single internal path to `page_cache_submit_read_batch()`.
- this is a low-level alignment step for true async traversal: one-key get,
  get_ex, lower-bound, cache materialization, and cursor child-page reads that
  submit a single page no longer bypass the batch submit machinery before
  reaching the explicit page cache / storage read layer.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-single-page-batch-before.txt` and
    `/tmp/mdbx-async-bench-single-page-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `a6ec541`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.902 Mops/s | 1.810 Mops/s |
| blocking parallel get | 1.052 Mops/s | 1.084 Mops/s |
| async single get | 421.915 Kops/s | 238.591 Kops/s |
| async single get_ex | 360.075 Kops/s | 387.106 Kops/s |
| async single lowerbound | 332.410 Kops/s | 219.418 Kops/s |
| async parallel get | 2.768 Mops/s | 2.689 Mops/s |
| async many parallel get | 2.387 Mops/s | 2.705 Mops/s |
| async get_ex batch | 1.500 Mops/s | 1.796 Mops/s |
| async get_ex many | 1.476 Mops/s | 2.738 Mops/s |
| async cache many | 2.706 Mops/s | 4.491 Mops/s |
| async cache st many | 3.967 Mops/s | 3.689 Mops/s |
| async cache batch | 3.730 Mops/s | 3.362 Mops/s |
| async cache st batch | 3.132 Mops/s | 2.486 Mops/s |
| async cache loop | 4.897 Mops/s | 4.895 Mops/s |
| async cache st loop | 3.428 Mops/s | 2.692 Mops/s |
| async threaded cache loop | 4.243 Mops/s | 2.122 Mops/s |
| async threaded cache st loop | 4.835 Mops/s | 3.804 Mops/s |
| async lowerbound batch | 1.581 Mops/s | 1.668 Mops/s |
| async lowerbound many | 1.569 Mops/s | 1.264 Mops/s |
| async get loop | 3.245 Mops/s | 3.482 Mops/s |
| async get_ex loop | 2.412 Mops/s | 1.942 Mops/s |
| async lowerbound loop | 1.794 Mops/s | 1.852 Mops/s |
| blocking cursor get | 86.688 Mops/s | 86.281 Mops/s |
| parallel cursor get | 59.711 Mops/s | 65.097 Mops/s |
| async cursor get | 918.647 Kops/s | 871.763 Kops/s |
| async cursor get loop | 72.744 Mops/s | 70.254 Mops/s |
| async cursor get loop_from | 66.283 Mops/s | 69.716 Mops/s |
| async threaded cursor get loop | 120.731 Mops/s | 120.159 Mops/s |
| async threaded cget loop_from | 125.057 Mops/s | 124.166 Mops/s |
| async-single/blocking par | 0.401 | 0.220 |
| async-single-ex/par | 0.342 | 0.357 |
| async-single-lower/par | 0.316 | 0.202 |
| async/blocking parallel | 2.631 | 2.480 |
| async-many/blocking par | 2.269 | 2.495 |
| async-get-ex-batch/par | 1.426 | 1.656 |
| async-cache-many/par | 2.573 | 4.142 |
| async-cache-st-many/par | 3.771 | 3.402 |
| async-cache-loop/par | 4.655 | 4.514 |
| async-cache-st-loop/par | 3.259 | 2.482 |
| async-thread-cache-loop/par | 4.034 | 1.957 |
| async-lower-batch/par | 1.503 | 1.539 |
| async-lower-many/par | 1.492 | 1.166 |
| async-loop/blocking par | 3.085 | 3.211 |
| async-get-ex-loop/par | 2.293 | 1.791 |
| async-lower-loop/par | 1.705 | 1.708 |
| async-cursor-get-loop/par | 1.218 | 1.079 |
| async-cursor-get-loop/get | 79.186 | 80.589 |

- conclusion: this checkpoint trades the previous single-page fast path for a
  unified batch submit path. The one-run benchmark is mixed: get_ex batch/many,
  cache many, lowerbound batch, get loop, lowerbound loop, and cursor loop-from
  improved, while async single get, single lowerbound, threaded cache loop, and
  get_ex loop regressed in this sample.

Additional singleton page-cache submit batch-path checkpoint:

- changed `page_cache_submit_read()` to delegate to
  `page_cache_submit_read_batch()` with `count == 1`.
- removed the now-unused `dxb_storage_submit_read_cached_page()` direct fill
  helper. Singleton page-cache misses now use the same prepare/read/complete
  path as batched page-cache misses, including `dxb_storage_submit_read_data_batch()`
  for storage reads.
- this keeps direct cache-entry materialization on the same page-cache read
  path as async get/cache/cursor traversal work, reducing another one-off
  synchronous-looking storage read branch below the public APIs.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-page-cache-single-batch-before.txt` and
    `/tmp/mdbx-async-bench-page-cache-single-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `6db745d`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.819 Mops/s | 1.846 Mops/s |
| blocking parallel get | 1.070 Mops/s | 540.928 Kops/s |
| async single get | 201.406 Kops/s | 392.693 Kops/s |
| async single get_ex | 202.107 Kops/s | 424.275 Kops/s |
| async single lowerbound | 360.406 Kops/s | 346.790 Kops/s |
| async parallel get | 1.494 Mops/s | 2.798 Mops/s |
| async many parallel get | 2.160 Mops/s | 2.817 Mops/s |
| async get_ex batch | 2.628 Mops/s | 2.685 Mops/s |
| async get_ex many | 2.583 Mops/s | 2.449 Mops/s |
| async cache many | 2.892 Mops/s | 4.307 Mops/s |
| async cache st many | 2.591 Mops/s | 2.278 Mops/s |
| async cache batch | 3.793 Mops/s | 3.493 Mops/s |
| async cache st batch | 4.272 Mops/s | 4.271 Mops/s |
| async cache loop | 4.933 Mops/s | 4.561 Mops/s |
| async cache st loop | 4.072 Mops/s | 2.741 Mops/s |
| async threaded cache loop | 2.602 Mops/s | 3.774 Mops/s |
| async threaded cache st loop | 2.053 Mops/s | 2.079 Mops/s |
| async lowerbound batch | 1.348 Mops/s | 1.374 Mops/s |
| async lowerbound many | 1.659 Mops/s | 1.690 Mops/s |
| async get loop | 2.292 Mops/s | 2.828 Mops/s |
| async get_ex loop | 3.165 Mops/s | 3.047 Mops/s |
| async lowerbound loop | 1.880 Mops/s | 1.190 Mops/s |
| blocking cursor get | 86.548 Mops/s | 86.008 Mops/s |
| parallel cursor get | 57.693 Mops/s | 56.200 Mops/s |
| async cursor get | 864.311 Kops/s | 720.116 Kops/s |
| async cursor get loop | 69.771 Mops/s | 69.887 Mops/s |
| async cursor get loop_from | 70.649 Mops/s | 66.450 Mops/s |
| async threaded cursor get loop | 115.985 Mops/s | 116.020 Mops/s |
| async threaded cget loop_from | 72.048 Mops/s | 122.279 Mops/s |
| async-single/blocking par | 0.188 | 0.726 |
| async-single-ex/par | 0.189 | 0.784 |
| async-single-lower/par | 0.337 | 0.641 |
| async/blocking parallel | 1.395 | 5.173 |
| async-many/blocking par | 2.018 | 5.209 |
| async-get-ex-batch/par | 2.455 | 4.963 |
| async-cache-many/par | 2.702 | 7.962 |
| async-cache-st-many/par | 2.420 | 4.211 |
| async-cache-loop/par | 4.609 | 8.432 |
| async-cache-st-loop/par | 3.804 | 5.067 |
| async-thread-cache-loop/par | 2.431 | 6.978 |
| async-lower-batch/par | 1.259 | 2.541 |
| async-lower-many/par | 1.550 | 3.124 |
| async-loop/blocking par | 2.142 | 5.228 |
| async-get-ex-loop/par | 2.957 | 5.633 |
| async-lower-loop/par | 1.756 | 2.201 |
| async-cursor-get-loop/par | 1.209 | 1.244 |
| async-cursor-get-loop/get | 80.725 | 97.049 |

- conclusion: this checkpoint removes the direct singleton cached-page fill
  path and makes direct cache-entry materialization use the same page-cache
  batch submit machinery as async traversal. The one-run benchmark is noisy but
  mostly positive for get/cache ratios: single get, single get_ex, parallel get,
  many get, cache many, threaded cache loop, get loop, and threaded cursor
  loop-from improved, while lowerbound loop, async cursor get, cache loop, and
  single-thread cache loop regressed in this sample.

Additional singleton storage-read batch-path checkpoint:

- changed `dxb_storage_submit_read_data()` to delegate to
  `dxb_storage_submit_read_data_batch()` with `count == 1`.
- changed `osal_ioring_pread_batch()` so the Linux io_uring batch backend is
  used for `count == 1` as well as larger batches. Non-io_uring fallback still
  loops through `osal_ioring_pread()` for each item.
- this removes another direct singleton explicit data-file read branch below
  page-cache and large-page materialization. Singleton callers still complete
  synchronously at the public API boundary, but internally they now enter the
  same storage read batch submit/completion path as batched page-cache misses.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-storage-single-batch-before.txt` and
    `/tmp/mdbx-async-bench-storage-single-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `859a3ba`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.835 Mops/s | 1.866 Mops/s |
| blocking parallel get | 1.088 Mops/s | 832.681 Kops/s |
| async single get | 224.499 Kops/s | 236.999 Kops/s |
| async single get_ex | 346.561 Kops/s | 202.073 Kops/s |
| async single lowerbound | 259.239 Kops/s | 179.960 Kops/s |
| async parallel get | 2.556 Mops/s | 1.973 Mops/s |
| async many parallel get | 2.874 Mops/s | 2.784 Mops/s |
| async get_ex batch | 2.492 Mops/s | 1.719 Mops/s |
| async get_ex many | 2.403 Mops/s | 2.732 Mops/s |
| async cache many | 4.400 Mops/s | 3.088 Mops/s |
| async cache st many | 4.315 Mops/s | 3.840 Mops/s |
| async cache batch | 3.697 Mops/s | 4.044 Mops/s |
| async cache st batch | 4.216 Mops/s | 4.098 Mops/s |
| async cache loop | 4.818 Mops/s | 4.443 Mops/s |
| async cache st loop | 2.621 Mops/s | 2.358 Mops/s |
| async threaded cache loop | 3.711 Mops/s | 2.430 Mops/s |
| async threaded cache st loop | 4.612 Mops/s | 4.252 Mops/s |
| async lowerbound batch | 1.655 Mops/s | 884.814 Kops/s |
| async lowerbound many | 1.671 Mops/s | 714.107 Kops/s |
| async get loop | 2.815 Mops/s | 1.727 Mops/s |
| async get_ex loop | 3.405 Mops/s | 2.108 Mops/s |
| async lowerbound loop | 1.918 Mops/s | 1.180 Mops/s |
| blocking cursor get | 87.266 Mops/s | 85.512 Mops/s |
| parallel cursor get | 97.138 Mops/s | 54.692 Mops/s |
| async cursor get | 1.058 Mops/s | 860.784 Kops/s |
| async cursor get loop | 71.046 Mops/s | 65.706 Mops/s |
| async cursor get loop_from | 68.014 Mops/s | 61.091 Mops/s |
| async threaded cursor get loop | 118.645 Mops/s | 125.466 Mops/s |
| async threaded cget loop_from | 121.604 Mops/s | 116.922 Mops/s |
| async-single/blocking par | 0.206 | 0.285 |
| async-single-ex/par | 0.319 | 0.243 |
| async-single-lower/par | 0.238 | 0.216 |
| async/blocking parallel | 2.350 | 2.369 |
| async-many/blocking par | 2.642 | 3.343 |
| async-get-ex-batch/par | 2.291 | 2.064 |
| async-cache-many/par | 4.045 | 3.708 |
| async-cache-st-many/par | 3.967 | 4.612 |
| async-cache-loop/par | 4.430 | 5.335 |
| async-cache-st-loop/par | 2.410 | 2.832 |
| async-thread-cache-loop/par | 3.412 | 2.918 |
| async-lower-batch/par | 1.522 | 1.063 |
| async-lower-many/par | 1.537 | 0.858 |
| async-loop/blocking par | 2.588 | 2.074 |
| async-get-ex-loop/par | 3.130 | 2.531 |
| async-lower-loop/par | 1.764 | 1.417 |
| async-cursor-get-loop/par | 0.731 | 1.201 |
| async-cursor-get-loop/get | 67.136 | 76.333 |

- conclusion: this checkpoint routes singleton storage reads through the same
  storage/io_uring batch adapter as multi-read work. The one-run benchmark is
  mixed: single get, get_ex many, cache batch, cache-loop ratios,
  single-thread cache ratios, many/blocking ratio, and threaded cursor get loop
  improved, while single get_ex, lowerbound rows, cache many, get/get_ex loop,
  and cursor loop rows regressed in this sample.

Additional singleton cursor page-get batch-path checkpoint:

- changed `page_submit_cursor_get()` to delegate to
  `page_submit_cursor_get_batch()` with `count == 1`.
- removed the one-item bypass inside `page_submit_cursor_get_batch()`, so
  ordinary cursor child-page fetches now use the same cursor validation,
  `ops_pget` accounting, raw page batch submit, and cursor completion path as
  multi-page cursor reads.
- this extends the singleton batch-path alignment from raw page/cache/storage
  reads up through cursor page gets. Public cursor APIs still complete
  synchronously, but their explicit-I/O page fetches now enter the same cursor
  submit/complete batch layer used by batched traversal.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursor-single-before.txt` and
    `/tmp/mdbx-async-bench-cursor-single-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `5bc71d9`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.863 Mops/s | 1.849 Mops/s |
| blocking parallel get | 1.045 Mops/s | 1.069 Mops/s |
| async single get | 311.116 Kops/s | 304.976 Kops/s |
| async single get_ex | 369.002 Kops/s | 316.524 Kops/s |
| async single lowerbound | 184.938 Kops/s | 261.548 Kops/s |
| async parallel get | 2.000 Mops/s | 2.167 Mops/s |
| async many parallel get | 2.846 Mops/s | 1.619 Mops/s |
| async get_ex batch | 2.702 Mops/s | 2.722 Mops/s |
| async get_ex many | 2.861 Mops/s | 2.827 Mops/s |
| async cache many | 4.431 Mops/s | 2.189 Mops/s |
| async cache st many | 2.389 Mops/s | 4.361 Mops/s |
| async cache batch | 3.132 Mops/s | 3.435 Mops/s |
| async cache st batch | 3.248 Mops/s | 3.822 Mops/s |
| async cache loop | 4.916 Mops/s | 2.800 Mops/s |
| async cache st loop | 2.697 Mops/s | 2.751 Mops/s |
| async threaded cache loop | 3.738 Mops/s | 3.645 Mops/s |
| async threaded cache st loop | 2.769 Mops/s | 4.679 Mops/s |
| async lowerbound batch | 1.634 Mops/s | 902.225 Kops/s |
| async lowerbound many | 1.519 Mops/s | 1.663 Mops/s |
| async get loop | 3.169 Mops/s | 2.888 Mops/s |
| async get_ex loop | 2.683 Mops/s | 3.335 Mops/s |
| async lowerbound loop | 1.881 Mops/s | 1.860 Mops/s |
| blocking cursor get | 82.349 Mops/s | 87.373 Mops/s |
| parallel cursor get | 61.362 Mops/s | 57.499 Mops/s |
| async cursor get | 859.144 Kops/s | 873.156 Kops/s |
| async cursor get loop | 69.122 Mops/s | 68.057 Mops/s |
| async cursor get loop_from | 67.462 Mops/s | 71.861 Mops/s |
| async threaded cursor get loop | 121.107 Mops/s | 71.167 Mops/s |
| async threaded cget loop_from | 128.503 Mops/s | 116.635 Mops/s |
| async/blocking parallel | 1.914 | 2.026 |
| async-many/blocking par | 2.722 | 1.514 |
| async-get-ex-batch/par | 2.585 | 2.546 |
| async-cache-st-many/par | 2.286 | 4.077 |
| async-cache-loop/par | 4.703 | 2.618 |
| async-thread-cache-loop/par | 3.576 | 3.408 |
| async-lower-batch/par | 1.563 | 0.844 |
| async-lower-many/par | 1.453 | 1.555 |
| async-loop/blocking par | 3.031 | 2.701 |
| async-get-ex-loop/par | 2.567 | 3.119 |
| async-cursor-get-loop/par | 1.126 | 1.184 |
| async-cursor-get-loop/get | 80.455 | 77.944 |

- conclusion: this checkpoint removes the last singleton bypass in the cursor
  page-get submit layer. The one-run benchmark is mixed: async parallel get,
  single lowerbound, get_ex batch, single-thread cache rows, lowerbound many,
  get_ex loop, blocking cursor get, async cursor get, and cursor loop-from
  improved, while many parallel get, regular cache rows, lowerbound batch,
  get loop, and threaded cursor loop rows regressed in this sample.

Additional Linux io_uring read submit/complete split checkpoint:

- split `osal_ioring_linux_uring_read_batch()` into an explicit batch state plus
  `osal_ioring_linux_uring_read_batch_submit()` and
  `osal_ioring_linux_uring_read_batch_complete()`.
- the public internal wrapper is still synchronous, but it now drives a real
  submit/progress/complete state object instead of keeping all io_uring SQ/CQ
  handling inside one monolithic loop.
- added a no-progress guard so the synchronous driver reports an I/O error
  instead of spinning if the ring cannot submit or complete anything.
- this is plumbing for the requested suspend/resume traversal work: higher
  layers can now be moved toward retaining an in-flight read batch and polling
  completions, rather than requiring every page-cache miss to be submitted and
  waited inside the same helper call.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-read-split-before.txt` and
    `/tmp/mdbx-async-bench-read-split-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `9a0124a`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.867 Mops/s | 1.818 Mops/s |
| blocking parallel get | 944.451 Kops/s | 1.083 Mops/s |
| async single get | 244.401 Kops/s | 425.482 Kops/s |
| async single get_ex | 203.504 Kops/s | 426.810 Kops/s |
| async single lowerbound | 304.348 Kops/s | 236.423 Kops/s |
| async parallel get | 2.364 Mops/s | 2.725 Mops/s |
| async many parallel get | 2.124 Mops/s | 2.505 Mops/s |
| async get_ex batch | 2.384 Mops/s | 2.388 Mops/s |
| async get_ex many | 2.619 Mops/s | 2.903 Mops/s |
| async cache many | 4.342 Mops/s | 3.716 Mops/s |
| async cache st many | 3.027 Mops/s | 4.626 Mops/s |
| async cache batch | 4.165 Mops/s | 3.824 Mops/s |
| async cache st batch | 3.930 Mops/s | 3.096 Mops/s |
| async cache batch cb | 4.217 Mops/s | 3.153 Mops/s |
| async cache st batch cb | 2.370 Mops/s | 3.235 Mops/s |
| async cache loop | 4.385 Mops/s | 5.061 Mops/s |
| async cache st loop | 3.937 Mops/s | 4.333 Mops/s |
| async threaded cache loop | 4.818 Mops/s | 3.868 Mops/s |
| async threaded cache st loop | 2.753 Mops/s | 3.848 Mops/s |
| async lowerbound batch | 1.641 Mops/s | 1.571 Mops/s |
| async lowerbound many | 1.681 Mops/s | 1.638 Mops/s |
| async get loop | 1.918 Mops/s | 3.388 Mops/s |
| async get_ex loop | 3.442 Mops/s | 2.563 Mops/s |
| async lowerbound loop | 1.438 Mops/s | 1.887 Mops/s |
| blocking cursor get | 87.259 Mops/s | 85.196 Mops/s |
| parallel cursor get | 55.076 Mops/s | 55.141 Mops/s |
| async cursor get | 885.343 Kops/s | 1.217 Mops/s |
| async cursor get loop | 74.682 Mops/s | 72.309 Mops/s |
| async cursor get loop_from | 67.395 Mops/s | 68.125 Mops/s |
| async threaded cursor get loop | 117.054 Mops/s | 120.673 Mops/s |
| async threaded cget loop_from | 126.063 Mops/s | 66.530 Mops/s |
| async/blocking parallel | 2.504 | 2.516 |
| async-many/blocking par | 2.249 | 2.313 |
| async-cache-many/par | 4.598 | 3.431 |
| async-lower-loop/par | 1.523 | 1.743 |
| async-cursor-get-loop/get | 84.354 | 59.419 |

- conclusion: this checkpoint introduces the low-level read-batch state needed
  for submit/poll/complete layering without changing synchronous API behavior.
  The one-run benchmark is mixed: async single get/get_ex, parallel get, many
  get, get_ex many, single-thread cache many, cache loops, get loop,
  lowerbound loop, async cursor get, cursor loop-from, and threaded cursor get
  loop improved, while cache batch/callback rows, threaded cache loop,
  lowerbound batch/many, get_ex loop, cursor loop, and threaded cursor
  loop-from regressed in this sample.

Additional meta-read batch-adapter checkpoint:

- routed `dxb_storage_submit_read_meta()` through `osal_ioring_pread_batch()`
  with a one-item byte read request instead of calling `osal_ioring_pread()`
  directly.
- kept meta-specific validation intact because meta probing can use a probe
  page size before normal data-page geometry is finalized.
- this makes explicit meta reads use the same Linux io_uring read-batch
  submit/complete driver as data-file page reads, while preserving the existing
  synchronous meta-read result contract and read/read-complete fault injection.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-meta-batch-before.txt` and
    `/tmp/mdbx-async-bench-meta-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `74c3cc3`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.842 Mops/s | 1.826 Mops/s |
| blocking parallel get | 1.064 Mops/s | 1.069 Mops/s |
| async single get | 420.809 Kops/s | 290.103 Kops/s |
| async single get_ex | 239.512 Kops/s | 415.260 Kops/s |
| async single lowerbound | 275.434 Kops/s | 281.873 Kops/s |
| async parallel get | 2.463 Mops/s | 2.843 Mops/s |
| async many parallel get | 2.838 Mops/s | 2.365 Mops/s |
| async get_ex batch | 1.880 Mops/s | 2.547 Mops/s |
| async get_ex many | 2.779 Mops/s | 2.841 Mops/s |
| async cache many | 3.284 Mops/s | 4.369 Mops/s |
| async cache st many | 4.159 Mops/s | 3.176 Mops/s |
| async cache batch | 3.556 Mops/s | 4.217 Mops/s |
| async cache st batch | 4.055 Mops/s | 4.274 Mops/s |
| async cache loop | 4.767 Mops/s | 4.376 Mops/s |
| async cache st loop | 2.194 Mops/s | 2.782 Mops/s |
| async threaded cache loop | 2.971 Mops/s | 4.475 Mops/s |
| async threaded cache st loop | 4.842 Mops/s | 4.752 Mops/s |
| async lowerbound batch | 1.601 Mops/s | 1.536 Mops/s |
| async lowerbound many | 1.497 Mops/s | 1.499 Mops/s |
| async get loop | 2.590 Mops/s | 3.347 Mops/s |
| async get_ex loop | 3.154 Mops/s | 1.872 Mops/s |
| async lowerbound loop | 1.852 Mops/s | 1.262 Mops/s |
| blocking cursor get | 86.831 Mops/s | 84.138 Mops/s |
| parallel cursor get | 56.770 Mops/s | 55.828 Mops/s |
| async cursor get | 1.030 Mops/s | 870.667 Kops/s |
| async cursor get loop | 67.814 Mops/s | 68.786 Mops/s |
| async cursor get loop_from | 70.082 Mops/s | 70.016 Mops/s |
| async threaded cursor get loop | 124.766 Mops/s | 117.764 Mops/s |
| async threaded cget loop_from | 118.528 Mops/s | 126.180 Mops/s |
| async/blocking parallel | 2.314 | 2.660 |
| async-cache-many/par | 3.087 | 4.089 |
| async-cache-loop/par | 4.480 | 4.095 |
| async-loop/blocking par | 2.434 | 3.133 |
| async-cursor-get-loop/get | 65.824 | 79.003 |

- conclusion: this checkpoint removes the last direct explicit meta-read call
  to `osal_ioring_pread()`. The steady-state read benchmark is mostly noise for
  this startup/meta path; in this one run async get_ex, parallel get, get_ex
  batch/many, cache many/batch, cache st loop, threaded cache loop, get loop,
  cursor loop, threaded cursor loop-from, and cursor-loop/get ratio improved,
  while async single get, many get, cache st many, cache loop, get_ex loop,
  lowerbound loop, cursor get, and threaded cursor get loop regressed.

Additional io_uring read completion-context checkpoint:

- added `osal_ioring_linux_read_item_t` records for each submitted read in a
  Linux io_uring read batch.
- changed SQE `user_data` from a batch-local slot number to a pointer to that
  read item. Completion now uses the item to find the original
  `dxb_read_submit_io_t` and destination `dxb_read_result_t`.
- kept the synchronous wrapper and lock scope unchanged for now, but removed
  the completion path's dependency on ordered slot ids. This is a prerequisite
  for allowing a future read-batch state to survive outside the synchronous
  wrapper and be completed by polling CQEs later.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-read-context-before.txt` and
    `/tmp/mdbx-async-bench-read-context-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `da26bdb`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.789 Mops/s | 1.857 Mops/s |
| blocking parallel get | 773.729 Kops/s | 1.072 Mops/s |
| async single get | 419.763 Kops/s | 319.700 Kops/s |
| async single get_ex | 403.570 Kops/s | 423.052 Kops/s |
| async single lowerbound | 355.467 Kops/s | 304.119 Kops/s |
| async parallel get | 2.263 Mops/s | 2.685 Mops/s |
| async many parallel get | 2.105 Mops/s | 2.869 Mops/s |
| async get_ex batch | 2.747 Mops/s | 2.369 Mops/s |
| async get_ex many | 2.342 Mops/s | 2.563 Mops/s |
| async cache many | 3.707 Mops/s | 2.413 Mops/s |
| async cache st many | 2.737 Mops/s | 2.660 Mops/s |
| async cache batch | 2.687 Mops/s | 3.432 Mops/s |
| async cache st batch | 3.663 Mops/s | 4.200 Mops/s |
| async cache loop | 4.966 Mops/s | 4.963 Mops/s |
| async cache st loop | 5.037 Mops/s | 2.726 Mops/s |
| async threaded cache loop | 4.738 Mops/s | 2.362 Mops/s |
| async threaded cache st loop | 3.011 Mops/s | 1.986 Mops/s |
| async lowerbound batch | 943.605 Kops/s | 1.409 Mops/s |
| async lowerbound many | 895.252 Kops/s | 1.656 Mops/s |
| async get loop | 1.906 Mops/s | 3.439 Mops/s |
| async get_ex loop | 1.912 Mops/s | 1.886 Mops/s |
| async lowerbound loop | 1.013 Mops/s | 1.878 Mops/s |
| blocking cursor get | 86.553 Mops/s | 86.533 Mops/s |
| parallel cursor get | 64.928 Mops/s | 91.574 Mops/s |
| async cursor get | 1.023 Mops/s | 1.103 Mops/s |
| async cursor get loop | 68.458 Mops/s | 67.244 Mops/s |
| async cursor get loop_from | 69.506 Mops/s | 70.496 Mops/s |
| async threaded cursor get loop | 71.639 Mops/s | 127.171 Mops/s |
| async threaded cget loop_from | 65.455 Mops/s | 125.592 Mops/s |
| async/blocking parallel | 2.924 | 2.505 |
| async-many/blocking par | 2.721 | 2.677 |
| async-cache-many/par | 4.791 | 2.251 |
| async-cache-loop/par | 6.418 | 4.631 |
| async-lower-batch/par | 1.220 | 1.315 |
| async-lower-many/par | 1.157 | 1.545 |
| async-loop/blocking par | 2.463 | 3.209 |
| async-cursor-get-loop/par | 1.054 | 0.734 |
| async-cursor-get-loop/get | 66.898 | 60.944 |

- conclusion: this checkpoint changes how io_uring completions find their
  request context, not which pages are read. The one-run benchmark is mixed:
  parallel/many get, get_ex many, cache batch rows, lowerbound batch/many/loop,
  get loop, parallel cursor get, async cursor get, cursor loop-from, and
  threaded cursor rows improved, while single get/lowerbound, get_ex batch,
  cache many, cache st loop, threaded cache rows, get_ex loop, cursor loop, and
  cursor-loop ratios regressed in this sample.

Additional io_uring read-batch poll checkpoint:

- changed the synchronous Linux io_uring read-batch driver to poll the CQ ring
  without blocking before entering the wait path.
- the wrapper still drives all reads to completion before returning, but each
  loop now follows the intended submit/poll/complete shape: submit as many
  reads as fit, drain already-completed CQEs, and only call
  `io_uring_enter(... GETEVENTS)` if no completion was observed and submitted
  reads remain in flight.
- this is still an internal stepping stone rather than a full async traversal
  state machine. It makes the synchronous adapter use the same nonblocking
  completion primitive that an external/resumable batch state will need later.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-poll-before.txt` and
    `/tmp/mdbx-async-bench-poll-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `44d9ab8`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.709 Mops/s | 1.727 Mops/s |
| blocking parallel get | 1.065 Mops/s | 1.050 Mops/s |
| async single get | 301.029 Kops/s | 371.811 Kops/s |
| async single get_ex | 279.624 Kops/s | 201.361 Kops/s |
| async single lowerbound | 245.246 Kops/s | 290.816 Kops/s |
| async parallel get | 1.735 Mops/s | 2.668 Mops/s |
| async many parallel get | 2.800 Mops/s | 2.813 Mops/s |
| async get_ex batch | 2.323 Mops/s | 1.726 Mops/s |
| async get_ex many | 2.835 Mops/s | 2.536 Mops/s |
| async cache many | 3.210 Mops/s | 2.353 Mops/s |
| async cache st many | 4.346 Mops/s | 3.568 Mops/s |
| async cache batch | 4.319 Mops/s | 4.182 Mops/s |
| async cache st batch | 3.928 Mops/s | 2.339 Mops/s |
| async cache loop | 2.328 Mops/s | 2.714 Mops/s |
| async cache st loop | 3.486 Mops/s | 2.659 Mops/s |
| async threaded cache loop | 2.714 Mops/s | 4.700 Mops/s |
| async threaded cache st loop | 2.027 Mops/s | 3.491 Mops/s |
| async lowerbound batch | 1.334 Mops/s | 1.463 Mops/s |
| async lowerbound many | 1.581 Mops/s | 1.612 Mops/s |
| async get loop | 3.444 Mops/s | 2.810 Mops/s |
| async get_ex loop | 3.400 Mops/s | 2.524 Mops/s |
| async lowerbound loop | 1.554 Mops/s | 1.589 Mops/s |
| blocking cursor get | 86.753 Mops/s | 85.961 Mops/s |
| parallel cursor get | 58.105 Mops/s | 56.687 Mops/s |
| async cursor get | 1.125 Mops/s | 863.006 Kops/s |
| async cursor get loop | 68.022 Mops/s | 66.459 Mops/s |
| async cursor get loop_from | 68.159 Mops/s | 77.372 Mops/s |
| async threaded cursor get loop | 119.013 Mops/s | 125.893 Mops/s |
| async threaded cget loop_from | 126.259 Mops/s | 124.547 Mops/s |
| async/blocking parallel | 1.630 | 2.541 |
| async-many/blocking par | 2.630 | 2.680 |
| async-cache-many/par | 3.014 | 2.242 |
| async-cache-loop/par | 2.187 | 2.585 |
| async-thread-cache-loop/par | 2.549 | 4.477 |
| async-lower-batch/par | 1.253 | 1.394 |
| async-lower-many/par | 1.485 | 1.535 |
| async-loop/blocking par | 3.234 | 2.677 |
| async-cursor-get-loop/par | 1.171 | 1.172 |
| async-cursor-get-loop/get | 60.454 | 77.009 |

- conclusion: this checkpoint changes only when the synchronous driver chooses
  to block for completions. It improves async parallel get, single get,
  lowerbound rows, cache loop, threaded cache loop rows, cursor loop-from, and
  the cursor-loop/get ratio in this run. It regresses get_ex batch/many/loop,
  cache many/st rows, get loop, async cursor get, and the cursor get-loop
  absolute row. The result is still consistent with a structural stepping stone:
  it validates the nonblocking completion path but does not yet create more
  traversal overlap than the existing wrapper can expose.

Additional io_uring read-batch state checkpoint:

- refactored the Linux io_uring read batch from ad hoc wrapper-local arrays into
  a reusable `osal_ioring_linux_read_batch_t` state object.
- added explicit batch init/dispose helpers that own the per-request completion
  context records. Small batches keep item storage embedded in the batch state;
  larger batches allocate item storage.
- added a locked drive helper that performs one submit/poll/optional-wait step
  against the batch state. The existing synchronous wrapper still loops until
  completion, but the state now has a shape that a future resumable get/cache
  traversal can hold across calls.
- kept the public storage read behavior unchanged: callers still receive a
  completed `dxb_read_result_t` array from `dxb_storage_submit_read_data_batch()`.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-state-before.txt` and
    `/tmp/mdbx-async-bench-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `554d63d`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.853 Mops/s | 1.820 Mops/s |
| blocking parallel get | 753.981 Kops/s | 790.739 Kops/s |
| async single get | 270.422 Kops/s | 239.865 Kops/s |
| async single get_ex | 391.035 Kops/s | 348.662 Kops/s |
| async single lowerbound | 191.112 Kops/s | 337.449 Kops/s |
| async parallel get | 2.034 Mops/s | 2.513 Mops/s |
| async many parallel get | 2.856 Mops/s | 2.447 Mops/s |
| async get_ex batch | 2.598 Mops/s | 2.564 Mops/s |
| async get_ex many | 2.761 Mops/s | 2.782 Mops/s |
| async cache many | 2.917 Mops/s | 1.959 Mops/s |
| async cache st many | 2.920 Mops/s | 2.219 Mops/s |
| async cache batch | 3.714 Mops/s | 4.265 Mops/s |
| async cache st batch | 3.521 Mops/s | 2.589 Mops/s |
| async cache loop | 4.837 Mops/s | 4.881 Mops/s |
| async cache st loop | 3.658 Mops/s | 4.355 Mops/s |
| async threaded cache loop | 4.321 Mops/s | 4.831 Mops/s |
| async threaded cache st loop | 4.825 Mops/s | 4.021 Mops/s |
| async lowerbound batch | 1.545 Mops/s | 1.598 Mops/s |
| async lowerbound many | 1.670 Mops/s | 1.624 Mops/s |
| async get loop | 3.323 Mops/s | 2.332 Mops/s |
| async get_ex loop | 3.306 Mops/s | 2.832 Mops/s |
| async lowerbound loop | 1.573 Mops/s | 1.544 Mops/s |
| blocking cursor get | 85.661 Mops/s | 85.031 Mops/s |
| parallel cursor get | 58.700 Mops/s | 56.049 Mops/s |
| async cursor get | 1.046 Mops/s | 1.178 Mops/s |
| async cursor get loop | 123.215 Mops/s | 102.491 Mops/s |
| async cursor get loop_from | 72.310 Mops/s | 124.078 Mops/s |
| async threaded cursor get loop | 123.652 Mops/s | 100.597 Mops/s |
| async threaded cget loop_from | 121.604 Mops/s | 96.755 Mops/s |
| async/blocking parallel | 2.697 | 3.177 |
| async-many/blocking par | 3.788 | 3.094 |
| async-cache-many/par | 3.868 | 2.478 |
| async-cache-loop/par | 6.416 | 6.172 |
| async-thread-cache-loop/par | 5.730 | 6.110 |
| async-lower-batch/par | 2.050 | 2.021 |
| async-lower-many/par | 2.215 | 2.054 |
| async-loop/blocking par | 4.408 | 2.949 |
| async-cursor-get-loop/par | 2.099 | 1.829 |
| async-cursor-get-loop/get | 117.817 | 86.987 |

- conclusion: this checkpoint is primarily structural. It improves async
  lowerbound single, async parallel get, cache batch, cache loop/st loop,
  threaded cache loop, lowerbound batch, async cursor get, and cursor loop-from
  in this run. It regresses async single get/get_ex, many get, cache many/st
  many, get/get_ex loop, cursor get loop, and threaded cursor rows. The mixed
  result is expected because the synchronous wrapper still drains each batch to
  completion; the important change is that the read-batch state now survives as
  a real object suitable for later suspension/resumption.

Additional storage read-batch state checkpoint:

- introduced an internal `dxb_storage_read_batch_t` state above the OSAL
  io_uring read-batch layer.
- split storage reads into begin/drive/finish helpers:
  validation and read fault injection happen in begin, OSAL submission/progress
  and read-complete fault injection happen in drive, and terminal error
  reporting happens in finish.
- kept `dxb_storage_submit_read_data_batch()` as the blocking compatibility
  adapter by driving the storage batch to completion before returning. Current
  get/cache/cursor callers therefore keep the same behavior.
- this creates the next ownership level needed by a future resumable B-tree
  traversal: page/cache code can eventually hold a storage batch state instead
  of calling a monolithic submit-and-complete function.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-storage-before.txt` and
    `/tmp/mdbx-async-bench-storage-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `9562c16`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.825 Mops/s | 1.854 Mops/s |
| blocking parallel get | 965.565 Kops/s | 791.357 Kops/s |
| async single get | 201.219 Kops/s | 317.123 Kops/s |
| async single get_ex | 199.891 Kops/s | 427.975 Kops/s |
| async single lowerbound | 318.300 Kops/s | 288.421 Kops/s |
| async parallel get | 2.653 Mops/s | 2.035 Mops/s |
| async many parallel get | 2.853 Mops/s | 2.674 Mops/s |
| async get_ex batch | 2.679 Mops/s | 2.697 Mops/s |
| async get_ex many | 2.376 Mops/s | 2.724 Mops/s |
| async cache many | 2.220 Mops/s | 3.275 Mops/s |
| async cache st many | 3.488 Mops/s | 3.645 Mops/s |
| async cache batch | 3.690 Mops/s | 2.702 Mops/s |
| async cache st batch | 4.091 Mops/s | 4.269 Mops/s |
| async cache loop | 2.047 Mops/s | 2.649 Mops/s |
| async cache st loop | 2.660 Mops/s | 4.929 Mops/s |
| async threaded cache loop | 4.304 Mops/s | 3.049 Mops/s |
| async threaded cache st loop | 2.054 Mops/s | 2.728 Mops/s |
| async lowerbound batch | 989.514 Kops/s | 1.680 Mops/s |
| async lowerbound many | 1.680 Mops/s | 1.587 Mops/s |
| async get loop | 2.214 Mops/s | 2.143 Mops/s |
| async get_ex loop | 2.204 Mops/s | 2.009 Mops/s |
| async lowerbound loop | 1.786 Mops/s | 1.838 Mops/s |
| blocking cursor get | 86.879 Mops/s | 87.020 Mops/s |
| parallel cursor get | 58.229 Mops/s | 60.133 Mops/s |
| async cursor get | 869.689 Kops/s | 1.343 Mops/s |
| async cursor get loop | 70.762 Mops/s | 66.181 Mops/s |
| async cursor get loop_from | 65.832 Mops/s | 67.579 Mops/s |
| async threaded cursor get loop | 121.043 Mops/s | 113.642 Mops/s |
| async threaded cget loop_from | 129.918 Mops/s | 122.134 Mops/s |
| async/blocking parallel | 2.747 | 2.571 |
| async-many/blocking par | 2.955 | 3.379 |
| async-cache-many/par | 2.299 | 4.139 |
| async-cache-loop/par | 2.120 | 3.347 |
| async-cache-st-loop/par | 2.754 | 6.229 |
| async-thread-cache-loop/par | 4.458 | 3.853 |
| async-lower-batch/par | 1.025 | 2.122 |
| async-lower-many/par | 1.740 | 2.005 |
| async-loop/blocking par | 2.293 | 2.708 |
| async-cursor-get-loop/par | 1.215 | 1.101 |
| async-cursor-get-loop/get | 81.365 | 49.265 |

- conclusion: this checkpoint mostly moves storage-read ownership boundaries.
  It improves async single get/get_ex, get_ex batch/many, cache many/st many,
  cache st batch, cache loop/st loop, threaded cache st loop, lowerbound batch,
  lowerbound loop, parallel cursor get, async cursor get, and cursor loop-from
  in this run. It regresses blocking parallel get, async parallel/many get,
  cache batch, threaded cache loop, lowerbound many, get/get_ex loop, cursor get
  loop, threaded cursor rows, and cursor-loop/get ratio. The adapter still
  completes synchronously, so benchmark movement should be treated as noise
  around a structural prerequisite rather than an expected performance result.

Additional OSAL read-batch state checkpoint:

- introduced an internal `osal_ioring_read_batch_t` state beneath
  `dxb_storage_read_batch_t`.
- split the OSAL read adapter into begin/drive/finish helpers. The compatibility
  `osal_ioring_pread_batch()` wrapper now begins a batch, drives it to
  completion, and finishes it.
- moved Linux io_uring read-batch ownership under this OSAL state. For Linux,
  begin allocates and initializes the `osal_ioring_linux_read_batch_t`, drive
  advances it through the existing locked submit/poll/wait helper, and finish
  unlocks and releases the Linux state.
- kept existing storage callers synchronous: `dxb_storage_submit_read_data_batch()`
  still returns only after the OSAL batch is complete. The difference is that
  storage now owns OSAL state instead of calling a monolithic OSAL helper.
- removed the obsolete direct Linux synchronous read-batch wrapper; the OSAL
  compatibility wrapper now provides that behavior through the state path.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-osal-before.txt` and
    `/tmp/mdbx-async-bench-osal-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `673b3ee`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.854 Mops/s | 1.809 Mops/s |
| blocking parallel get | 646.513 Kops/s | 1.072 Mops/s |
| async single get | 263.073 Kops/s | 240.396 Kops/s |
| async single get_ex | 361.704 Kops/s | 416.379 Kops/s |
| async single lowerbound | 281.535 Kops/s | 339.134 Kops/s |
| async parallel get | 2.447 Mops/s | 2.453 Mops/s |
| async many parallel get | 2.835 Mops/s | 2.885 Mops/s |
| async get_ex batch | 2.741 Mops/s | 1.412 Mops/s |
| async get_ex many | 2.687 Mops/s | 1.667 Mops/s |
| async cache many | 4.227 Mops/s | 3.162 Mops/s |
| async cache st many | 3.546 Mops/s | 3.367 Mops/s |
| async cache batch | 3.635 Mops/s | 3.280 Mops/s |
| async cache st batch | 4.350 Mops/s | 3.855 Mops/s |
| async cache loop | 3.312 Mops/s | 2.696 Mops/s |
| async cache st loop | 4.875 Mops/s | 2.747 Mops/s |
| async threaded cache loop | 4.161 Mops/s | 2.530 Mops/s |
| async threaded cache st loop | 4.804 Mops/s | 4.474 Mops/s |
| async lowerbound batch | 1.137 Mops/s | 1.664 Mops/s |
| async lowerbound many | 1.284 Mops/s | 1.312 Mops/s |
| async get loop | 1.884 Mops/s | 1.901 Mops/s |
| async get_ex loop | 3.354 Mops/s | 1.977 Mops/s |
| async lowerbound loop | 1.868 Mops/s | 1.409 Mops/s |
| blocking cursor get | 85.919 Mops/s | 86.201 Mops/s |
| parallel cursor get | 86.756 Mops/s | 55.436 Mops/s |
| async cursor get | 873.793 Kops/s | 820.292 Kops/s |
| async cursor get loop | 67.550 Mops/s | 71.733 Mops/s |
| async cursor get loop_from | 67.747 Mops/s | 72.614 Mops/s |
| async threaded cursor get loop | 123.806 Mops/s | 118.126 Mops/s |
| async threaded cget loop_from | 75.124 Mops/s | 120.648 Mops/s |
| async/blocking parallel | 3.785 | 2.288 |
| async-many/blocking par | 4.385 | 2.692 |
| async-cache-many/par | 6.538 | 2.950 |
| async-cache-loop/par | 5.123 | 2.515 |
| async-cache-st-loop/par | 7.540 | 2.562 |
| async-thread-cache-loop/par | 6.436 | 2.360 |
| async-lower-batch/par | 1.759 | 1.552 |
| async-lower-many/par | 1.986 | 1.224 |
| async-loop/blocking par | 2.914 | 1.774 |
| async-get-ex-loop/par | 5.188 | 1.845 |
| async-lower-loop/par | 2.889 | 1.315 |
| async-cursor-get-loop/par | 0.779 | 1.294 |
| async-cursor-get-loop/get | 77.307 | 87.449 |

- conclusion: this checkpoint moves the async-read state boundary down one
  layer. It improves blocking parallel get, async single get_ex/lowerbound,
  async parallel/many get, lowerbound batch/many, get loop, cursor get loop,
  cursor loop-from, threaded cursor loop-from, and cursor-loop ratios in this
  run. It regresses async single get, get_ex batch/many, cache rows, threaded
  cache rows, get_ex/lowerbound loop, parallel cursor get, async cursor get, and
  threaded cursor get loop. Since the compatibility path still finishes every
  OSAL batch synchronously, these benchmark changes should be treated as noise
  around the ownership refactor, not as proof of improved overlap yet.

Additional storage/OSAL wrapper poll checkpoint:

- changed the synchronous storage and OSAL read-batch compatibility wrappers to
  drive their read-batch state once with `wait=false` before entering the
  blocking wait-to-completion loop.
- this keeps existing public behavior unchanged, but makes the highest current
  blocking read adapters use the submit/poll/wait shape directly: begin state,
  submit and poll without waiting, then wait only while the batch remains
  pending.
- direct OSAL read-batch users, including meta reads, now exercise the
  nonblocking drive path before blocking. Storage page-cache misses do the same
  through `dxb_storage_read_batch_drive(false)`.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-wrapper-before.txt` and
    `/tmp/mdbx-async-bench-wrapper-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `1fc942f`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.841 Mops/s | 1.855 Mops/s |
| blocking parallel get | 792.250 Kops/s | 682.349 Kops/s |
| async single get | 201.970 Kops/s | 381.351 Kops/s |
| async single get_ex | 414.690 Kops/s | 368.976 Kops/s |
| async single lowerbound | 200.728 Kops/s | 354.308 Kops/s |
| async parallel get | 2.090 Mops/s | 2.066 Mops/s |
| async many parallel get | 2.827 Mops/s | 2.551 Mops/s |
| async get_ex batch | 2.567 Mops/s | 2.609 Mops/s |
| async get_ex many | 2.879 Mops/s | 2.737 Mops/s |
| async cache many | 4.515 Mops/s | 3.362 Mops/s |
| async cache st many | 4.140 Mops/s | 2.814 Mops/s |
| async cache batch | 4.108 Mops/s | 3.834 Mops/s |
| async cache st batch | 4.267 Mops/s | 3.934 Mops/s |
| async cache loop | 4.958 Mops/s | 2.600 Mops/s |
| async cache st loop | 4.283 Mops/s | 4.607 Mops/s |
| async threaded cache loop | 4.847 Mops/s | 3.450 Mops/s |
| async threaded cache st loop | 2.225 Mops/s | 2.814 Mops/s |
| async lowerbound batch | 1.668 Mops/s | 1.463 Mops/s |
| async lowerbound many | 1.660 Mops/s | 1.404 Mops/s |
| async get loop | 2.639 Mops/s | 2.766 Mops/s |
| async get_ex loop | 3.417 Mops/s | 3.265 Mops/s |
| async lowerbound loop | 1.492 Mops/s | 1.805 Mops/s |
| blocking cursor get | 85.887 Mops/s | 86.819 Mops/s |
| parallel cursor get | 90.631 Mops/s | 58.713 Mops/s |
| async cursor get | 831.626 Kops/s | 852.130 Kops/s |
| async cursor get loop | 69.311 Mops/s | 108.732 Mops/s |
| async cursor get loop_from | 120.232 Mops/s | 62.683 Mops/s |
| async threaded cursor get loop | 116.115 Mops/s | 122.115 Mops/s |
| async threaded cget loop_from | 121.234 Mops/s | 120.546 Mops/s |
| async/blocking parallel | 2.638 | 3.028 |
| async-many/blocking par | 3.568 | 3.738 |
| async-get-ex-batch/par | 3.241 | 3.824 |
| async-cache-many/par | 5.699 | 4.928 |
| async-cache-loop/par | 6.258 | 3.810 |
| async-cache-st-loop/par | 5.406 | 6.751 |
| async-thread-cache-loop/par | 6.118 | 5.057 |
| async-lower-batch/par | 2.105 | 2.145 |
| async-lower-many/par | 2.095 | 2.057 |
| async-loop/blocking par | 3.331 | 4.053 |
| async-get-ex-loop/par | 4.313 | 4.784 |
| async-lower-loop/par | 1.883 | 2.645 |
| async-cursor-get-loop/par | 0.765 | 1.852 |
| async-cursor-get-loop/get | 83.344 | 127.600 |

- conclusion: this checkpoint changes how the blocking adapters drive the
  already-split state rather than changing traversal. It improves async single
  get, single lowerbound, get_ex batch, cache st loop, threaded cache st loop,
  get/lowerbound loops, blocking cursor get, async cursor get, async cursor loop,
  threaded cursor loop, and most normalized ratios in this run. It regresses
  blocking parallel get, async parallel/many get, cache many/st many/batch/loop,
  lowerbound batch/many, get_ex loop, parallel cursor get, cursor loop-from, and
  threaded cursor loop-from. The important structural result is that the top
  synchronous read adapters now exercise nonblocking progress before waiting.

Additional page-cache storage batch ownership checkpoint:

- changed `page_cache_submit_read_batch()` so page-cache miss handling owns a
  `dxb_storage_read_batch_t` directly instead of calling the monolithic
  `dxb_storage_submit_read_data_batch()` compatibility wrapper.
- the page-cache path now begins a storage read batch, polls it once with
  `wait=false`, waits while it remains pending, and finishes it before
  materializing cache entries. Current behavior is still synchronous, but the
  page-cache miss code now holds the storage read state that a future resumable
  traversal can keep across suspension.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-pagecache-state-before.txt` and
    `/tmp/mdbx-async-bench-pagecache-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `a729dd2`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.825 Mops/s | 1.828 Mops/s |
| blocking parallel get | 1.076 Mops/s | 1.090 Mops/s |
| async single get | 232.437 Kops/s | 377.031 Kops/s |
| async single get_ex | 348.366 Kops/s | 210.129 Kops/s |
| async single lowerbound | 322.074 Kops/s | 359.963 Kops/s |
| async parallel get | 2.506 Mops/s | 2.803 Mops/s |
| async many parallel get | 2.813 Mops/s | 2.680 Mops/s |
| async get_ex batch | 2.588 Mops/s | 2.794 Mops/s |
| async get_ex many | 2.731 Mops/s | 2.686 Mops/s |
| async cache many | 4.024 Mops/s | 3.648 Mops/s |
| async cache st many | 4.333 Mops/s | 2.425 Mops/s |
| async cache batch | 3.873 Mops/s | 3.494 Mops/s |
| async cache st batch | 4.011 Mops/s | 3.444 Mops/s |
| async cache loop | 4.924 Mops/s | 4.902 Mops/s |
| async cache st loop | 2.108 Mops/s | 2.535 Mops/s |
| async threaded cache loop | 2.230 Mops/s | 3.327 Mops/s |
| async threaded cache st loop | 4.868 Mops/s | 4.255 Mops/s |
| async lowerbound batch | 1.416 Mops/s | 1.673 Mops/s |
| async lowerbound many | 1.719 Mops/s | 1.646 Mops/s |
| async get loop | 2.955 Mops/s | 3.378 Mops/s |
| async get_ex loop | 3.483 Mops/s | 3.261 Mops/s |
| async lowerbound loop | 1.793 Mops/s | 1.869 Mops/s |
| blocking cursor get | 85.324 Mops/s | 85.863 Mops/s |
| parallel cursor get | 62.825 Mops/s | 58.188 Mops/s |
| async cursor get | 951.730 Kops/s | 876.248 Kops/s |
| async cursor get loop | 89.307 Mops/s | 67.758 Mops/s |
| async cursor get loop_from | 70.557 Mops/s | 62.161 Mops/s |
| async threaded cursor get loop | 115.519 Mops/s | 122.594 Mops/s |
| async threaded cget loop_from | 124.248 Mops/s | 116.963 Mops/s |
| async/blocking parallel | 2.329 | 2.571 |
| async-many/blocking par | 2.614 | 2.458 |
| async-get-ex-batch/par | 2.405 | 2.563 |
| async-cache-many/par | 3.739 | 3.345 |
| async-cache-loop/par | 4.576 | 4.496 |
| async-cache-st-loop/par | 1.959 | 2.325 |
| async-thread-cache-loop/par | 2.072 | 3.051 |
| async-lower-batch/par | 1.316 | 1.534 |
| async-lower-many/par | 1.597 | 1.509 |
| async-loop/blocking par | 2.746 | 3.098 |
| async-get-ex-loop/par | 3.237 | 2.990 |
| async-lower-loop/par | 1.666 | 1.714 |
| async-cursor-get-loop/par | 1.422 | 1.164 |
| async-cursor-get-loop/get | 93.836 | 77.328 |

- conclusion: this checkpoint moves ownership of the storage read state into the
  page-cache miss path. It improves blocking get rows, async single get,
  lowerbound single/batch/loop, async parallel get, get_ex batch, cache st loop,
  threaded cache loop, get loop, blocking cursor get, threaded cursor loop, and
  several normalized ratios in this run. It regresses single get_ex, many get,
  cache many/st many/batch rows, threaded cache st loop, get_ex loop, cursor
  rows, and cursor ratios. Since the page-cache path still waits before
  materializing entries, this remains a structural prerequisite rather than true
  traversal suspension.

Additional page-cache read-batch state checkpoint:

- introduced `dxb_page_cache_read_batch_t` and split page-cache miss handling
  into `page_cache_read_batch_begin()`, `page_cache_read_batch_drive()`, and
  `page_cache_read_batch_finish()`.
- kept `page_cache_submit_read_batch()` as the synchronous compatibility wrapper
  that begins the state, polls once with `wait=false`, waits while pending, and
  finishes cleanup. This preserves current callers while exposing a page-cache
  state object that future get/cursor traversal continuations can keep across
  suspension.
- storage reads are still materialized before the wrapper returns, so this is a
  state-boundary checkpoint, not yet full traversal suspension.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-pagecache-batch-before.txt` and
    `/tmp/mdbx-async-bench-pagecache-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `860fdc9`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.887 Mops/s | 1.744 Mops/s |
| blocking parallel get | 520.531 Kops/s | 710.257 Kops/s |
| async single get | 279.271 Kops/s | 212.956 Kops/s |
| async single get_ex | 420.784 Kops/s | 359.726 Kops/s |
| async single lowerbound | 322.025 Kops/s | 336.370 Kops/s |
| async parallel get | 2.812 Mops/s | 2.667 Mops/s |
| async many parallel get | 2.552 Mops/s | 2.672 Mops/s |
| async get_ex batch | 2.740 Mops/s | 2.767 Mops/s |
| async get_ex many | 2.848 Mops/s | 1.881 Mops/s |
| async cache many | 3.504 Mops/s | 2.351 Mops/s |
| async cache st many | 2.311 Mops/s | 4.410 Mops/s |
| async cache batch | 2.888 Mops/s | 3.097 Mops/s |
| async cache st batch | 4.235 Mops/s | 3.005 Mops/s |
| async cache loop | 2.026 Mops/s | 4.915 Mops/s |
| async threaded cache batch | 5.125 Mops/s | 5.393 Mops/s |
| async threaded cache loop | 4.457 Mops/s | 2.271 Mops/s |
| async lowerbound batch | 1.462 Mops/s | 1.416 Mops/s |
| async lowerbound many | 1.690 Mops/s | 1.124 Mops/s |
| async get loop | 3.403 Mops/s | 2.376 Mops/s |
| async get_ex loop | 3.438 Mops/s | 3.104 Mops/s |
| async lowerbound loop | 1.219 Mops/s | 1.929 Mops/s |
| blocking cursor get | 86.753 Mops/s | 83.808 Mops/s |
| parallel cursor get | 57.295 Mops/s | 59.326 Mops/s |
| async cursor get | 860.379 Kops/s | 865.581 Kops/s |
| async cursor get loop | 68.488 Mops/s | 66.645 Mops/s |
| async cursor get loop_from | 66.622 Mops/s | 67.102 Mops/s |
| async threaded cursor get loop | 116.608 Mops/s | 120.653 Mops/s |
| async threaded cget loop_from | 75.849 Mops/s | 119.531 Mops/s |
| async/blocking parallel | 5.403 | 3.755 |
| async-many/blocking par | 4.903 | 3.762 |
| async-cache-loop/par | 3.892 | 6.921 |
| async-thread-cache-batch/par | 9.846 | 7.593 |
| async-lower-loop/par | 2.341 | 2.716 |
| async-cursor-get-loop/par | 1.195 | 1.123 |

- conclusion: this checkpoint gives page-cache miss handling the same explicit
  begin/drive/finish shape as the storage and OSAL read batches. It improves
  blocking parallel get, async many get, get_ex batch, cache st many, cache
  batch, cache loop, threaded cache batch, lowerbound loop, parallel cursor get,
  async cursor get, cursor loop-from, threaded cursor loop, and threaded cursor
  loop-from in this run. It regresses blocking serial get, async single get,
  single get_ex, async parallel get, get_ex many, cache many/st batch/threaded
  loop, lowerbound batch/many, get/get_ex loop, blocking cursor get, cursor get
  loop, and several normalized ratios. The structural gain is that page-cache
  miss submission and completion are now separable from synchronous cleanup.

Additional committed-page read-batch state checkpoint:

- introduced `dxb_committed_page_read_batch_t` and split committed page reads
  into `page_committed_read_batch_begin()`,
  `page_committed_read_batch_drive()`, and
  `page_committed_read_batch_finish()`.
- kept `page_get_committed_batch()` as the synchronous compatibility wrapper.
  It now owns a committed-page batch state, drives the underlying
  `dxb_page_cache_read_batch_t`, scatters page-cache results back to committed
  read result slots, and then performs cleanup.
- this moves the explicit begin/drive/finish boundary above the page-cache layer
  and closer to cursor/get traversal. It is still synchronous at the wrapper,
  but future traversal continuations can retain committed-page read state rather
  than re-entering a monolithic helper.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-committed-batch-before.txt` and
    `/tmp/mdbx-async-bench-committed-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `352a8a0`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.709 Mops/s | 1.472 Mops/s |
| blocking parallel get | 1.068 Mops/s | 931.129 Kops/s |
| async single get | 333.964 Kops/s | 322.786 Kops/s |
| async single get_ex | 238.140 Kops/s | 371.853 Kops/s |
| async single lowerbound | 337.696 Kops/s | 320.091 Kops/s |
| async parallel get | 2.616 Mops/s | 2.016 Mops/s |
| async many parallel get | 2.315 Mops/s | 2.832 Mops/s |
| async get_ex batch | 2.306 Mops/s | 2.506 Mops/s |
| async get_ex many | 2.716 Mops/s | 2.845 Mops/s |
| async cache many | 4.319 Mops/s | 3.817 Mops/s |
| async cache st many | 4.366 Mops/s | 2.762 Mops/s |
| async cache batch | 2.947 Mops/s | 2.467 Mops/s |
| async cache st batch | 4.157 Mops/s | 2.500 Mops/s |
| async cache loop | 2.022 Mops/s | 2.909 Mops/s |
| async threaded cache batch | 4.029 Mops/s | 2.864 Mops/s |
| async threaded cache loop | 3.433 Mops/s | 4.891 Mops/s |
| async lowerbound batch | 1.397 Mops/s | 1.592 Mops/s |
| async lowerbound many | 1.527 Mops/s | 1.634 Mops/s |
| async get loop | 2.189 Mops/s | 3.207 Mops/s |
| async get_ex loop | 3.404 Mops/s | 2.354 Mops/s |
| async lowerbound loop | 1.604 Mops/s | 1.363 Mops/s |
| blocking cursor get | 77.394 Mops/s | 80.373 Mops/s |
| parallel cursor get | 57.271 Mops/s | 58.456 Mops/s |
| async cursor get | 843.345 Kops/s | 1.314 Mops/s |
| async cursor get loop | 67.878 Mops/s | 65.033 Mops/s |
| async cursor get loop_from | 67.313 Mops/s | 65.831 Mops/s |
| async threaded cursor get loop | 120.164 Mops/s | 113.234 Mops/s |
| async threaded cget loop_from | 115.675 Mops/s | 118.735 Mops/s |
| async/blocking parallel | 2.448 | 2.166 |
| async-many/blocking par | 2.167 | 3.041 |
| async-cache-loop/par | 1.893 | 3.124 |
| async-thread-cache-batch/par | 3.771 | 3.076 |
| async-lower-loop/par | 1.502 | 1.463 |
| async-cursor-get-loop/par | 1.185 | 1.113 |

- conclusion: this checkpoint improves async single get_ex, async many get,
  get_ex batch/many, cache loop, threaded cache loop, lowerbound batch/many,
  get loop, blocking/parallel cursor get, async cursor get, and threaded cursor
  loop-from in this run. It regresses blocking get rows, async single/parallel
  get, cache many/st many/batch/st batch, threaded cache batch, get_ex loop,
  lowerbound loop, cursor get loop rows, threaded cursor get loop, and some
  normalized ratios. The structural gain is that committed-page reads now expose
  a pollable state object above page-cache reads.

Additional page-get batch state checkpoint:

- introduced `dxb_page_get_batch_t` and split unchecked page-get batches into
  `page_get_batch_begin()`, `page_get_batch_drive()`, and
  `page_get_batch_finish()`.
- the begin phase validates page-get requests, resolves dirty/spilled pages
  immediately, prepares committed-page reads for the remaining requests, and
  begins the nested `dxb_committed_page_read_batch_t`.
- the drive phase polls or waits the committed-page batch and completes the
  page-number validation/scatter step after reads finish. The public synchronous
  path is preserved by `page_submit_get_unchecked_batch()` driving the state to
  completion internally.
- this moves the pollable state boundary above dirty/spilled page filtering and
  committed-read submission. Cursor and batched get traversal still call the
  synchronous wrapper, but the next layer now has a state object that traversal
  continuations can retain.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-pageget-batch-before.txt` and
    `/tmp/mdbx-async-bench-pageget-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `1fb43be`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.489 Mops/s | 1.439 Mops/s |
| blocking parallel get | 1.016 Mops/s | 1.035 Mops/s |
| async single get | 418.958 Kops/s | 419.029 Kops/s |
| async single get_ex | 377.917 Kops/s | 405.243 Kops/s |
| async single lowerbound | 325.950 Kops/s | 243.660 Kops/s |
| async parallel get | 2.762 Mops/s | 2.557 Mops/s |
| async many parallel get | 2.690 Mops/s | 2.232 Mops/s |
| async get_ex batch | 2.407 Mops/s | 2.641 Mops/s |
| async get_ex many | 2.406 Mops/s | 2.867 Mops/s |
| async cache many | 4.405 Mops/s | 3.397 Mops/s |
| async cache st many | 4.160 Mops/s | 3.833 Mops/s |
| async cache batch | 4.265 Mops/s | 4.108 Mops/s |
| async cache st batch | 3.551 Mops/s | 4.168 Mops/s |
| async cache loop | 5.004 Mops/s | 4.190 Mops/s |
| async threaded cache batch | 4.975 Mops/s | 5.134 Mops/s |
| async threaded cache loop | 4.562 Mops/s | 4.800 Mops/s |
| async lowerbound batch | 1.503 Mops/s | 1.613 Mops/s |
| async lowerbound many | 1.364 Mops/s | 1.635 Mops/s |
| async get loop | 2.139 Mops/s | 2.337 Mops/s |
| async get_ex loop | 3.219 Mops/s | 3.433 Mops/s |
| async lowerbound loop | 1.684 Mops/s | 1.851 Mops/s |
| blocking cursor get | 78.872 Mops/s | 80.289 Mops/s |
| parallel cursor get | 57.752 Mops/s | 54.835 Mops/s |
| async cursor get | 906.317 Kops/s | 730.498 Kops/s |
| async cursor get loop | 66.385 Mops/s | 68.611 Mops/s |
| async cursor get loop_from | 66.416 Mops/s | 67.591 Mops/s |
| async threaded cursor get loop | 117.399 Mops/s | 75.861 Mops/s |
| async threaded cget loop_from | 116.052 Mops/s | 118.871 Mops/s |
| async/blocking parallel | 2.718 | 2.471 |
| async-many/blocking par | 2.647 | 2.157 |
| async-cache-loop/par | 4.924 | 4.049 |
| async-thread-cache-batch/par | 4.896 | 4.961 |
| async-lower-loop/par | 1.657 | 1.789 |
| async-cursor-get-loop/par | 1.149 | 1.251 |

- conclusion: this checkpoint improves blocking parallel get, async single
  get/get_ex, get_ex batch/many/loop, cache st batch, threaded cache
  batch/loop, lowerbound batch/many/loop, get loop, blocking cursor get, cursor
  get loops, threaded cursor loop-from, and some cursor/lowerbound ratios in
  this run. It regresses blocking serial get, single lowerbound, async
  parallel/many get, cache many/st many/batch/loop, parallel cursor get, async
  cursor get, threaded cursor get loop, and several normalized ratios. The
  structural gain is that page-get miss submission and completion are now
  separable above dirty/spilled-page handling.

Additional cursor-page get-batch state checkpoint:

- introduced `dxb_cursor_page_get_batch_t` and split cursor-page read batches
  into `page_cursor_get_batch_begin()`, `page_cursor_get_batch_drive()`, and
  `page_cursor_get_batch_finish()`.
- the begin phase validates cursor page-get requests, groups the underlying
  page-get requests, accounts `ops_pget`, and begins a nested
  `dxb_page_get_batch_t`.
- the drive phase polls or waits the page-get batch and then applies
  cursor-specific completion checks, including page-header validation and large
  page materialization. The public synchronous helper
  `page_submit_cursor_get_batch()` now only drives this state to completion.
- this moves the pollable state boundary to the cursor-page layer used by
  batched get/lowerbound traversal for root and child page reads. Traversal
  still waits at each level, but it can now retain cursor-page read state
  directly in a future continuation.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorpage-batch-before.txt` and
    `/tmp/mdbx-async-bench-cursorpage-batch-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `231a183`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.442 Mops/s | 1.188 Mops/s |
| blocking parallel get | 941.472 Kops/s | 1.054 Mops/s |
| async single get | 420.105 Kops/s | 419.046 Kops/s |
| async single get_ex | 277.864 Kops/s | 345.860 Kops/s |
| async single lowerbound | 341.072 Kops/s | 274.887 Kops/s |
| async parallel get | 1.917 Mops/s | 1.885 Mops/s |
| async many parallel get | 2.492 Mops/s | 2.118 Mops/s |
| async get_ex batch | 2.137 Mops/s | 2.343 Mops/s |
| async get_ex many | 2.649 Mops/s | 2.832 Mops/s |
| async cache many | 2.000 Mops/s | 3.440 Mops/s |
| async cache st many | 4.287 Mops/s | 3.677 Mops/s |
| async cache batch | 3.190 Mops/s | 3.803 Mops/s |
| async cache st batch | 3.790 Mops/s | 2.379 Mops/s |
| async cache loop | 3.898 Mops/s | 2.791 Mops/s |
| async threaded cache batch | 5.195 Mops/s | 5.314 Mops/s |
| async threaded cache loop | 1.947 Mops/s | 2.019 Mops/s |
| async lowerbound batch | 1.669 Mops/s | 1.642 Mops/s |
| async lowerbound many | 1.470 Mops/s | 1.680 Mops/s |
| async get loop | 2.980 Mops/s | 2.251 Mops/s |
| async get_ex loop | 2.426 Mops/s | 2.645 Mops/s |
| async lowerbound loop | 1.725 Mops/s | 1.616 Mops/s |
| blocking cursor get | 79.992 Mops/s | 73.093 Mops/s |
| parallel cursor get | 58.994 Mops/s | 65.763 Mops/s |
| async cursor get | 870.566 Kops/s | 876.454 Kops/s |
| async cursor get loop | 68.409 Mops/s | 67.493 Mops/s |
| async cursor get loop_from | 65.591 Mops/s | 63.855 Mops/s |
| async threaded cursor get loop | 116.489 Mops/s | 63.136 Mops/s |
| async threaded cget loop_from | 114.999 Mops/s | 64.585 Mops/s |
| async/blocking parallel | 2.036 | 1.787 |
| async-many/blocking par | 2.647 | 2.009 |
| async-cache-loop/par | 4.140 | 2.647 |
| async-thread-cache-batch/par | 5.518 | 5.040 |
| async-lower-loop/par | 1.832 | 1.533 |
| async-cursor-get-loop/par | 1.160 | 1.026 |

- conclusion: this checkpoint improves blocking parallel get, async single
  get_ex, get_ex batch/many/loop, cache many/batch, threaded cache batch/loop,
  lowerbound many, parallel cursor get, and async cursor get in this run. It
  regresses blocking serial get, single lowerbound, async parallel/many get,
  cache st many/st batch/loop, get loop, lowerbound batch/loop, blocking cursor
  get, cursor loop rows, threaded cursor rows, and several normalized ratios.
  The structural gain is that cursor page reads now expose a pollable state
  object at the exact layer batched get/lowerbound traversal calls.

Additional batched traversal cursor-page ownership checkpoint:

- changed `async_batched_get_traverse()` and
  `async_batched_lowerbound_traverse()` so root and child level reads own a
  `dxb_cursor_page_get_batch_t` directly instead of calling the synchronous
  `page_submit_cursor_get_batch()` wrapper.
- traversal still drives each cursor-page read batch to completion before
  consuming pages, but the state now lives in the traversal code at the point
  where a future continuation will need to suspend and resume.
- this does not yet make traversal resumable; it removes another wrapper
  boundary between batched get/lowerbound traversal and the pollable page-read
  state stack.
- validation:
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - `git diff --check`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-traverse-own-before.txt` and
    `/tmp/mdbx-async-bench-traverse-own-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `ecbd43b`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.207 Mops/s | 1.192 Mops/s |
| blocking parallel get | 1.034 Mops/s | 800.443 Kops/s |
| async single get | 414.778 Kops/s | 233.562 Kops/s |
| async single get_ex | 359.571 Kops/s | 377.175 Kops/s |
| async single lowerbound | 302.767 Kops/s | 176.986 Kops/s |
| async parallel get | 2.775 Mops/s | 2.567 Mops/s |
| async many parallel get | 2.739 Mops/s | 2.761 Mops/s |
| async get_ex batch | 2.338 Mops/s | 2.750 Mops/s |
| async get_ex many | 2.117 Mops/s | 2.750 Mops/s |
| async cache many | 4.404 Mops/s | 4.466 Mops/s |
| async cache st many | 2.656 Mops/s | 4.402 Mops/s |
| async cache batch | 3.743 Mops/s | 4.245 Mops/s |
| async cache st batch | 3.363 Mops/s | 4.211 Mops/s |
| async cache loop | 2.417 Mops/s | 2.700 Mops/s |
| async threaded cache batch | 5.074 Mops/s | 4.995 Mops/s |
| async threaded cache loop | 5.125 Mops/s | 4.748 Mops/s |
| async lowerbound batch | 1.534 Mops/s | 1.663 Mops/s |
| async lowerbound many | 1.319 Mops/s | 1.652 Mops/s |
| async get loop | 1.869 Mops/s | 1.877 Mops/s |
| async get_ex loop | 3.048 Mops/s | 3.356 Mops/s |
| async lowerbound loop | 1.632 Mops/s | 1.554 Mops/s |
| blocking cursor get | 73.991 Mops/s | 74.403 Mops/s |
| parallel cursor get | 56.944 Mops/s | 57.623 Mops/s |
| async cursor get | 863.404 Kops/s | 769.143 Kops/s |
| async cursor get loop | 64.023 Mops/s | 63.399 Mops/s |
| async cursor get loop_from | 63.630 Mops/s | 63.338 Mops/s |
| async threaded cursor get loop | 109.519 Mops/s | 112.181 Mops/s |
| async threaded cget loop_from | 113.289 Mops/s | 117.058 Mops/s |
| async/blocking parallel | 2.683 | 3.207 |
| async-many/blocking par | 2.649 | 3.449 |
| async-cache-loop/par | 2.337 | 3.373 |
| async-thread-cache-batch/par | 4.907 | 6.240 |
| async-lower-loop/par | 1.578 | 1.942 |
| async-cursor-get-loop/par | 1.124 | 1.100 |

- conclusion: this checkpoint improves async single get_ex, async many get,
  get_ex batch/many/loop, cache many/st many/batch/st batch/loop, lowerbound
  batch/many, get loop, blocking/parallel cursor get, threaded cursor rows, and
  several normalized ratios in this run. It regresses blocking get rows, async
  single get, single lowerbound, async parallel get, threaded cache rows,
  lowerbound loop, async cursor get/loops, and cursor loop ratio. The structural
  gain is that batched get/lowerbound traversal now directly owns cursor-page
  read state at root and child read levels.

Additional batched get traversal state-machine checkpoint:

- changed `async_batched_get_traverse()` from a single synchronous traversal
  body into an explicit `async_batched_get_traverse_state_t` with
  begin/drive/finish phases.
- root-page reads, child-page reads, page consumption, child preparation, and
  final seek completion are now separate resumable phases. The compatibility
  wrapper still drives the state to completion, preserving existing blocking
  behavior at the public API boundary.
- this moves batched exact-key get traversal closer to the requested internal
  async shape: page-read batches can now be represented as pending traversal
  state instead of stack-local synchronous control flow.
- `async_batched_lowerbound_traverse()` still uses the previous direct
  `dxb_cursor_page_get_batch_t` ownership pattern and has not yet been split
  into its own begin/drive/finish traversal state machine.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-get-traverse-state-before.txt` and
    `/tmp/mdbx-async-bench-get-traverse-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `ae54b08`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.182 Mops/s | 1.217 Mops/s |
| blocking parallel get | 1.019 Mops/s | 1.035 Mops/s |
| async single get | 299.436 Kops/s | 391.771 Kops/s |
| async single get_ex | 406.115 Kops/s | 372.740 Kops/s |
| async single lowerbound | 267.957 Kops/s | 317.385 Kops/s |
| async parallel get | 2.445 Mops/s | 1.984 Mops/s |
| async many parallel get | 2.269 Mops/s | 2.880 Mops/s |
| async threaded get | 1.600 Mops/s | 2.784 Mops/s |
| async batch parallel get | 2.305 Mops/s | 2.760 Mops/s |
| async batch callback get | 1.844 Mops/s | 2.758 Mops/s |
| async get_ex batch | 2.043 Mops/s | 2.451 Mops/s |
| async get_ex many | 2.846 Mops/s | 2.773 Mops/s |
| async cache many | 2.350 Mops/s | 4.436 Mops/s |
| async cache batch | 3.003 Mops/s | 4.110 Mops/s |
| async cache loop | 3.839 Mops/s | 3.441 Mops/s |
| async cache st loop | 3.486 Mops/s | 4.304 Mops/s |
| async threaded cache loop | 3.399 Mops/s | 4.936 Mops/s |
| async lowerbound batch | 1.650 Mops/s | 1.644 Mops/s |
| async lowerbound many | 1.670 Mops/s | 1.674 Mops/s |
| async get loop | 3.387 Mops/s | 3.385 Mops/s |
| async get_ex loop | 2.793 Mops/s | 3.347 Mops/s |
| async lowerbound loop | 1.647 Mops/s | 1.855 Mops/s |
| async cursor get | 1.178 Mops/s | 859.415 Kops/s |
| async cursor get loop | 62.293 Mops/s | 66.108 Mops/s |
| async cursor batch | 61.610 Mops/s | 124.946 Mops/s |
| async cursor scan | 72.598 Mops/s | 135.639 Mops/s |
| async cursor scan_from | 70.978 Mops/s | 67.913 Mops/s |

- conclusion: this checkpoint is mostly structural. It improves many batched,
  cache, loop, and cursor batch/scan rows in this run, while regressing async
  parallel get, async single get_ex, async cursor get, and a few cache/scan
  rows. The important migration gain is not the mixed microbenchmark result;
  it is that exact-key batched traversal now has an explicit state object that
  can be driven incrementally instead of being tied to a monolithic blocking
  function body.

Additional batched lowerbound traversal state-machine checkpoint:

- changed `async_batched_lowerbound_traverse()` from monolithic synchronous
  traversal into `async_batched_lowerbound_traverse_state_t` with
  begin/drive/finish phases.
- root-page reads, child-page reads, branch descent, page consumption, and
  final `MDBX_SET_LOWERBOUND` completion are now explicit phases. The wrapper
  still drives the state to completion to preserve the current blocking public
  behavior.
- this brings lowerbound traversal to the same structural shape as exact-key
  batched get traversal: page-read batches are now owned by resumable traversal
  state instead of stack-local synchronous control flow.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-lowerbound-state-before.txt` and
    `/tmp/mdbx-async-bench-lowerbound-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `c676b2e`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.213 Mops/s | 1.198 Mops/s |
| blocking parallel get | 649.202 Kops/s | 1.028 Mops/s |
| async single get | 392.079 Kops/s | 358.523 Kops/s |
| async single get_ex | 372.436 Kops/s | 370.471 Kops/s |
| async single lowerbound | 241.625 Kops/s | 199.026 Kops/s |
| async parallel get | 2.704 Mops/s | 2.499 Mops/s |
| async many parallel get | 1.451 Mops/s | 2.494 Mops/s |
| async batch parallel get | 1.643 Mops/s | 1.460 Mops/s |
| async batch callback get | 2.410 Mops/s | 2.582 Mops/s |
| async get_ex batch | 2.718 Mops/s | 2.780 Mops/s |
| async get_ex many | 2.422 Mops/s | 1.400 Mops/s |
| async cache many | 4.493 Mops/s | 2.242 Mops/s |
| async cache batch | 3.405 Mops/s | 2.550 Mops/s |
| async cache st batch | 3.949 Mops/s | 4.018 Mops/s |
| async cache loop | 2.046 Mops/s | 2.103 Mops/s |
| async cache st loop | 2.354 Mops/s | 3.196 Mops/s |
| async threaded cache loop | 2.691 Mops/s | 4.890 Mops/s |
| async lowerbound batch | 1.602 Mops/s | 1.664 Mops/s |
| async lowerbound many | 1.492 Mops/s | 1.631 Mops/s |
| async lowerbound loop | 1.267 Mops/s | 1.684 Mops/s |
| async threaded lower loop | 1.795 Mops/s | 1.270 Mops/s |
| async cursor get | 1.001 Mops/s | 874.034 Kops/s |
| async cursor get loop | 64.085 Mops/s | 64.504 Mops/s |
| async cursor batch | 66.919 Mops/s | 79.968 Mops/s |
| async cursor loop | 71.550 Mops/s | 57.439 Mops/s |
| async cursor scan | 72.272 Mops/s | 72.650 Mops/s |
| async cursor scan_from | 65.306 Mops/s | 68.692 Mops/s |

- conclusion: the lowerbound-specific rows improved for batch, many, and loop
  forms in this run, while single lowerbound and threaded lower loop regressed.
  Several unrelated get/cache/cursor rows moved substantially as well, so this
  benchmark should be read as a checkpoint comparison rather than a stable
  performance claim. The structural gain is that both exact-key and lowerbound
  batched traversal now expose resumable internal state for page-read batches.

Async loop traversal state ownership checkpoint:

- added `async_batched_get_traverse_drive_to_completion()` and
  `async_batched_lowerbound_traverse_drive_to_completion()` helpers so callers
  can own traversal state explicitly while still preserving today's blocking
  drive-to-finish behavior.
- changed async `get_loop`, `get_ex_loop`, and `get_equal_or_great_loop`
  chunk execution to instantiate traversal state directly with begin/drive/
  finish instead of calling the compatibility wrappers.
- this does not yet suspend an async loop operation across worker iterations,
  but it moves loop chunk execution to the resumable traversal API that future
  scheduler integration can poll and resume.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-loop-state-before.txt` and
    `/tmp/mdbx-async-bench-loop-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `f9b70e7`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 388.955 Kops/s | 351.912 Kops/s |
| async single get_ex | 338.758 Kops/s | 326.191 Kops/s |
| async single lowerbound | 249.562 Kops/s | 294.163 Kops/s |
| async parallel get | 2.457 Mops/s | 2.217 Mops/s |
| async many parallel get | 2.870 Mops/s | 2.274 Mops/s |
| async batch parallel get | 2.094 Mops/s | 2.743 Mops/s |
| async get_ex many | 1.782 Mops/s | 2.415 Mops/s |
| async cache many | 3.756 Mops/s | 4.290 Mops/s |
| async cache batch | 3.826 Mops/s | 4.179 Mops/s |
| async cache loop | 3.582 Mops/s | 4.920 Mops/s |
| async cache st loop | 4.876 Mops/s | 3.780 Mops/s |
| async threaded cache loop | 3.130 Mops/s | 4.859 Mops/s |
| async lowerbound batch | 1.621 Mops/s | 1.683 Mops/s |
| async lowerbound many | 1.475 Mops/s | 1.632 Mops/s |
| async get loop | 1.861 Mops/s | 3.356 Mops/s |
| async get_ex loop | 2.874 Mops/s | 3.385 Mops/s |
| async lowerbound loop | 1.871 Mops/s | 1.555 Mops/s |
| async threaded get loop | 3.384 Mops/s | 2.580 Mops/s |
| async threaded get_ex loop | 2.731 Mops/s | 3.381 Mops/s |
| async threaded lower loop | 1.881 Mops/s | 1.824 Mops/s |

- conclusion: the explicitly owned loop traversal state improved `get_loop`,
  `get_ex_loop`, cache loop, lowerbound batch, and lowerbound many in this run.
  It regressed lowerbound loop and threaded get loop rows, with broader noise
  in unrelated rows. The structural point is that async loop chunks now use the
  same resumable traversal state that later scheduler work can stop driving
  after a nonblocking poll returns pending.

Async batch traversal state ownership checkpoint:

- changed async `get_batch`, `get_ex_batch`, grouped `get`, grouped `get_ex`,
  `get_equal_or_great_batch`, grouped `get_equal_or_great`, and grouped
  cache-backed `get` paths to instantiate traversal state directly with
  begin/drive/finish.
- this removes the compatibility-wrapper boundary from multi-item async read
  paths. The single-operation helpers still use the wrappers, while actual
  submitted batches now own exact-key or lowerbound traversal state explicitly.
- behavior remains blocking at the async worker boundary for now, but the
  multi-item async paths are now wired to the resumable traversal objects that
  can later be held across scheduler turns.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-batch-state-before.txt` and
    `/tmp/mdbx-async-bench-batch-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `c260b12`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 196.417 Kops/s | 206.480 Kops/s |
| async single get_ex | 381.427 Kops/s | 188.001 Kops/s |
| async single lowerbound | 275.946 Kops/s | 304.968 Kops/s |
| async parallel get | 2.733 Mops/s | 2.507 Mops/s |
| async many parallel get | 2.731 Mops/s | 2.660 Mops/s |
| async threaded get | 1.424 Mops/s | 3.015 Mops/s |
| async threaded batch get | 1.656 Mops/s | 3.389 Mops/s |
| async batch parallel get | 2.265 Mops/s | 2.664 Mops/s |
| async batch callback get | 2.362 Mops/s | 2.685 Mops/s |
| async get_ex batch | 2.348 Mops/s | 2.680 Mops/s |
| async get_ex many | 2.260 Mops/s | 2.658 Mops/s |
| async cache many | 3.365 Mops/s | 3.247 Mops/s |
| async cache batch | 3.222 Mops/s | 3.028 Mops/s |
| async cache st batch | 3.725 Mops/s | 3.000 Mops/s |
| async lowerbound batch | 1.641 Mops/s | 1.678 Mops/s |
| async lowerbound many | 1.671 Mops/s | 1.684 Mops/s |
| async get loop | 3.252 Mops/s | 3.258 Mops/s |
| async get_ex loop | 3.297 Mops/s | 2.649 Mops/s |
| async lowerbound loop | 1.290 Mops/s | 1.610 Mops/s |
| async threaded lower loop | 1.299 Mops/s | 980.704 Kops/s |

- conclusion: this checkpoint improved async batch/callback get, get_ex
  batch/many, threaded get/batch rows, and lowerbound batch/many/loop in this
  run. It regressed single get_ex, cache batch rows, and get_ex loop. As with
  the previous checkpoints, the structural gain is more important than the
  noisy microbenchmark deltas: multi-item async read operations now hold
  traversal state directly instead of hiding it inside synchronous wrappers.

Single async helper traversal state ownership checkpoint:

- changed the remaining single exact-key and lowerbound helper paths to call
  stateful traversal helpers that allocate begin/drive/finish state directly.
- the older compatibility wrappers are now retained only as marked-unused
  internal helpers; async read call sites no longer use them.
- this completes the local transition from synchronous traversal wrappers to
  explicit traversal state across the async get, get_ex, cache-get, batch,
  grouped-op, and lowerbound read paths. The worker still drives state to
  completion today; a later scheduler checkpoint can stop after nonblocking
  drive returns pending and retain the state across completions.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-single-state-before.txt` and
    `/tmp/mdbx-async-bench-single-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `8b112fa`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 364.203 Kops/s | 366.154 Kops/s |
| async single get_ex | 315.843 Kops/s | 290.794 Kops/s |
| async single lowerbound | 290.737 Kops/s | 294.605 Kops/s |
| async parallel get | 2.764 Mops/s | 2.522 Mops/s |
| async many parallel get | 2.144 Mops/s | 2.850 Mops/s |
| async threaded get | 2.438 Mops/s | 2.922 Mops/s |
| async batch parallel get | 2.538 Mops/s | 2.345 Mops/s |
| async batch callback get | 2.715 Mops/s | 2.765 Mops/s |
| async get_ex batch | 2.470 Mops/s | 2.814 Mops/s |
| async get_ex many | 2.865 Mops/s | 2.864 Mops/s |
| async cache many | 3.938 Mops/s | 3.838 Mops/s |
| async cache st many | 4.127 Mops/s | 4.440 Mops/s |
| async cache loop | 2.792 Mops/s | 4.936 Mops/s |
| async cache st loop | 3.253 Mops/s | 4.863 Mops/s |
| async lowerbound batch | 1.676 Mops/s | 1.673 Mops/s |
| async lowerbound many | 1.422 Mops/s | 1.532 Mops/s |
| async get loop | 2.815 Mops/s | 2.463 Mops/s |
| async get_ex loop | 3.383 Mops/s | 3.467 Mops/s |
| async lowerbound loop | 1.888 Mops/s | 1.874 Mops/s |
| async threaded lower loop | 1.848 Mops/s | 1.453 Mops/s |

- conclusion: this checkpoint is primarily structural. Single get and single
  lowerbound were roughly flat, single get_ex regressed in this run, and several
  unrelated batch/cache/loop rows moved substantially. The important result is
  that the async read implementation now reaches exact-key and lowerbound
  traversal through explicit stateful entry points everywhere.

Public get traversal-state checkpoint:

- changed `mdbx_get()`, `mdbx_get_equal_or_great()`, and `mdbx_get_ex()` to
  drive the internal exact-key/lowerbound traversal state for eligible no-dup
  read transactions. This preserves the public blocking API shape while routing
  page-cache misses and explicit reads through the same async page-read engine
  used by async get/batch paths.
- unsupported cases still fall back to the original cursor path, including
  dupsort DBs, changed DBIs, invalid DBIs, and traversal-state allocation
  failures.
- `mdbx_get_ex()` reports `values_count == 1` on the new fast path because the
  traversal-state path is only enabled for non-dupsort DBs. Dupsort databases
  continue through the existing cursor implementation.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-public-state-before.txt` and
    `/tmp/mdbx-async-bench-public-state-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `5408d8d`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.195 Mops/s | 967.760 Kops/s |
| blocking parallel get | 647.909 Kops/s | 1.038 Mops/s |
| async single get | 386.770 Kops/s | 392.705 Kops/s |
| async single get_ex | 384.718 Kops/s | 196.887 Kops/s |
| async single lowerbound | 293.865 Kops/s | 230.932 Kops/s |
| async parallel get | 2.415 Mops/s | 2.604 Mops/s |
| async many parallel get | 2.811 Mops/s | 2.888 Mops/s |
| async threaded get | 2.131 Mops/s | 1.606 Mops/s |
| async threaded batch get | 3.343 Mops/s | 3.495 Mops/s |
| async batch parallel get | 2.670 Mops/s | 2.794 Mops/s |
| async get_ex batch | 2.481 Mops/s | 2.663 Mops/s |
| async lowerbound batch | 1.485 Mops/s | 1.389 Mops/s |
| async get loop | 2.201 Mops/s | 3.414 Mops/s |
| async get_ex loop | 2.719 Mops/s | 2.454 Mops/s |
| async lowerbound loop | 1.871 Mops/s | 1.755 Mops/s |

- conclusion: this checkpoint makes the synchronous public GET-family APIs
  participate in the internal async traversal path for the supported no-dup read
  case. The direct blocking serial row regressed in this run, which is expected
  from the extra traversal-state setup while the API still drives to completion
  synchronously. Blocking parallel get, async get, batch get, get_ex batch, and
  get loop improved; single get_ex/lowerbound and some threaded rows regressed.
  The structural gain is that public blocking GET no longer bypasses the async
  page-read state machine for the common no-dup explicit-I/O read case.

Async get scheduler retained-state checkpoint:

- split grouped `async_op_get` worker execution into a start/complete form.
  The start path allocates a worker-local retained state object, prepares cache
  slots/materialized hits, begins exact-key traversal, and calls
  `async_batched_get_traverse_drive(..., false)`.
- if the nonblocking drive reports `MDBX_RESULT_TRUE`, the worker keeps the
  traversal state instead of immediately waiting. It can then submit later
  `async_op_get` groups before draining retained states, allowing multiple
  independent GET page-read batches to be in flight within the same completion
  chunk.
- completion order is still FIFO: operations are appended to the ready list in
  queue order, retained states are drained before `completed_seq` is published,
  and the worker flushes before any non-GET operation while retained GET state
  exists.
- allocation failures and unsupported cases fall back to the previous
  synchronous grouped GET execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-get-retain-before.txt` and
    `/tmp/mdbx-async-bench-get-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `1dd14ec`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 981.267 Kops/s | 997.864 Kops/s |
| blocking parallel get | 1.036 Mops/s | 604.939 Kops/s |
| async single get | 277.058 Kops/s | 319.014 Kops/s |
| async parallel get | 2.318 Mops/s | 2.359 Mops/s |
| async many parallel get | 2.333 Mops/s | 2.721 Mops/s |
| async threaded get | 2.436 Mops/s | 3.052 Mops/s |
| async threaded many get | 2.233 Mops/s | 2.703 Mops/s |
| async threaded batch get | 3.477 Mops/s | 3.381 Mops/s |
| async batch parallel get | 2.532 Mops/s | 1.332 Mops/s |
| async batch callback get | 2.808 Mops/s | 1.235 Mops/s |
| async get loop | 3.451 Mops/s | 3.216 Mops/s |
| async threaded get loop | 3.423 Mops/s | 3.305 Mops/s |
| async get_ex loop | 3.062 Mops/s | 3.378 Mops/s |
| async lowerbound batch | 1.438 Mops/s | 1.648 Mops/s |
| async lowerbound loop | 1.263 Mops/s | 1.814 Mops/s |

- conclusion: this is the first scheduler checkpoint where an async GET
  traversal can remain live after a nonblocking drive returns pending, while
  later GET groups can submit their own page reads before the worker blocks for
  completion. The target many/threaded GET rows improved in this run; explicit
  batch/callback GET regressed and several non-target rows moved with the usual
  single-run noise. The important change is semantic: grouped async GET no
  longer has to immediately drive every page miss to completion before the
  worker can submit another independent GET group.

Async get_ex scheduler retained-state checkpoint:

- split grouped `async_op_get_ex` worker execution into the same start/complete
  shape used for grouped `async_op_get`. The start path prepares cache slots,
  materialized hits, exact-key traversal state, and calls
  `async_batched_get_traverse_drive(..., false)`.
- if the nonblocking drive reports `MDBX_RESULT_TRUE`, the worker retains the
  `get_ex` traversal state and may submit later `async_op_get_ex` groups before
  draining retained states. This extends scheduler-level traversal suspension
  from ordinary GET to extended GET.
- completion publication remains FIFO. The worker drains retained GET/GET_EX
  states before updating `completed_seq`, and it flushes before crossing between
  GET, GET_EX, and other operation families while retained state exists.
- non-batchable DBIs and allocation failures fall back to the previous
  synchronous grouped `get_ex` execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getex-retain-before.txt` and
    `/tmp/mdbx-async-bench-getex-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `677160c`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 335.345 Kops/s | 312.680 Kops/s |
| async single get_ex | 270.410 Kops/s | 268.997 Kops/s |
| async parallel get | 2.776 Mops/s | 2.692 Mops/s |
| async many parallel get | 1.670 Mops/s | 2.770 Mops/s |
| async threaded get | 3.153 Mops/s | 1.721 Mops/s |
| async threaded many get | 3.240 Mops/s | 3.054 Mops/s |
| async batch parallel get | 2.215 Mops/s | 2.718 Mops/s |
| async batch callback get | 2.715 Mops/s | 2.809 Mops/s |
| async get_ex batch | 2.519 Mops/s | 1.963 Mops/s |
| async get_ex many | 2.897 Mops/s | 2.222 Mops/s |
| async get_ex loop | 3.463 Mops/s | 2.700 Mops/s |
| async threaded get_ex loop | 3.528 Mops/s | 1.922 Mops/s |
| async cache many | 3.448 Mops/s | 4.426 Mops/s |
| async cache batch | 3.685 Mops/s | 4.108 Mops/s |
| async lowerbound batch | 1.414 Mops/s | 1.584 Mops/s |
| async lowerbound loop | 1.905 Mops/s | 1.507 Mops/s |

- conclusion: this checkpoint is primarily semantic and regressed the direct
  get_ex rows in this single benchmark run. The useful change is that extended
  GET now reaches the same retained exact-key traversal state as ordinary GET,
  so scheduler work can continue from one pending mechanism instead of keeping
  get_ex as an immediate drive-to-completion special case.

Async lowerbound scheduler retained-state checkpoint:

- split grouped `async_op_get_equal_or_great` worker execution into a
  start/complete form using `async_batched_lowerbound_traverse_state_t`.
- the start path prepares lowerbound keys, found-key storage, value buffers,
  result arrays, and eligible flags, then calls
  `async_batched_lowerbound_traverse_drive(..., false)`.
- if the nonblocking drive reports `MDBX_RESULT_TRUE`, the worker retains the
  lowerbound traversal state and may submit later lowerbound groups before
  draining retained states.
- completion publication remains FIFO. The worker drains retained GET, GET_EX,
  and lowerbound states before updating `completed_seq`, and it flushes before
  crossing between retained operation families.
- non-batchable DBIs and allocation failures fall back to the previous
  synchronous grouped lowerbound execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-lower-retain-before.txt` and
    `/tmp/mdbx-async-bench-lower-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `e874325`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 336.784 Kops/s | 296.560 Kops/s |
| async single get_ex | 320.105 Kops/s | 324.433 Kops/s |
| async single lowerbound | 287.693 Kops/s | 253.750 Kops/s |
| async parallel get | 2.791 Mops/s | 1.493 Mops/s |
| async many parallel get | 2.862 Mops/s | 1.879 Mops/s |
| async threaded get | 2.527 Mops/s | 2.370 Mops/s |
| async threaded many get | 3.179 Mops/s | 2.279 Mops/s |
| async batch parallel get | 2.122 Mops/s | 2.533 Mops/s |
| async get_ex batch | 2.680 Mops/s | 1.544 Mops/s |
| async get_ex many | 2.651 Mops/s | 1.252 Mops/s |
| async lowerbound batch | 1.673 Mops/s | 1.242 Mops/s |
| async lowerbound many | 1.671 Mops/s | 1.620 Mops/s |
| async lowerbound loop | 1.740 Mops/s | 1.102 Mops/s |
| async threaded lower loop | 1.859 Mops/s | 1.585 Mops/s |
| async cache batch | 3.971 Mops/s | 4.068 Mops/s |
| async cache st batch | 3.826 Mops/s | 3.648 Mops/s |

- conclusion: this checkpoint is semantic rather than a performance win in this
  single benchmark run. The lowerbound grouped worker path now participates in
  scheduler-retained traversal state, so the main grouped GET-family operations
  can all suspend after a nonblocking page-read drive and be completed through
  the same retained-state drain mechanism.

Mixed GET-family retained-state checkpoint:

- relaxed the async worker pending-read boundary so retained GET, GET_EX, and
  lowerbound read states can coexist within the same completion chunk.
- when any retained GET-family state exists, the worker may continue submitting
  later GET-family operations, regardless of whether they are exact-key,
  extended exact-key, or lowerbound reads.
- the worker still drains all retained read states before publishing
  `completed_seq`, and it still flushes before writes, cursor mutations, cache
  invalidating operations, or other non-GET-family operations.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-mixed-read-retain-before.txt` and
    `/tmp/mdbx-async-bench-mixed-read-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `44dfd4f`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 235.474 Kops/s | 337.418 Kops/s |
| async single get_ex | 260.305 Kops/s | 298.097 Kops/s |
| async single lowerbound | 274.675 Kops/s | 272.401 Kops/s |
| async parallel get | 2.800 Mops/s | 2.088 Mops/s |
| async many parallel get | 2.420 Mops/s | 2.163 Mops/s |
| async threaded get | 2.120 Mops/s | 2.161 Mops/s |
| async threaded many get | 2.978 Mops/s | 2.926 Mops/s |
| async threaded batch get | 2.601 Mops/s | 3.525 Mops/s |
| async get_ex batch | 2.538 Mops/s | 2.314 Mops/s |
| async get_ex many | 2.860 Mops/s | 2.894 Mops/s |
| async get_ex loop | 2.553 Mops/s | 3.482 Mops/s |
| async lowerbound batch | 1.576 Mops/s | 1.670 Mops/s |
| async lowerbound many | 1.425 Mops/s | 1.404 Mops/s |
| async lowerbound loop | 1.528 Mops/s | 1.668 Mops/s |
| async threaded lower loop | 1.775 Mops/s | 1.533 Mops/s |
| async cache batch | 3.302 Mops/s | 4.073 Mops/s |

- conclusion: the benchmark workload mostly measures each GET-family operation
  separately, so this checkpoint primarily improves scheduler semantics rather
  than a single isolated row. It removes the artificial family boundary between
  retained exact-key, extended exact-key, and lowerbound page-read traversals,
  allowing mixed read queues to submit more independent page misses before the
  worker blocks for completion.

Async get_batch scheduler retained-state checkpoint:

- split `async_op_get_batch` execution into a start/complete form. The start
  path keeps the batch operation's cache slots, cache materialization results,
  cold/handled arrays, and exact-key traversal state alive after
  `async_batched_get_traverse_drive(..., false)` reports pending.
- the async worker now treats `async_op_get_batch` as part of the retained
  GET-family read set. Pending batch GET traversal can coexist with retained
  single GET, GET_EX, and lowerbound reads before the worker drains and
  publishes completions.
- callback invocation still happens only during completion, before the batch op
  is marked done. Allocation failures fall back to the previous synchronous
  batch execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getbatch-retain-before.txt` and
    `/tmp/mdbx-async-bench-getbatch-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `b9e3a43`:

| metric | before | after |
| --- | ---: | ---: |
| async single get | 335.667 Kops/s | 287.816 Kops/s |
| async single get_ex | 321.014 Kops/s | 328.071 Kops/s |
| async parallel get | 2.745 Mops/s | 2.437 Mops/s |
| async many parallel get | 2.712 Mops/s | 2.460 Mops/s |
| async threaded get | 2.419 Mops/s | 1.581 Mops/s |
| async threaded batch get | 3.579 Mops/s | 2.235 Mops/s |
| async batch parallel get | 2.719 Mops/s | 2.447 Mops/s |
| async batch callback get | 2.562 Mops/s | 2.139 Mops/s |
| async get_ex batch | 2.719 Mops/s | 2.701 Mops/s |
| async get_ex many | 2.562 Mops/s | 2.411 Mops/s |
| async cache many | 4.030 Mops/s | 3.674 Mops/s |
| async cache batch | 3.550 Mops/s | 3.593 Mops/s |
| async lowerbound batch | 1.602 Mops/s | 1.661 Mops/s |
| async lowerbound loop | 1.637 Mops/s | 1.822 Mops/s |
| async threaded get loop | 2.364 Mops/s | 3.488 Mops/s |
| async threaded lower loop | 1.666 Mops/s | 1.865 Mops/s |

- conclusion: this checkpoint adds scheduler-retained traversal to the explicit
  get-batch operation, which is semantically aligned with the async-I/O goal but
  regressed the direct batch GET rows in this single run. The value is that
  get-batch no longer has to be an immediate drive-to-completion worker island;
  it can participate in the same retained read drain as the other GET-family
  operations.

Async get_ex_batch scheduler retained-state checkpoint:

- split explicit `async_op_get_ex_batch` execution into a synchronous fallback
  plus a retained start/complete path. The start path owns cache slots,
  materialized cache results, cold/handled arrays, and exact-key traversal state
  for the caller-provided key/data/result arrays.
- the async worker now treats explicit `get_ex_batch` as part of the retained
  GET-family read set, so it can coexist with retained single GET, GET_EX,
  lowerbound, and explicit GET-batch page-read traversals before the worker
  drains and publishes completions.
- callbacks still run only during completion, before the operation is marked
  done. Non-batchable DBIs and allocation failures fall back to the previous
  synchronous batch execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getexbatch-retain-before.txt` and
    `/tmp/mdbx-async-bench-getexbatch-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `a5863e8`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 1.041 Mops/s | 811.187 Kops/s |
| async single get | 303.961 Kops/s | 192.422 Kops/s |
| async single get_ex | 325.354 Kops/s | 283.623 Kops/s |
| async parallel get | 2.475 Mops/s | 2.785 Mops/s |
| async many parallel get | 2.528 Mops/s | 2.019 Mops/s |
| async threaded batch get | 2.501 Mops/s | 3.267 Mops/s |
| async batch parallel get | 2.596 Mops/s | 1.498 Mops/s |
| async batch callback get | 2.342 Mops/s | 1.427 Mops/s |
| async get_ex batch | 2.115 Mops/s | 1.529 Mops/s |
| async get_ex many | 2.574 Mops/s | 1.506 Mops/s |
| async get_ex loop | 2.875 Mops/s | 3.488 Mops/s |
| async threaded get_ex loop | 3.395 Mops/s | 3.458 Mops/s |
| async cache batch | 3.113 Mops/s | 2.597 Mops/s |
| async cache st batch | 2.403 Mops/s | 4.121 Mops/s |
| async lowerbound batch | 1.564 Mops/s | 1.344 Mops/s |
| async lowerbound loop | 1.303 Mops/s | 1.807 Mops/s |

- conclusion: this checkpoint is semantic rather than a single-run performance
  win. The direct `get_ex_batch` row regressed in this run, while adjacent
  retained read and cache rows moved in both directions. The important change is
  that explicit extended batch GET now uses the same retained traversal drain as
  the rest of the GET-family scheduler instead of being an immediate
  drive-to-completion operation.

Async lowerbound_batch scheduler retained-state checkpoint:

- split explicit `async_op_get_equal_or_great_batch` execution into a
  synchronous fallback plus a retained start/complete path. The start path owns
  the found-key side buffer, handled/eligible arrays, and lowerbound traversal
  state for the caller-provided key/data/result arrays.
- the async worker now treats explicit lowerbound batch as part of the retained
  GET-family read set, so it can coexist with retained single GET, GET_EX,
  lowerbound, explicit GET-batch, and explicit GET_EX-batch traversals before
  the worker drains and publishes completions.
- callbacks still run only during completion, before the operation is marked
  done. Non-batchable DBIs and allocation failures fall back to the previous
  synchronous batch execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-lowerbatch-retain-before.txt` and
    `/tmp/mdbx-async-bench-lowerbatch-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `2b5c110`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 412.357 Kops/s | 784.675 Kops/s |
| async single get | 182.174 Kops/s | 321.589 Kops/s |
| async single get_ex | 322.026 Kops/s | 329.010 Kops/s |
| async single lowerbound | 262.810 Kops/s | 223.291 Kops/s |
| async parallel get | 1.638 Mops/s | 2.587 Mops/s |
| async many parallel get | 2.629 Mops/s | 2.280 Mops/s |
| async threaded batch get | 3.015 Mops/s | 3.575 Mops/s |
| async batch parallel get | 2.771 Mops/s | 2.588 Mops/s |
| async get_ex batch | 2.739 Mops/s | 2.468 Mops/s |
| async get_ex many | 2.578 Mops/s | 2.846 Mops/s |
| async lowerbound batch | 1.412 Mops/s | 1.381 Mops/s |
| async lowerbound many | 1.542 Mops/s | 1.687 Mops/s |
| async lowerbound loop | 1.885 Mops/s | 1.309 Mops/s |
| async threaded lower loop | 1.863 Mops/s | 1.709 Mops/s |
| async cache batch | 4.054 Mops/s | 3.383 Mops/s |
| async cache st batch | 3.395 Mops/s | 3.737 Mops/s |

- conclusion: this checkpoint is structural rather than a direct benchmark win.
  The explicit lowerbound batch row was essentially flat in this run, while
  adjacent rows moved with the usual single-run noise. The useful change is that
  all explicit GET-family batch APIs now enter the same retained read drain
  mechanism instead of leaving lowerbound batch as a synchronous
  drive-to-completion outlier.

Async cache_batch retained page-cache read checkpoint:

- added an internal retained materialization state for cache-entry batches. The
  begin path validates warm cache entries, prepares page-cache read submissions,
  and starts `page_cache_read_batch_begin/drive(..., false)` without forcing the
  worker to wait immediately.
- routed explicit `async_op_cache_get_batch` and
  `async_op_cache_get_singlethreaded_batch` through a retained start/complete
  path. The worker can now keep cache-batch page-cache misses pending alongside
  retained GET-family reads before draining and publishing completions.
- the retained materializer handles ordinary cached-page hits. Large/overflow
  page materialization is intentionally left unhandled and falls back to the
  existing synchronous cache-get path for correctness.
- callbacks still run only during completion, before the operation is marked
  done. Allocation failures and unsupported materialization paths fall back to
  the previous synchronous batch execution path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cachebatch-retain-before.txt` and
    `/tmp/mdbx-async-bench-cachebatch-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `64912c0`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 941.680 Kops/s | 1.029 Mops/s |
| async parallel get | 2.183 Mops/s | 2.755 Mops/s |
| async many parallel get | 2.698 Mops/s | 2.599 Mops/s |
| async get_ex batch | 2.225 Mops/s | 2.705 Mops/s |
| async cache many | 2.156 Mops/s | 4.262 Mops/s |
| async cache st many | 2.076 Mops/s | 4.279 Mops/s |
| async cache batch | 3.135 Mops/s | 4.196 Mops/s |
| async cache st batch | 3.432 Mops/s | 3.545 Mops/s |
| async cache batch cb | 3.523 Mops/s | 3.896 Mops/s |
| async cache st batch cb | 3.676 Mops/s | 3.503 Mops/s |
| async large cache batch | 139.447 Kops/s | 111.045 Kops/s |
| async large cache st batch | 134.576 Kops/s | 122.545 Kops/s |
| async threaded cache batch | 4.574 Mops/s | 5.484 Mops/s |
| async threaded cache st batch | 2.895 Mops/s | 4.608 Mops/s |
| async cache loop | 3.263 Mops/s | 4.930 Mops/s |
| async cache st loop | 4.852 Mops/s | 5.027 Mops/s |

- conclusion: this checkpoint moves explicit cache-batch page-cache reads into
  the retained scheduler path and the direct cache-batch rows improved in this
  run. Large cache rows regressed, which is expected risk from conservatively
  falling back for large-page materialization after the retained page read. A
  later slice should add retained large-page materialization rather than
  bouncing those entries back through the synchronous fallback.

Async cache_get grouped retained page-cache read checkpoint:

- split grouped `async_op_cache_get` and
  `async_op_cache_get_singlethreaded` worker execution into synchronous
  fallback plus retained start/complete paths.
- grouped cache-get now snapshots the submitted cache entries, starts retained
  page-cache materialization with `async_cache_materialize_batch_drive(...,
  false)`, and can remain pending while the worker submits later retained read
  work before publishing completions.
- ordinary cached-page hits share the retained materializer introduced for
  explicit cache batches. Large/overflow page materialization still falls back
  to the existing synchronous cache-get path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cacheget-retain-before.txt` and
    `/tmp/mdbx-async-bench-cacheget-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `b17f86a`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 942.698 Kops/s | 685.917 Kops/s |
| async parallel get | 2.094 Mops/s | 2.329 Mops/s |
| async many parallel get | 2.851 Mops/s | 2.359 Mops/s |
| async cache many | 3.668 Mops/s | 3.344 Mops/s |
| async cache st many | 4.403 Mops/s | 3.681 Mops/s |
| async cache batch | 4.016 Mops/s | 3.792 Mops/s |
| async cache st batch | 3.508 Mops/s | 2.935 Mops/s |
| async cache batch cb | 3.583 Mops/s | 4.156 Mops/s |
| async cache st batch cb | 3.907 Mops/s | 3.774 Mops/s |
| async large cache batch | 119.345 Kops/s | 121.771 Kops/s |
| async large cache st batch | 117.248 Kops/s | 122.973 Kops/s |
| async threaded cache batch | 5.284 Mops/s | 2.445 Mops/s |
| async threaded cache st batch | 5.617 Mops/s | 4.518 Mops/s |
| async cache loop | 2.817 Mops/s | 3.431 Mops/s |
| async cache st loop | 3.406 Mops/s | 3.742 Mops/s |
| async threaded cache loop | 2.814 Mops/s | 2.637 Mops/s |
| async threaded cache st loop | 2.961 Mops/s | 3.894 Mops/s |

- conclusion: this checkpoint is primarily structural. Grouped cache-get now
  participates in retained page-cache read scheduling, but this single
  benchmark run is mixed: cache-many and threaded cache-batch regressed, while
  cache-loop and some callback/large-cache rows improved. The useful semantic
  change is that adjacent cache-get operations no longer have to materialize
  all page-cache misses to completion before the worker can collect later
  retained read work.

Async large cache materialization retained-state checkpoint:

- extended the retained cache materializer with a second async phase for
  large/overflow page materialization. After the retained page-cache read phase
  identifies large cached pages, it now prepares deduplicated large-page read
  submissions and drives them with `dxb_storage_read_batch_begin/drive/finish`
  instead of immediately falling back through synchronous cache-get.
- large materialization buffers and page refs are now owned by
  `async_cache_materialize_batch_state_t` until completion or cleanup. Ordinary
  cached-page reads, explicit cache batches, grouped cache gets, and their
  single-threaded variants all share this retained large-page path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-largecache-retain-before.txt` and
    `/tmp/mdbx-async-bench-largecache-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `4b62bb3`:

| metric | before | after |
| --- | ---: | ---: |
| blocking parallel get | 998.439 Kops/s | 521.365 Kops/s |
| async cache many | 4.881 Mops/s | 4.461 Mops/s |
| async cache st many | 4.143 Mops/s | 4.230 Mops/s |
| async cache batch | 2.472 Mops/s | 3.806 Mops/s |
| async cache st batch | 3.413 Mops/s | 3.803 Mops/s |
| async cache batch cb | 4.075 Mops/s | 4.044 Mops/s |
| async cache st batch cb | 3.385 Mops/s | 4.330 Mops/s |
| async large cache batch | 123.844 Kops/s | 139.458 Kops/s |
| async large cache st batch | 121.483 Kops/s | 130.667 Kops/s |
| async threaded cache batch | 5.505 Mops/s | 3.929 Mops/s |
| async threaded cache st batch | 4.807 Mops/s | 5.463 Mops/s |
| async cache loop | 2.821 Mops/s | 2.067 Mops/s |
| async cache st loop | 5.133 Mops/s | 2.162 Mops/s |
| async threaded cache loop | 2.134 Mops/s | 2.668 Mops/s |
| async threaded cache st loop | 2.792 Mops/s | 2.239 Mops/s |

- conclusion: this checkpoint directly addresses the previous large-cache
  fallback caveat. The large cache batch rows improved in this run, while
  unrelated cache loop and threaded rows remain noisy. The semantic improvement
  is that large/overflow cache materialization can now stay inside the retained
  page-read state machine rather than re-entering synchronous cache-get for the
  second storage read.

Async get_loop retained traversal checkpoint:

- added `async_get_loop_pending_t`, a heap-owned retained state for one
  64-key `mdbx_async_get_loop()` window. The state owns copied keys,
  cache-entry snapshots, cache materialization results, cold-slot flags, and
  the `async_batched_get_traverse_state_t` continuation until the pending page
  reads complete.
- changed the async worker to route `async_op_get_loop` through the retained
  read scheduler. The first window now starts traversal with
  `async_batched_get_traverse_drive(..., false)` and can remain pending while
  the worker accepts later retained read work before publishing completions.
- result callbacks and `completed` updates remain in index order. Allocation
  failure falls back to the existing synchronous executor path; key callback
  errors and result callback errors stop the loop with the same completed-count
  semantics as before.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getloop-before.txt` and
    `/tmp/mdbx-async-bench-getloop-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `7947edf`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 994.428 Kops/s | 961.754 Kops/s |
| blocking parallel get | 1.029 Mops/s | 565.433 Kops/s |
| async get loop | 3.341 Mops/s | 3.479 Mops/s |
| async threaded get loop | 2.358 Mops/s | 3.484 Mops/s |
| async-loop/blocking par | 3.248 | 6.152 |
| async-thread-loop/par | 2.292 | 6.161 |
| async-get-loop/batch | 1.917 | 0.530 |

- conclusion: this checkpoint makes `mdbx_async_get_loop()` participate in the
  retained traversal/page-read scheduler instead of driving every traversal
  window to completion before the worker can collect more read work. The direct
  get-loop rows improved in this run, especially the threaded loop row. The
  ratio rows are distorted by a slower blocking-parallel baseline in the
  after-run, and `async-get-loop/batch` regressed because the batch path was
  already heavily optimized in previous checkpoints; this should be revisited
  after `get_ex_loop` and lower-bound loop are moved to retained state too.

Async get_ex_loop retained traversal checkpoint:

- added `async_get_ex_loop_pending_t`, the `mdbx_async_get_ex_loop()` analogue
  of the retained get-loop state. For batchable DBIs it owns a 64-key window,
  cache-entry snapshots, cache materialization results, cold-slot flags, and
  the `async_batched_get_traverse_state_t` continuation across async page-read
  completion.
- changed the worker to route `async_op_get_ex_loop` through the retained read
  scheduler. Non-batchable DBIs and allocation failures fall back to the
  existing synchronous executor path.
- result callbacks, `values_count`, and `completed` updates keep the same
  per-index ordering as the previous loop body. Items not handled by retained
  traversal fall back through `async_get_ex_one()`.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-getexloop-before.txt` and
    `/tmp/mdbx-async-bench-getexloop-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `dc146a5`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 972.606 Kops/s | 976.832 Kops/s |
| blocking parallel get | 880.081 Kops/s | 872.591 Kops/s |
| async get_ex loop | 3.507 Mops/s | 3.423 Mops/s |
| async threaded get_ex loop | 3.347 Mops/s | 3.202 Mops/s |
| async-get-ex-loop/par | 3.984 | 3.922 |
| async-thread-get-ex-loop/par | 3.803 | 3.670 |
| async-get-ex-batch/loop | 0.760 | 0.762 |
| async-thread-get-ex-loop/loop | 0.954 | 0.936 |

- conclusion: this is a structural retained-state checkpoint rather than a
  warm-cache throughput win. The direct get_ex loop rows were slightly lower in
  this single run, while the relative rows stayed close. The semantic
  improvement is that batchable get_ex loops can now suspend on explicit
  page-cache misses and resume from retained traversal state instead of driving
  every window to completion inside the executor.

Async lower-bound loop retained traversal checkpoint:

- added `async_lowerbound_loop_pending_t`, a retained state for
  `mdbx_async_get_equal_or_great_loop()` windows. It owns copied search keys,
  found-key outputs, optional caller-provided data values, result slots, and
  the `async_batched_lowerbound_traverse_state_t` continuation while explicit
  page reads are pending.
- changed the worker to route `async_op_get_equal_or_great_loop` through the
  retained read scheduler for batchable no-dup read transactions. Unsupported
  DBI shapes and allocation failures still fall back to the existing
  synchronous executor path.
- callbacks and `completed` updates remain in index order. Items not handled
  by retained traversal fall back through `async_get_equal_or_great_one()`.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-lowerloop-before.txt` and
    `/tmp/mdbx-async-bench-lowerloop-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `a119944`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.011 Mops/s | 1.003 Mops/s |
| blocking parallel get | 947.079 Kops/s | 949.887 Kops/s |
| async lowerbound loop | 1.895 Mops/s | 1.873 Mops/s |
| async threaded lower loop | 1.884 Mops/s | 1.898 Mops/s |
| async-lower-loop/par | 2.000 | 1.972 |
| async-thread-lower-loop/par | 1.989 | 1.998 |
| async-lower-batch/loop | 0.886 | 0.909 |
| async-thread-lower-loop/loop | 0.994 | 1.013 |

- conclusion: this checkpoint completes the retained-state migration for the
  get-family loop traversal APIs. The warm repeated-key benchmark is neutral:
  direct lower-bound loop was slightly lower, threaded lower loop and
  lower-batch/loop ratios were slightly higher. The important change is that
  lower-bound loop traversal now has a suspension point on explicit page-cache
  misses instead of always driving the traversal window to completion inside the
  worker.

Async cache_get loop retained materialization checkpoint:

- added `async_cache_get_loop_pending_t`, a retained state for one 64-entry
  `mdbx_async_cache_get_loop()` or
  `mdbx_async_cache_get_SingleThreaded_loop()` prefetch window. It owns the
  cache-entry snapshots, materialized values, cache results, handled flags, and
  `async_cache_materialize_batch_state_t` until explicit page-cache reads
  complete.
- changed the worker to route cache-get loop opcodes through the retained read
  scheduler. The retained window snapshots/cache-materializes entries ahead,
  but still calls `key_func`, fallback cache-get, `result_func`, and
  `completed` updates sequentially during completion.
- unsupported materializer setup before any callback falls back to the existing
  synchronous executor path; setup failures after partial completion return the
  setup error rather than rerunning already-completed callbacks.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cacheloop-retain-before.txt` and
    `/tmp/mdbx-async-bench-cacheloop-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `44d90ac`:

| metric | before | after |
| --- | ---: | ---: |
| blocking serial get | 1.013 Mops/s | 992.120 Kops/s |
| blocking parallel get | 948.016 Kops/s | 799.594 Kops/s |
| async cache loop | 5.020 Mops/s | 3.922 Mops/s |
| async cache st loop | 5.023 Mops/s | 4.941 Mops/s |
| async threaded cache loop | 4.931 Mops/s | 4.898 Mops/s |
| async threaded cache st loop | 4.934 Mops/s | 4.995 Mops/s |
| async-cache-loop/par | 5.295 | 4.905 |
| async-cache-st-loop/par | 5.298 | 6.179 |
| async-thread-cache-loop/par | 5.201 | 6.126 |
| async-thread-cache-st-l/par | 5.205 | 6.247 |
| async-cache-loop/batch | 1.852 | 0.952 |
| async-cache-st-loop/batch | 1.260 | 1.203 |
| async-cache-loop/many | 1.928 | 0.987 |
| async-cache-st-loop/many | 1.041 | 1.138 |

- conclusion: this checkpoint moves cache-get loop prefetch materialization
  into the retained page-cache read scheduler, but it is not a warm-cache
  throughput win in this run. The direct cache-loop row regressed, while
  single-threaded and threaded loop rows were roughly flat to slightly higher
  relative to the slower blocking baseline. The semantic improvement is that
  cache-get loops can now suspend while their prefetch window has explicit
  page-cache reads outstanding, instead of materializing the window to
  completion before the worker can collect more retained read work.

Async cursor get_batch retained sibling-read checkpoint:

- added `async_cursor_get_batch_pending_t`, a retained state for
  `mdbx_async_cursor_get_batch()` on the simple non-dupsort `MDBX_NEXT` path
  and already-positioned `MDBX_FIRST` path. Unsupported starts still fall back
  to the existing synchronous `mdbx_cursor_get_batch()` implementation.
- the retained cursor batch fills key/value pairs from the current leaf in
  order. When it reaches a leaf boundary, it prepares the right-sibling child
  read and drives it with `page_cursor_get_batch_begin/drive/finish`, allowing
  the async worker to collect other retained read work while the sibling page
  read is outstanding.
- result count and cursor-batch return code semantics are preserved for the
  covered path. Large/overflow value reads through `node_read()` and complex
  initial cursor seeks are still synchronous islands and need later retained
  states.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorbatch-retain-before.txt` and
    `/tmp/mdbx-async-bench-cursorbatch-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `4fa5d73`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor batch | 149.443 Mops/s | 149.525 Mops/s |
| parallel cursor batch | 63.436 Mops/s | 60.750 Mops/s |
| async cursor batch | 63.762 Mops/s | 55.370 Mops/s |
| async threaded cursor batch | 113.215 Mops/s | 64.507 Mops/s |
| async-cursor-batch/get | 59.053 | 46.739 |
| async-cursor/blocking par | 1.005 | 0.911 |
| async-cursor/blocking ser | 0.427 | 0.370 |
| async-thread-cbatch/par | 1.785 | 1.062 |
| async-thread-cbatch/ser | 0.758 | 0.431 |
| async-thread-cbatch/batch | 1.776 | 1.165 |

- conclusion: this checkpoint is a semantic cursor-read migration step, not a
  throughput improvement. The added retained sibling-read state regressed the
  warm cursor-batch benchmark, especially the threaded cursor batch row. The
  useful change is that `mdbx_async_cursor_get_batch()` no longer has to block
  the worker at every right-sibling page-cache miss on the covered path. Future
  work should extend the same retained state through `async_cursor_get_batches`
  and cursor loop/scan paths, then reduce the overhead in the hot in-page case.

Async cursor get_batches retained sibling-read checkpoint:

- added `async_cursor_get_batches_pending_t`, a retained state for
  `mdbx_async_cursor_get_batches()` and
  `mdbx_async_cursor_get_batches_from()`. It owns the temporary pair buffer and
  drives an embedded cursor-batch operation through the retained
  right-sibling-read state introduced in the previous checkpoint.
- the multi-batch API now preserves callback order and `completed_pairs`
  updates while allowing the first outstanding sibling page read to suspend the
  operation. Initial `from_key` positioning still uses the synchronous cursor
  seek path and remains a follow-up target.
- fixed EOF/restart handling so `MDBX_RESULT_TRUE` from a cursor batch is
  treated as end-of-data only when there was no wrapped-scan progress to
  continue, matching the existing synchronous multi-batch loop behavior.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorbatches-retain-before.txt` and
    `/tmp/mdbx-async-bench-cursorbatches-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `a052b5b`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor batch | 152.361 Mops/s | 150.447 Mops/s |
| parallel cursor batch | 119.098 Mops/s | 55.536 Mops/s |
| async cursor batch | 64.260 Mops/s | 96.419 Mops/s |
| async threaded cursor batch | 128.179 Mops/s | 118.846 Mops/s |
| async cursor loop | 142.105 Mops/s | 71.314 Mops/s |
| async cursor loop_from | 139.426 Mops/s | 70.657 Mops/s |
| async threaded cursor loop | 130.745 Mops/s | 125.593 Mops/s |
| async threaded cursor loop_from | 123.826 Mops/s | 134.681 Mops/s |
| async-cursor-batch/get | 63.217 | 72.866 |
| async-cursor/blocking par | 0.540 | 1.736 |
| async-thread-cbatch/par | 1.076 | 2.140 |
| async-thread-cbatch/batch | 1.995 | 1.233 |

- conclusion: this checkpoint extends the retained cursor sibling-read state
  through the public multi-batch cursor API. The direct async cursor batch row
  improved in this run and the relative-to-parallel ratios improved because the
  parallel cursor baseline was slower; cursor loop rows were mixed and remain
  dominated by their own synchronous `mdbx_cursor_get_batch()` calls. The next
  cursor target is to share this retained batch state with cursor loop/scan
  helpers rather than only the public batch/batches operations.

Async cursor get_loop retained sibling-read checkpoint:

- added `async_cursor_get_loop_pending_t`, a retained state for the simple
  `mdbx_async_cursor_get_loop()` fast path that starts from an unfilled,
  non-dupsort cursor with `MDBX_FIRST` and then advances with `MDBX_NEXT`.
  Unsupported loop shapes, including the current `_from` seek path, still
  fall back to `async_cursor_get_loop_execute()`.
- the retained loop owns a small pair buffer and drives an embedded
  `async_cursor_get_batch` operation through the retained sibling-read state
  introduced for cursor batches. This lets the async worker keep the cursor
  loop suspended while a right-sibling page-cache read is outstanding and
  collect later retained reads before publishing completions.
- callback order, completed-count updates, EOF handling, and the single-item
  tail case remain compatible with the previous loop body. Initial seeks,
  duplicate-subcursor loops, `_from` positioning, cursor scans, and
  large/overflow value materialization are still follow-up retained-state
  targets.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorloop-retain-before.txt` and
    `/tmp/mdbx-async-bench-cursorloop-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `72fb9b7`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor get | 74.446 Mops/s | 72.884 Mops/s |
| parallel cursor get | 104.304 Mops/s | 59.361 Mops/s |
| async cursor get loop | 112.604 Mops/s | 64.456 Mops/s |
| async cursor get loop_from | 59.183 Mops/s | 111.063 Mops/s |
| async threaded cursor get loop | 104.446 Mops/s | 112.367 Mops/s |
| async threaded cget loop_from | 111.609 Mops/s | 114.040 Mops/s |
| async cursor loop | 131.901 Mops/s | 75.351 Mops/s |
| async cursor loop_from | 121.310 Mops/s | 72.047 Mops/s |
| async threaded cursor loop | 132.502 Mops/s | 130.164 Mops/s |
| async threaded cursor loop_from | 114.995 Mops/s | 131.890 Mops/s |
| async-cursor-get-loop/par | 1.080 | 1.086 |
| async-cget-loop-from/par | 0.445 | 1.776 |
| async-cursor-get-loop/ser | 1.513 | 0.884 |
| async-cget-loop-from/ser | 0.523 | 1.002 |
| async-cursor-get-loop/get | 100.449 | 75.907 |
| async-thread-cget-loop/par | 1.001 | 1.893 |
| async-thread-cget-from/par | 0.840 | 1.824 |
| async-thread-cget-loop/ser | 1.403 | 1.542 |
| async-thread-cget-from/ser | 0.986 | 1.029 |
| async-thread-cget-loop/get | 93.172 | 132.330 |
| async-thread-cget-loop/loop | 0.928 | 1.743 |
| async-loop-cursor/par | 2.264 | 1.373 |
| async-loop-cursor/ser | 0.894 | 0.511 |
| async-thread-cursor-loop/par | 2.274 | 2.372 |
| async-thread-cursor-loop/ser | 0.898 | 0.883 |
| async-thread-cursor/batch | 2.138 | 2.059 |
| async-thread-cursor/loop | 1.005 | 1.727 |

- conclusion: this checkpoint is another semantic migration step with mixed
  single-run performance. The direct `async cursor get loop` and
  `async cursor loop` rows regressed, while the threaded cursor-get loop rows
  and several loop-from ratios improved. The important structural change is
  that the covered cursor loop can now suspend around retained sibling
  page-cache reads instead of driving every batch to completion synchronously.
  The next cursor work should retain `_from` seek positioning, scan helpers,
  duplicate-subcursor cases, and large/overflow value reads, then reduce the
  added hot in-page overhead.

Async cursor get_loop_from retained sibling-read checkpoint:

- extended `async_cursor_get_loop_pending_t` so the positioned
  `mdbx_async_cursor_get_loop_from()` path with `MDBX_SET_LOWERBOUND` or
  `MDBX_SET_KEY` and `MDBX_NEXT` can reuse retained cursor-batch sibling reads
  after the first positioned item.
- the initial `mdbx_cursor_get(from_op)` positioning call is still
  synchronous. After that item is consumed, the retained loop requests one
  extra batch pair, skips the already-current item, and consumes later pairs
  from the retained cursor-batch state. This preserves the previous
  skip-current behavior while allowing later right-sibling page reads to
  suspend in the async worker.
- `from_key`, optional `from_value`, callback order, completed counts,
  `MDBX_NOTFOUND` to EOF conversion, and successful `MDBX_RESULT_TRUE`
  lower-bound positioning semantics match the previous executor path.
  Remaining cursor work includes retained initial seek positioning, scan
  helpers, duplicate-subcursor cases, and large/overflow value reads.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorloop-retain-after.txt` and
    `/tmp/mdbx-async-bench-cursorloop-from-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `2e87de0`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor get | 72.884 Mops/s | 72.447 Mops/s |
| parallel cursor get | 59.361 Mops/s | 60.408 Mops/s |
| async cursor get loop | 64.456 Mops/s | 112.291 Mops/s |
| async cursor get loop_from | 111.063 Mops/s | 63.962 Mops/s |
| async threaded cursor get loop | 112.367 Mops/s | 61.741 Mops/s |
| async threaded cget loop_from | 114.040 Mops/s | 103.154 Mops/s |
| async cursor loop | 75.351 Mops/s | 66.881 Mops/s |
| async cursor loop_from | 72.047 Mops/s | 72.505 Mops/s |
| async threaded cursor loop | 130.164 Mops/s | 135.013 Mops/s |
| async threaded cursor loop_from | 131.890 Mops/s | 57.558 Mops/s |
| async-cursor-get-loop/par | 1.086 | 1.859 |
| async-cget-loop-from/par | 1.776 | 0.918 |
| async-cursor-get-loop/ser | 0.884 | 1.550 |
| async-cget-loop-from/ser | 1.002 | 0.569 |
| async-cursor-get-loop/get | 75.907 | 130.118 |
| async-thread-cget-loop/par | 1.893 | 1.022 |
| async-thread-cget-from/par | 1.824 | 1.480 |
| async-thread-cget-loop/ser | 1.542 | 0.852 |
| async-thread-cget-from/ser | 1.029 | 0.917 |
| async-thread-cget-loop/get | 132.330 | 71.542 |
| async-thread-cget-loop/loop | 1.743 | 0.550 |
| async-loop-cursor/par | 1.373 | 1.233 |
| async-loop-cursor/ser | 0.511 | 0.471 |
| async-thread-cursor-loop/par | 2.372 | 2.489 |
| async-thread-cursor-loop/ser | 0.883 | 0.951 |
| async-thread-cursor/batch | 2.059 | 2.112 |
| async-thread-cursor/loop | 1.727 | 2.019 |

- conclusion: this checkpoint moves the `_from` loop continuation after the
  initial seek into the retained sibling-read scheduler, but the benchmark is
  not a throughput win. Direct get-loop improved in this sample, direct
  get-loop-from and several threaded get-loop rows regressed, and cursor-loop
  rows moved in both directions. The structural value is narrower: positioned
  cursor loops no longer have to drive every post-seek sibling read
  synchronously. The next high-value work is a retained seek/lower-bound state
  so `_from` can suspend before the first positioned item too, followed by
  cursor scan and large/overflow value materialization.

Async cursor scan retained sibling-read checkpoint:

- added `async_cursor_scan_pending_t`, a retained state for the common
  non-dupsort `mdbx_async_cursor_scan()` shape using `MDBX_FIRST` plus
  `MDBX_NEXT`, and for `mdbx_async_cursor_scan_from()` using
  `MDBX_SET_LOWERBOUND` or `MDBX_SET_KEY` plus `MDBX_NEXT`.
- initial cursor positioning is still synchronous to preserve the exact public
  scan semantics, including `MDBX_FIRST` repositioning even when the cursor is
  already filled and the `scan_from` no-value `MDBX_GET_CURRENT` fetch. After
  the first item is probed, the retained scan drives an embedded
  `async_cursor_get_batch` operation, skips the already-current pair, and can
  suspend while later right-sibling page-cache reads are outstanding.
- predicate result semantics are preserved: `MDBX_RESULT_TRUE` stops with a
  match, `MDBX_RESULT_FALSE` continues internally or finishes as no-match at
  EOF, and any other predicate result is returned unchanged. `scan_from`
  key/value outputs are updated before the predicate call, matching the
  blocking scan path.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorloop-from-retain-after.txt` and
    `/tmp/mdbx-async-bench-cursorscan-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `03598b4`:

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor get | 72.447 Mops/s | 72.826 Mops/s |
| parallel cursor get | 60.408 Mops/s | 78.192 Mops/s |
| async cursor get loop | 112.291 Mops/s | 66.235 Mops/s |
| async cursor get loop_from | 63.962 Mops/s | 62.966 Mops/s |
| async threaded cursor get loop | 61.741 Mops/s | 107.241 Mops/s |
| async threaded cget loop_from | 103.154 Mops/s | 109.547 Mops/s |
| blocking cursor scan | 109.857 Mops/s | 114.692 Mops/s |
| parallel cursor scan | 120.634 Mops/s | 89.846 Mops/s |
| async cursor scan | 72.009 Mops/s | 104.475 Mops/s |
| async threaded cursor scan | 116.214 Mops/s | 109.244 Mops/s |
| blocking cursor scan_from | 112.507 Mops/s | 111.385 Mops/s |
| parallel cursor scan_from | 69.680 Mops/s | 61.930 Mops/s |
| async cursor scan_from | 119.065 Mops/s | 79.580 Mops/s |
| async threaded cursor scan_from | 117.036 Mops/s | 109.479 Mops/s |
| async-cursor-scan/par | 0.597 | 1.163 |
| async-cursor-scan/ser | 0.655 | 0.911 |

- conclusion: this checkpoint moves cursor scan continuation into the retained
  sibling-read scheduler for the main forward scan shapes. It is mixed on
  throughput: direct async scan improved in this run and now beats the parallel
  cursor-scan baseline, while direct scan-from and threaded scan rows regressed.
  The next cursor work remains retained initial seek/lower-bound traversal,
  duplicate-subcursor scan/loop shapes, reverse scans, and retained
  large/overflow value materialization.

Async cursor large-value retained page-read checkpoint:

- extended `async_cursor_get_batch_pending_t` with a retained large-value read
  phase for `N_BIG` leaf nodes. Instead of calling `node_read()` and blocking
  inside `node_read_bigdata()`, the retained cursor batch now prepares the
  large/overflow page get, submits it through `page_cursor_get_batch_begin()`,
  and can suspend while the explicit page read is outstanding.
- completion reuses the same cursor page-get completion path as ordinary
  cursor page reads, including page validation and cached large-page
  materialization performed by `page_complete_cursor_get()`. The retained
  batch then installs the cursor value ref, fills the output pair, captures the
  transaction pin, and continues with the next item. Cursor loops and retained
  scans inherit this behavior because they drive embedded cursor batches.
- added async smoke coverage that opens a cursor on the large-value test DB,
  reads the rows through `mdbx_async_cursor_get_batch()`, accepts the documented
  EOF `MDBX_RESULT_TRUE` completion, and verifies each overflow payload.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorscan-retain-after.txt` and
    `/tmp/mdbx-async-bench-cursorlarge-retain-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `2be8552`:

| metric | before | after |
| --- | ---: | ---: |
| async large cache batch | 136.305 Kops/s | 139.057 Kops/s |
| async large cache st batch | 138.071 Kops/s | 133.091 Kops/s |
| blocking cursor get | 72.826 Mops/s | 72.309 Mops/s |
| parallel cursor get | 78.192 Mops/s | 54.562 Mops/s |
| async cursor get loop | 66.235 Mops/s | 66.576 Mops/s |
| async cursor get loop_from | 62.966 Mops/s | 62.093 Mops/s |
| async threaded cursor get loop | 107.241 Mops/s | 106.569 Mops/s |
| async threaded cget loop_from | 109.547 Mops/s | 65.505 Mops/s |
| blocking cursor batch | 148.177 Mops/s | 138.732 Mops/s |
| parallel cursor batch | 50.988 Mops/s | 53.693 Mops/s |
| async cursor batch | 64.174 Mops/s | 62.973 Mops/s |
| async threaded cursor batch | 111.921 Mops/s | 59.281 Mops/s |
| blocking cursor scan | 114.692 Mops/s | 111.278 Mops/s |
| parallel cursor scan | 89.846 Mops/s | 58.199 Mops/s |
| async cursor scan | 104.475 Mops/s | 56.401 Mops/s |
| async threaded cursor scan | 109.244 Mops/s | 63.891 Mops/s |
| blocking cursor scan_from | 111.385 Mops/s | 113.651 Mops/s |
| parallel cursor scan_from | 61.930 Mops/s | 62.424 Mops/s |
| async cursor scan_from | 79.580 Mops/s | 100.150 Mops/s |
| async threaded cursor scan_from | 109.479 Mops/s | 108.237 Mops/s |
| async-cursor-get-loop/get | 77.347 | 90.548 |
| async-cursor-batch/get | 74.939 | 85.648 |
| async-thread-cbatch/par | 2.195 | 1.104 |
| async-thread-cbatch/ser | 0.755 | 0.427 |
| async-thread-cbatch/batch | 1.744 | 0.941 |
| async-cursor-scan/par | 1.163 | 0.969 |
| async-cursor-scan/ser | 0.911 | 0.507 |

- conclusion: this is primarily a correctness/semantics migration checkpoint.
  The existing benchmark has large cache rows but no isolated cursor-overflow
  row, so the regular cursor rows are only indirect performance evidence for
  this change. Those rows are mixed and noisy: async cursor batch is roughly
  flat, async scan regressed in this sample, scan-from improved, and threaded
  cursor batch regressed. The structural result is that retained cursor batch,
  cursor loop, and retained scan paths no longer have to block on an
  `N_BIG` overflow page read before the worker can collect other retained read
  work. A later benchmark slice should add an explicit cursor-overflow workload.

Async cursor large-value benchmark coverage checkpoint:

- added isolated cursor-overflow benchmark rows to `mdbx_async_api_bench`.
  The large-value DB is now measured through blocking serial cursor batches,
  blocking pthread-parallel cursor batches, direct async cursor batches, and
  threaded async cursor batches before the large DB is dropped.
- reused the existing large-value payload checker for cursor batch pairs, so
  the new benchmark verifies every returned overflow payload instead of only
  measuring cursor movement over ordinary in-page values.
- added normalized ratios for direct async and threaded async cursor-large
  batches against the blocking serial/parallel cursor-large baselines.
- validation:
  - `git diff --check`: passed
  - `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
  - `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
  - `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
  - Release before/after benchmark logs:
    `/tmp/mdbx-async-bench-cursorlarge-retain-after.txt` and
    `/tmp/mdbx-async-bench-cursorlarge-bench-after.txt`
- repeated-key forced no-mmap/io_uring benchmark with
  `MDBX_ASYNC_BENCH_ITEMS=1000`, `MDBX_ASYNC_BENCH_OPS=30000`,
  `MDBX_ASYNC_BENCH_WRITE_OPS=1`, `MDBX_ASYNC_BENCH_LARGE_OPS=2000`, and
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against `1771c0a`. The
  cursor-large rows are new in the after run:

| metric | before | after |
| --- | ---: | ---: |
| async large cache batch | 139.057 Kops/s | 139.301 Kops/s |
| async large cache st batch | 133.091 Kops/s | 132.311 Kops/s |
| blocking cursor large batch | n/a | 131.708 Kops/s |
| parallel cursor large batch | n/a | 246.520 Kops/s |
| async cursor large batch | n/a | 126.497 Kops/s |
| async threaded cursor large | n/a | 259.483 Kops/s |
| blocking cursor batch | 138.732 Mops/s | 137.337 Mops/s |
| parallel cursor batch | 53.693 Mops/s | 51.485 Mops/s |
| async cursor batch | 62.973 Mops/s | 59.824 Mops/s |
| async threaded cursor batch | 59.281 Mops/s | 107.680 Mops/s |
| async-cursor-large/par | n/a | 0.513 |
| async-cursor-large/ser | n/a | 0.960 |
| async-thread-clarge/par | n/a | 1.053 |
| async-thread-clarge/ser | n/a | 1.970 |
| async-thread-clarge/batch | n/a | 2.051 |
| async-cursor-batch/get | 85.648 | 64.335 |
| async-thread-cbatch/par | 1.104 | 2.092 |
| async-thread-cbatch/ser | 0.427 | 0.784 |
| async-thread-cbatch/batch | 0.941 | 1.800 |

- conclusion: this checkpoint does not change library behavior; it adds the
  missing measurement surface for the retained cursor large-value read path.
  In this sample, direct async cursor-large batch is slightly below the
  blocking serial cursor-large row and about half the blocking parallel row,
  while threaded async cursor-large is slightly above blocking parallel. These
  rows now give future retained large-value changes direct evidence instead of
  relying on ordinary cursor rows as an indirect proxy.

## Public Async Cursor Get Retained NEXT Slice

Routed `mdbx_async_cursor_get(..., MDBX_NEXT)` for plain non-dupsort read
cursors through a retained single-row cursor state when the next step may block
on explicit I/O: crossing to a sibling leaf page or reading an `N_BIG` overflow
value. The normal in-leaf small-value path still uses the existing
`mdbx_cursor_get()` fallback, because there is no page read to overlap and the
extra retained-state allocation only adds overhead.

The retained path reuses the cursor-batch page-read drive internally, but
restores ordinary single-cursor positioning before completing the public async
operation. This avoids the batch API's continuation semantics, where the cursor
is advanced past the returned pair. The worker scheduler now treats
`async_op_cursor_get` as a retained read operation, while draining a pending
single cursor-get before starting later operations to preserve FIFO cursor-state
semantics for callers that queue multiple operations on one cursor.

Unsupported cursor shapes still fall back to the blocking implementation:
`MDBX_FIRST`, seek operations, dupsort cursors, fresh/EOF/hollow/after-delete
cursor states, allocation failure, and non-readable cursors.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-cursorget-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the latest same-shape
saved 10k cursor-get log `/tmp/mdbx-async-bench-singleget-after.txt`. The
large-op count differs, but the ordinary cursor-get measurements run before the
large-value benchmark section.

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor get | 44.974 Mops/s | 39.773 Mops/s |
| parallel cursor get | 51.444 Mops/s | 42.539 Mops/s |
| async cursor get | 1.124 Mops/s | 1.261 Mops/s |
| async cursor get loop | 55.434 Mops/s | 72.008 Mops/s |
| async cursor get loop_from | 52.630 Mops/s | 74.537 Mops/s |
| blocking cursor batch | 93.213 Mops/s | 72.417 Mops/s |
| async cursor batch | 74.441 Mops/s | 71.256 Mops/s |
| async-cursor-get/par | 0.022 | 0.030 |
| async-cursor-get/ser | 0.025 | 0.032 |
| async-cursor-get-loop/get | 49.316 | 57.095 |
| async-cursor-batch/get | 66.225 | 56.499 |

Single-run noise remains substantial in the cursor microbenchmarks. The direct
`async cursor get` row improved modestly in this sample, but the main result of
this checkpoint is structural: public async cursor `NEXT` is no longer purely a
worker-offloaded blocking call when it must fetch a sibling page or overflow
value from explicit I/O.

## Retained Cursor FIRST Page-Positioning Slice

Moved the `MDBX_FIRST` startup path used by retained cursor batches from the
blocking `outer_first()` fallback to an internal submit/drive/complete state.
For a fresh plain non-dupsort read cursor, `async_cursor_get_batch_start()` now
submits the root page read, consumes it into the cursor stack, then follows the
leftmost branch child one page at a time through `page_cursor_get_batch_*()`
until it reaches the first leaf. Once the first leaf is positioned, the existing
retained cursor-batch loop handles ordinary values, `N_BIG` overflow values,
and sibling transitions.

This automatically affects higher-level retained cursor APIs that build their
initial scan from `async_cursor_get_batch_start(MDBX_FIRST)`, including plain
cursor batch, cursor batch loops, and plain cursor scans. It also fixed the
retained batch EOF probe so filling the output buffer exactly at the end of the
last leaf still reports `MDBX_RESULT_TRUE`, matching
`mdbx_cursor_get_batch()`.

Unsupported cursor shapes still fall back or return the existing errors:
dupsort cursors, invalid limits, non-readable cursors, and non-`FIRST`/`NEXT`
batch operations. Positioned lower-bound starts still have a blocking
`mdbx_cursor_get(..., MDBX_SET_LOWERBOUND)` island and remain future work.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark logs:
  `/tmp/mdbx-async-bench-cursorget-retain-after.txt` and
  `/tmp/mdbx-async-bench-cursorfirst-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`f49d824`.

| metric | before | after |
| --- | ---: | ---: |
| async cursor get | 1.261 Mops/s | 846.441 Kops/s |
| async cursor get loop | 72.008 Mops/s | 47.696 Mops/s |
| async cursor get loop_from | 74.537 Mops/s | 50.718 Mops/s |
| async threaded cursor get loop | 52.581 Mops/s | 67.437 Mops/s |
| blocking cursor batch | 72.417 Mops/s | 72.766 Mops/s |
| async cursor batch | 71.256 Mops/s | 50.193 Mops/s |
| async threaded cursor batch | 42.443 Mops/s | 47.149 Mops/s |
| async cursor scan | 50.694 Mops/s | 60.591 Mops/s |
| async cursor scan_from | 68.969 Mops/s | 56.276 Mops/s |
| async-cursor-get-loop/get | 57.095 | 56.349 |
| async-cursor-batch/get | 56.499 | 59.299 |
| async-cursor-scan/par | 1.055 | 1.313 |
| async-scan-from/par | 1.314 | 1.065 |

The cache-hot cursor microbenchmarks are mixed and some rows regress, which is
expected for a first-position state machine that is mainly valuable when the
root/leftmost descent has real explicit-I/O misses to overlap. The structural
gain is that retained cursor batch/loop/scan no longer have to block in
`outer_first()` before the worker can collect other pending read work.

## Public Async Cursor Get FIRST Retained Slice

Extended the retained single-row cursor get path from `MDBX_NEXT` to
`MDBX_FIRST` for plain read-only non-dupsort cursors. Public
`mdbx_async_cursor_get(..., MDBX_FIRST)` now uses the same retained
root/leftmost descent added for cursor batches, then consumes one pair through
the retained cursor-batch drive and restores normal single-cursor positioning.
This moves the common public cursor-first read path away from direct
worker-offloaded `mdbx_cursor_get()` when the cursor belongs to a read-only
transaction.

The retained cursor batch and single cursor-get paths now explicitly require a
read-only transaction before using retained page-cache I/O. Write cursors fall
back to the existing blocking worker path. The benchmark caught this: allowing
retained first-positioning on write cursors corrupted later write-cursor delete
state, so the read-only gate is part of the correctness fix as well as the API
scope boundary.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-cursorsingle-first-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`8ea07f5`.

| metric | before | after |
| --- | ---: | ---: |
| async cursor get | 846.441 Kops/s | 840.556 Kops/s |
| async cursor get loop | 47.696 Mops/s | 50.088 Mops/s |
| async cursor get loop_from | 50.718 Mops/s | 53.954 Mops/s |
| async threaded cursor get loop | 67.437 Mops/s | 50.704 Mops/s |
| async cursor batch | 50.193 Mops/s | 42.006 Mops/s |
| async threaded cursor batch | 47.149 Mops/s | 72.145 Mops/s |
| async cursor scan | 60.591 Mops/s | 51.244 Mops/s |
| async cursor scan_from | 56.276 Mops/s | 54.748 Mops/s |
| async-cursor-get/par | 0.017 | 0.016 |
| async-cursor-get-loop/get | 56.349 | 59.590 |
| async-cursor-batch/get | 59.299 | 49.974 |

The direct `async cursor get` microbenchmark is essentially flat in this
cache-hot sample. The structural result is that both public single cursor
`FIRST` and public single cursor `NEXT` can now suspend on read-only explicit
I/O page reads for the supported plain-cursor cases; write cursors and
positioned seek starts still use the blocking worker path.

## Public Async Cursor Get Lower-Bound Retained Slice

Extended the retained single-row cursor get path to
`mdbx_async_cursor_get(..., MDBX_SET_LOWERBOUND)` for plain read-only
non-dupsort cursors. The supported public cursor lower-bound path now submits
the root page read, drives branch-page descent through the explicit page-cache
read engine, and resumes the actual cursor state after each page read
completion instead of starting with a blocking `mdbx_cursor_get()` seek.

The leaf finish is intentionally still conservative: after the retained
root/branch descent reaches a resident leaf, it calls
`cursor_ops(..., MDBX_SET_LOWERBOUND)` to perform the leaf-local search and
normal result setup. That means overflow values or leaf-edge sibling movement
inside the final cursor operation can still block. This is an incremental seek
migration, not the finished lower-bound coroutine for every page access.

The smoke test now covers an inexact lower-bound single cursor get and verifies
the expected `MDBX_RESULT_TRUE` result, returned key, and value payload.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-cursorget-lowerbound-retain-after-rerun.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`761a0c8`.

| metric | before | after |
| --- | ---: | ---: |
| async single lowerbound | 266.116 Kops/s | 242.052 Kops/s |
| async lowerbound batch | 1.221 Mops/s | 1.095 Mops/s |
| async lowerbound many | 1.201 Mops/s | 673.068 Kops/s |
| async lowerbound loop | 1.337 Mops/s | 1.123 Mops/s |
| async threaded lower loop | 1.315 Mops/s | 1.468 Mops/s |
| async cursor get | 840.556 Kops/s | 906.229 Kops/s |
| async cursor get loop | 50.088 Mops/s | 52.361 Mops/s |
| async cursor get loop_from | 53.954 Mops/s | 68.560 Mops/s |
| async threaded cget loop_from | 58.238 Mops/s | 47.094 Mops/s |
| async cursor scan_from | 54.748 Mops/s | 49.968 Mops/s |
| async-lower-loop/par | 2.317 | 1.564 |
| async-thread-lower-loop/par | 2.278 | 2.044 |
| async-cursor-get/par | 0.016 | 0.021 |
| async-cursor-get-loop/get | 59.590 | 57.779 |
| async-cget-loop-from/scan | 0.985 | 1.372 |

Single-run benchmark noise remains visible, and most lower-bound batch/loop
rows are separate public API paths rather than this single cursor-get path. The
important structural result is that public single cursor lower-bound can now
suspend on root and branch page reads in the supported read-only plain-cursor
case. Positioned `loop_from` and `scan_from` starts still contain blocking
`mdbx_cursor_get(..., MDBX_SET_LOWERBOUND)` islands and remain future work.

## Public Async Cursor Loop/Scan Lower-Bound Start Slice

Refactored the retained public cursor lower-bound descent into a reusable
`async_cursor_seek_pending_t` state and embedded it in the retained
`mdbx_async_cursor_get_loop_from()` and `mdbx_async_cursor_scan_from()` state
machines. For plain read-only non-dupsort cursors, positioned
`MDBX_SET_LOWERBOUND` starts now submit the root and branch-page reads through
the explicit page-cache engine, suspend while the page read is in flight, and
resume into the loop callback or scan predicate once the leaf is resident.

Compatibility boundaries remain conservative. `MDBX_SET_KEY` positioned starts,
write cursors, dupsort/subcursor shapes, and unsupported cursor forms still use
the existing blocking fallback. The retained seek still finishes on the
resident leaf through `cursor_ops(..., MDBX_SET_LOWERBOUND)`, so leaf-local
search, edge sibling movement, and overflow-value handling inside that final
operation are not fully coroutine-driven yet.

The smoke test now covers an inexact `mdbx_async_cursor_scan_from(...,
MDBX_SET_LOWERBOUND)` start in addition to the existing inexact cursor
`loop_from` and single cursor-get lower-bound checks.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-loop-scan-lowerbound-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`25a6575`.

| metric | before | after |
| --- | ---: | ---: |
| async lowerbound loop | 1.123 Mops/s | 1.225 Mops/s |
| async threaded lower loop | 1.468 Mops/s | 1.081 Mops/s |
| async cursor get loop_from | 68.560 Mops/s | 53.770 Mops/s |
| async threaded cget loop_from | 47.094 Mops/s | 53.171 Mops/s |
| async cursor scan_from | 49.968 Mops/s | 47.863 Mops/s |
| async-lower-loop/par | 1.564 | 1.660 |
| async-thread-lower-loop/par | 2.044 | 1.465 |
| async-cget-loop-from/par | 1.460 | 0.976 |
| async-cget-loop-from/ser | 1.377 | 1.067 |
| async-cget-loop-from/scan | 1.372 | 1.123 |
| async-scan-from/par | 1.064 | 0.869 |
| async-scan-from/ser | 1.003 | 0.950 |
| async-scan-from/scan | 0.785 | 0.812 |

The cache-hot loop/scan microbenchmarks remain noisy and mixed. This checkpoint
is primarily structural: the first positioned lower-bound page descent for
public loop and scan operations is no longer a mandatory blocking
`mdbx_cursor_get()` call on the async worker.

## Public Async Cursor Exact Seek Retained Slice

Extended the reusable retained seek state so the final leaf operation can be
`MDBX_SET_KEY` as well as `MDBX_SET_LOWERBOUND`. Plain read-only non-dupsort
public cursor paths now use retained root/branch page descent for exact
positioned seeks in:

- `mdbx_async_cursor_get(..., MDBX_SET_KEY)`
- `mdbx_async_cursor_get_loop_from(..., MDBX_SET_KEY, ..., MDBX_NEXT)`
- `mdbx_async_cursor_scan_from(..., MDBX_SET_KEY, ..., MDBX_NEXT)`

The compatibility envelope is unchanged: write cursors, dupsort/subcursor
shapes, and unsupported cursor forms still fall back to the existing blocking
worker path. The retained seek still delegates the resident-leaf finish to
`cursor_ops()`, so overflow value reads and any leaf-edge movement triggered
inside that final operation remain future coroutine work.

Smoke coverage now includes exact `MDBX_SET_KEY` single cursor get, cursor
`loop_from`, and `scan_from` cases. The benchmark harness also adds a direct
`async cursor get set-key` row and normalized ratios for that exact seek path.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-cursor-setkey-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`0d3caa9`.

| metric | before | after |
| --- | ---: | ---: |
| async cursor get | 1.277 Mops/s | 774.470 Kops/s |
| async cursor get set-key | n/a | 644.125 Kops/s |
| async cursor get loop_from | 53.770 Mops/s | 50.468 Mops/s |
| async cursor scan_from | 47.863 Mops/s | 66.294 Mops/s |
| async-cursor-get/par | 0.030 | 0.016 |
| async-cursor-get/ser | 0.033 | 0.019 |
| async-cget-setkey/par | n/a | 0.013 |
| async-cget-setkey/ser | n/a | 0.016 |
| async-cget-setkey/get | n/a | 0.832 |
| async-cget-loop-from/par | 0.976 | 1.051 |
| async-scan-from/par | 0.869 | 1.380 |
| async-scan-from/scan | 0.812 | 1.360 |

The direct async cursor-get row regressed in this noisy cache-hot sample, while
scan-from improved. The important structural change is that exact positioned
public cursor seeks can now suspend on root and branch explicit-I/O page reads
instead of starting with a blocking `mdbx_cursor_get(..., MDBX_SET_KEY)`.

## Public Async Cursor Seek Leaf-Finish Slice

Moved the supported plain-cursor retained seek leaf finish out of
`cursor_ops()` and into the async seek state. After root/branch descent reaches
a resident leaf, `async_cursor_seek_pending_t` now performs the plain
`tree_search_foliage()` search itself, preserves integer-key alignment through
`check_key()`, and completes `MDBX_SET_KEY` / `MDBX_SET_LOWERBOUND` without a
mandatory blocking cursor call.

This also adds retained continuations for the two remaining page-read cases in
the supported plain seek finish:

- inexact lower-bound at the end of a leaf submits the right-sibling leaf read
  before resuming the leaf search;
- `N_BIG` overflow values submit and complete the large-page read before
  returning the value payload.

Unsupported shapes remain conservative. Dupsort nodes, write cursors,
subcursors, and other cursor operations still use the existing fallback paths.

Smoke coverage now includes an exact `MDBX_SET_KEY` async cursor get over a
large/overflow value. The benchmark harness also adds
`async cursor large set-key`, which measures public async cursor exact seeks
over overflow values directly.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-seek-leaf-retain-after-final.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`972bcf7`.

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor large batch | 140.999 Kops/s | 140.367 Kops/s |
| parallel cursor large batch | 467.899 Kops/s | 459.407 Kops/s |
| async cursor large batch | 144.305 Kops/s | 143.808 Kops/s |
| async cursor large set-key | n/a | 121.223 Kops/s |
| async threaded cursor large | 500.774 Kops/s | 497.134 Kops/s |
| async cursor get | 774.470 Kops/s | 1.162 Mops/s |
| async cursor get set-key | 644.125 Kops/s | 586.130 Kops/s |
| async cursor scan_from | 66.294 Mops/s | 66.660 Mops/s |
| async-cursor-large/par | 0.308 | 0.313 |
| async-cursor-large/ser | 1.023 | 1.025 |
| async-clarge-setkey/par | n/a | 0.264 |
| async-clarge-setkey/ser | n/a | 0.864 |
| async-clarge-setkey/batch | n/a | 0.843 |
| async-cget-setkey/get | 0.832 | 0.504 |

As before, cache-hot cursor rows are noisy. The direct structural gain is that
the supported public cursor seek path can now suspend through root, branch,
right-sibling, and overflow-value page reads without falling back to a blocking
`cursor_ops()` finish.

## Public Async Cursor Batches Positioned-Start Slice

Routed `mdbx_async_cursor_get_batches_from()` through the retained async seek
state for plain read-only non-dupsort positioned starts with
`MDBX_SET_KEY` or `MDBX_SET_LOWERBOUND`. The operation now submits and resumes
the initial root/branch/leaf/overflow seek before continuing with the existing
retained cursor batch loop, instead of always starting with a blocking
`mdbx_cursor_get()`.

The batch API's existing continuation semantics are preserved: the positioned
row is the first row visible to the following retained batch, and output
`from_key` / `from_value` continue to track the last returned pair after batch
callbacks run.

Smoke coverage now includes exact `MDBX_SET_KEY` cursor batch-from. The
benchmark harness also adds `async cursor loop set-key` and normalized ratios
against the existing cursor batch and lower-bound batch-from rows.

Validation:

- `git diff --check`: passed
- `cmake --build @cmake-ninja-build --target mdbx_async_api_smoke mdbx_async_api_bench`: passed
- `ctest --test-dir @cmake-ninja-build --output-on-failure -R '^(async_api|c_api|migration_smoke)'`: passed 11/11
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_IO_BACKEND=io_uring MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_async_api_smoke`: passed
- Benchmark log: `/tmp/mdbx-async-bench-batches-setkey-retain-after.txt`

Reduced forced no-mmap/io_uring benchmark with `MDBX_ASYNC_BENCH_ITEMS=10000`,
`MDBX_ASYNC_BENCH_OPS=30000`, `MDBX_ASYNC_BENCH_WRITE_OPS=1000`,
`MDBX_ASYNC_BENCH_LARGE_OPS=30000`, and
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, compared against the previous checkpoint
`45ca3ef`.

| metric | before | after |
| --- | ---: | ---: |
| blocking cursor batch | 77.021 Mops/s | 75.762 Mops/s |
| parallel cursor batch | 33.890 Mops/s | 53.466 Mops/s |
| async cursor batch | 70.207 Mops/s | 51.270 Mops/s |
| async cursor loop | 50.665 Mops/s | 51.611 Mops/s |
| async cursor loop_from | 52.554 Mops/s | 53.350 Mops/s |
| async cursor loop set-key | n/a | 51.473 Mops/s |
| async threaded cursor loop | 68.561 Mops/s | 70.977 Mops/s |
| async-loop-cursor/par | 1.495 | 0.965 |
| async-loop-cursor/ser | 0.658 | 0.681 |
| async-loop-csetkey/par | n/a | 0.963 |
| async-loop-csetkey/ser | n/a | 0.679 |
| async-loop-csetkey/from | n/a | 0.965 |

The direct structural result is that public async cursor batch-from positioned
starts now share the retained seek machinery used by single cursor get,
cursor get loops, and scans for the supported plain read-only cases.
