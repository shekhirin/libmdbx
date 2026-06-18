# Migrate Data-File Access To Explicit I/O

This note records the current feasibility assessment and the migration work
packages for replacing libmdbx data-file mmap access with an explicit,
async-capable I/O layer. The intended first milestone is still synchronous from
the public API: convert internal access from mmap pointer arithmetic to pinned
page-cache buffers first, then add async submission/completion behind the same
storage interface.

## Scope

- Remove data-file mmap from the DXB path.
- Keep lock-file mmap for the first phase. Reader slots, writer locking, and
  interprocess MVCC coordination continue to use `lck_mmap`.
- Keep the public C API synchronous until explicit I/O correctness and
  performance are proven.
- Treat `MDBX_WRITEMAP` as unsupported for the explicit-I/O migration. It cannot
  keep its old data-file mmap semantics, so the current branch rejects it at
  open time.

## Current Code Shape

The data-file mmap used to be part of the core page addressing contract. The
current branch has removed the accepted data-file mapping and is converging the
remaining page access on explicit storage plus pinned page-cache buffers:

- `MDBX_env` no longer stores an `osal_mmap_t` for the data file or legacy raw
  DXB fd aliases. `dxb_storage_t` now owns the data/meta/dsync descriptors,
  file-size/geometry state, the explicit page cache, and the dirty-write queue.
  `osal_mmap_t lck_mmap` remains for the lock file.
- The former generic `pgno2page(env, pgno)` and mapped helper fallbacks have
  been removed from the core source. Normal committed-page reads go through
  `page_get_committed()` and the explicit page-cache path.
- `page_get_committed()` centralizes committed-page lookup. It now asserts that
  `MDBX_WRITEMAP` is absent, matching the open-time policy, instead of
  returning the old mapped page result. Normal reads flow through an explicit
  page-cache request built as a checked `dxb_page_io_t` and submitted through
  `dxb_storage_read_io()`.
  Read-only transactions can reuse clean entries keyed by `(pgno,
  snapshot_txnid)`.
  Unpinned entries are always reusable, and pinned entries are reusable when
  they are branch/leaf pages or already-expanded overflow spans. A pinned
  single-page overflow header remains private because `page_cache_read_large()`
  can replace the entry buffer while materializing the full span; if an
  expandable tracked entry ever has multiple pins at materialization time, the
  expanding result detaches into a private unlisted cache ref instead of
  replacing a shared buffer. Normal writer reads use private unlisted entries,
  while validation/checking builds can track those private entries so
  `page_check()` can classify them.
  `page_get_unchecked()` checks dirty/spilled transaction pages before falling
  back to committed-page lookup.
- `pgr_t` now carries a named `page_ref_t` alongside the returned `page_t *`.
  Committed reads are cache refs, while dirty-list, newly allocated, loose, and
  unspilled pages are transaction-dirty refs.
  `MDBX_cursor` now has a parallel `pgref[]` stack; central push/pop,
  reset/drown, clone/copy, tree search/deepen, cache fallback search, new-root
  creation, page-split/root-split cursor adjustment, subtree cutoff, and
  `page_touch()` page replacement paths retain and clear that metadata.
  Compacting-copy cursor stacks now use synthetic refs for their writable page
  copies, and rebalance neighbor setup installs left/right sibling pages through
  the same helper path. Nested subcursor refresh/update paths now update
  level-zero subcursor refs through helper calls, including newly allocated
  subtree roots. Root-collapse stack shifts now route the new child root and
  shifted stack entries through helper calls, and range/cutoff cursor copies now
  preserve `pgref[]` metadata. Tree-drop stack restoration now saves and
  restores `pgref[]` metadata with the saved page stack. Node
  move/merge cursor redirection and post-merge stack restore now also use cursor
  stack helpers. Cursor refs are now initialized, retained before replacement,
  released on reset/drown/poor-state transitions, and explicitly retained across
  nested-transaction cursor backups. Inner-cursor invalidation and root-split
  debug clearing now release through the same helper layer. `dxb_storage_t` now
  has a page-cache state block, `page_ref_t` can point at a cache entry, cursor
  retain/release updates cache pin counts, env teardown frees cache entries, and
  `page_check()` asserts that `MDBX_WRITEMAP` is absent while accepting
  explicit-I/O cached, dirty, and txn-owned pages as valid non-mmap page
  sources. The page cache has a storage-owned mutex, snapshot ids for reusable
  read-only entries, private unlisted entries for normal writer reads, and a
  default 64 MiB explicit-cache cap. Audit cursor validation now keeps explicitly
  fetched branch-child pages pinned while their page contents are checked, so
  overflow validation cannot reuse the child cache buffer mid-check.
  `MDBX_cursor` also has a `value_ref` for the latest returned
  non-stack value, and `node_read_bigdata()` now retains overflow-page refs
  there so future cached large-value pages can outlive the local `pgr_t`. Local
  `pgr_t` ownership now has explicit release/consume helpers, and short-lived
  page results are released after they are retained by cursor/value stacks or
  after transient validation/copy/retire use. This now covers tree descent,
  sibling movement, root setup/collapse, compacting, defrag, overflow
  read/validate/delete paths, subtree cutoff, page retirement, page walking, and
  rebalance neighbor clones. For non-`MDBX_WRITEMAP` transactions,
  `page_get_committed()` now receives a checked `dxb_page_io_t`, allocates or
  reuses an explicit page-cache entry, reads the page through
  `dxb_storage_read_io()` on cache misses, and returns it as a `PAGE_REF_CACHE`
  result; overflow-page requests can extend that entry to the full large-page
  span after header validation. The remaining `MDBX_WRITEMAP` branch is
  unreachable through accepted opens.
  `page_get_unchecked()` checks dirty pages before falling back to explicit
  committed-page reads, so uncommitted/new pages are not fetched from disk. The
  remaining `pg[]` search hits must stay limited to helper internals,
  comparisons, assertions, and comments.
- `page_alloc_finalize()` now asserts that `MDBX_WRITEMAP` is absent. Accepted
  write transactions allocate new pages only as transaction-owned dirty
  buffers, and successful allocation results are tagged as
  `PAGE_REF_TXN_DIRTY | PAGE_REF_OWNED`. The old mapped allocation and
  prefault-write arm no longer exists in the accepted allocation path.
- `MDBX_txn` now has a retained page-ref list for cursorless public read
  results. `mdbx_get()`, `mdbx_get_ex()`, `mdbx_get_equal_or_great()`, and the
  non-writemap `mdbx_cache_get*()` fallback retain cache-backed cursor/value
  refs into the transaction before releasing their stack-local cursors. Those
  retained refs are released on read-txn reset/free, basal write-txn end, and
  nested txn finish/free. This preserves returned `MDBX_val` bytes without
  leaking every stack-local cursor pin until environment close.
- The mapped metadata helper island has been removed. The old
  `MAPPED_METAPAGE()`, `meta_*_mapped()`, `mapped_pgno2page()`,
  `mapped_ptr2page()`, and `meta_update_begin()`/`meta_update_end()` paths are
  gone from the accepted metadata flow. Metadata selection now uses refreshed
  env-owned shadow pages, and meta writes use logical slot offsets plus
  explicit `dxb_write()`/`dxb_write_pages()` calls.
- `dxb_setup()` reads candidate meta pages with `pread()` before open and now
  always initializes accepted environments through the explicit storage path
  without calling `osal_mmap()` for the data file. The temporary
  `MDBX_COMPAT_DATA_MMAP=1` comparison backend has been removed from data-file
  setup; `MDBX_WRITEMAP` remains rejected before backend selection can preserve
  old mapped-write behavior.
- Non-writemap commits already have a useful explicit write path:
  `txn_basal_commit()` builds an `iov_ctx_t`, `txn_write()` iterates the dirty
  page list, and `iov_page()`/`iov_write()` submit pages through the
  storage-owned dirty-write queue.
- This branch now routes explicit read, write, copy, advisory, prefetch, and
  discard operations through storage-owned helpers instead of env-shaped DXB
  adapters. The remaining higher-level sync wrappers keep environment pgop
  accounting and policy selection, while storage owns raw byte/page I/O,
  file-size changes, data-cache invalidation, `posix_fadvise()`/`F_RDADVISE`
  hints, and page/byte geometry for page-addressed operations. Explicit writes
  are channelized as data-file or meta-file writes, so call sites no longer pass
  raw DXB file handles into the facade. Full-page reads, writes, same-file page
  copies, and prefetch hints are routed through page-addressed helpers where the
  page number is already known. Those page-addressed helpers now share a
  checked `dxb_page_io_t` descriptor for `(pgno, npages)` to `(offset, bytes)`
  conversion, giving a later async backend one storage-owned page request shape
  for reads, queued writes, prefetch, writev offsets, cache invalidation, and
  file-range copies. Explicit page-cache entries now store that descriptor
  directly, so cache lifetime, eviction accounting, overlap invalidation, and
  overflow materialization use the same checked page-I/O geometry as the
  storage operations that fill or invalidate them. Cache miss fills and
  overflow-span materialization now pass the stored descriptor directly into
  the storage read helper, so the cache entry's descriptor is also the actual
  read request shape rather than a parallel bookkeeping copy. The page-cache
  read boundary now constructs that checked request descriptor before lookup,
  so cache hits, miss fills, and future async submission share the same
  storage-owned request object. Committed-page lookup now constructs the
  descriptor before entering the cache, and the fast key/value cache fallback
  does the same before materializing a cached offset. Dirty/spilled transaction
  checks run before committed-page request construction, so only reads that
  actually fall through to storage/cache create a page-I/O request. Defrag
  fallback page reads now also build one-page `dxb_page_io_t` requests and
  submit them through `dxb_storage_read_io()`, leaving no helper-local
  page-read conversion layer between page-number callers and storage reads.
  Remaining byte-addressed reads, including warmup range scans, portable
  environment-copy chunks, coherency root-txnid probes, and startup meta-header
  double-reads before page size is known, now build checked `dxb_byte_io_t`
  requests and submit them through `dxb_storage_read_bytes()`. The old raw
  `dxb_storage_read()` helper is gone from the C source. Byte-addressed meta
  writes now use the same checked `dxb_byte_io_t` request shape before
  submission through `dxb_storage_write_bytes()`, and page writes adapt their
  checked `dxb_page_io_t` through that byte-write submission point. Vector page
  writes now compute their iovec span, build a `dxb_byte_io_t` at the
  page-derived offset, and submit through `dxb_storage_writev_bytes()`. Queued
  dirty writes now adapt their checked page request into a checked
  `dxb_byte_io_t` before insertion, and `osal_ioring_add()`/`osal_ioring_walk()`
  exchange that byte request shape with dirty-write completion instead of
  loose offset/length pairs. Queued write items now also store that
  `dxb_byte_io_t` internally and use it for coalescing, walking, and POSIX
  write submission, with the queued payload span validated against the stored
  descriptor before submission; the old raw item offset and separate last-byte
  accounting are gone. The old raw `dxb_storage_read()`,
  `dxb_storage_write()`, `dxb_storage_writev()`, and
  `dxb_storage_add_queued_write()` helpers are gone from the C source. The
  final storage adapters for `pread()`, `pwrite()`, and `pwritev()` now accept
  the same `dxb_byte_io_t` request instead of loose byte/offset pairs, and the
  async-style partial-writev fault hook consumes that descriptor too.
  The remaining page-number read/write request adapters
  `dxb_storage_read_io()` and `dxb_storage_write_io()` have been removed from
  the C source. Meta-shadow refresh, initial meta triplet creation, defrag
  fallback page moves, dirty-page queue setup/enqueue, explicit meta override,
  and debug page-kill writes now build checked `dxb_page_io_t` descriptors
  first, while storage helpers derive `dxb_byte_io_t` descriptors at the
  submission boundary. The old single-use read/write page wrapper structs are
  gone, leaving the async-facing request shapes derived from one validated
  page-span object instead of helper-local `(pgno, npages)` adapters.
  The same descriptor-first cleanup now covers in-file page copies, data-page
  sync ranges, and page-field byte subranges: the old
  `dxb_storage_copy_io()`, `dxb_storage_sync_io()`,
  `dxb_storage_page_span_bytes_io()`, and `dxb_storage_page_field_io()` helpers
  are gone, while defrag copy tails, commit/pre-sync ranges, meta payload
  offsets, and coherency root-txnid probes build checked `dxb_page_io_t`
  requests before deriving copy, sync, or byte-range descriptors.
  Single-use wrappers for coverage-prefix and meta payload/sign-field requests
  are also gone; coherency head acceptance now constructs the page-prefix
  coverage descriptor in place, and meta commit/wipe paths call
  `dxb_storage_write_meta()` with logical meta number, payload offset, and
  length so storage derives the byte descriptor internally.
  Non-compacting environment-copy `sendfile()` and `copy_file_range()` fast
  paths now also build source `dxb_byte_io_t` requests before entering storage,
  and the in-file page-copy helper converts its source/destination page
  descriptors into byte descriptors before issuing `copy_file_range()`.
  Readahead plus data-file tail discard paths now build checked
  `dxb_byte_io_t` requests before fd-backed advice/discard through storage; an
  earlier explicit-only
  cleanup removed the mapped `madvise()`/`MADV_REMOVE` data-file helper
  branches.
  Coherency root probes now build a byte descriptor before checking the current
  file view, and cache invalidation receives the same descriptor shape from
  writes, discard, and truncate-driven shrink paths.
  Data-page sync callers now build checked `dxb_page_io_t` descriptors for the
  committed data range and submit them through storage instead of choosing
  `msync()` versus `fsync()` themselves, and explicit meta-write call sites
  apply the existing `meta_fd == data_fd` sync rule before submitting directly
  through storage.
  Data-file descriptor parking now uses a checked zero-length `dxb_byte_io_t`
  request instead of passing a loose raw offset through storage helpers.
  Data-file POSIX lock ranges now build checked `dxb_lock_io_t` descriptors
  before entering storage lock helpers.
- `MDBX_env` now contains an env-owned `dxb_storage_t` block for the data, meta,
  and dsync fds, `filesize`, `current`, and `limit` state, the explicit page
  cache, and the dirty-write queue. DXB helper internals and non-pointer size
  decisions now use the storage state instead of a mapped-file shell.
  Generic opened-environment checks now use `ENV_ACTIVE` plus the storage data
  fd instead of treating a mapped address as the open-state sentinel. This covers
  public env/txn validation, environment info file-stat reads, close-time sync
  and writer-owner checks, reader-slot binding, geometry setup routing, rejection
  of recovery opens against an already-open environment, and post-open rejection
  of fixed-size `max_db`/`max_readers` option changes. DXB close/reset now also
  uses the storage descriptors as the authoritative teardown state before
  synchronizing the legacy fd aliases back to invalid handles.
- The non-compacting environment-copy path now seeds the destination meta from
  the read transaction's actual geometry, GC tree, and main tree before writing
  the copied meta pages. Its portable fallback reads source bytes through
  `dxb_read()` instead of dereferencing a data-file mapping.
- Meta selection no longer returns data-file mapped pointers for accepted
  environments. `meta_ptr_t` carries the selected shadow pointer plus logical
  meta slot number, and meta writes use slot-derived DXB file offsets instead
  of subtracting mapped addresses from an old data-file mapping.
- `MDBX_env` now has an env-owned three-page meta shadow buffer. It is
  refreshed by building a checked `dxb_page_io_t` for the three meta pages and
  submitting it through `dxb_storage_read_io()` after the data file is opened,
  and is kept in step after successful meta-page/sign writes. Flat read-only
  and write transaction starts now choose their initial meta head from
  refreshed shadow snapshots, and `mdbx_env_info_ex()` builds its meta fields
  from refreshed shadow pages. `env_sync()` also refreshes shadow metadata
  before selecting the observed head or initializing the writer-txn troika, and
  `env_open()` seeds `meta_sync_txnid` from a shadow-backed
  recent-committed-txnid read. Open-time
  meta validation, automatic rollback decisions, meta geometry/signature upgrade
  checks, recovery meta turn-over, default meta-override DB identity selection,
  opened-environment geometry defaults/updates, and reader-list lag accounting
  now inspect source meta pages from the shadow buffer without a mapped
  `pgno2page()` fallback. Non-writemap steady-meta wiping, commit metadata selection,
  commit pending-meta construction, and GC steady-checkpoint decisions read the
  shadow meta slots before issuing explicit writes. Environment warmup range
  selection, active-writer `env_sync()` head selection, debug open logging,
  read-only MVCC oldest/recent discovery, and write-side MVCC oldest/laggard
  accounting now also use refreshed shadow meta snapshots instead of mapped
  meta pages for non-writemap environments. Warmup
  keeps its mapped touch/lock behavior when the data mapping exists, but the
  no-data-mapping path can now satisfy forced warmup by reading the selected
  range through `dxb_read()` into an aligned scratch buffer; lock warmup remains
  mapped-only and returns `MDBX_ENOSYS` without a process address range to lock.
  The former mmap tail-poisoning hook has been removed because accepted data
  opens no longer create a data-file mapping. Retired-page mapped-payload
  poisoning is skipped without a mapping. Meta-troika diagnostics can render
  shadow-backed
  snapshots. The shadow tap helper no longer samples mapped data-file meta
  pages as a freshness oracle; it rereads the three meta pages through
  the meta-shadow refresh descriptor path before selecting the current shadow
  troika. This removes one more data-mmap dependency at the cost of extra
  point-lookup
  overhead until an explicit generation/cache-refresh policy replaces the
  mapped oracle. A
  retry-protected cached tap helper now reuses the current shadow buffer for the
  first sample in read-transaction seize, read-only transaction observer, and
  reader-list loops; those paths still force an explicit metadata reread through
  `meta_shadow_should_retry()` before accepting the observed head, so a
  cross-process commit can only cause a retry rather than a stale result.
  `coherency_check()` now probes GC/Main root-page txnids by reading the root
  page's `txnid` field through `dxb_read()` when the explicit storage view
  covers that page. The mapped root-page fallback has been removed, and
  transaction-head acceptance refreshes storage size before validation when the
  accepted meta geometry extends beyond the current explicit storage view. This
  keeps root `mod_txnid` validation from silently disappearing with the data
  mapping.
  Commit metadata sync now uses the explicit data/meta file path for accepted
  opens; `MDBX_WRITEMAP` remains rejected before these paths are reachable.
- Fast key/value cache entries now treat `MDBX_cache_entry_t.offset` as a
  data-file offset. `mdbx_cache_get*()` refreshes stale entries with a normal
  cursor search, stores the resulting data-file offset, retains any cache-backed
  result pages in the transaction, and can serve later hits by reading/pinning
  the referenced page or overflow extent through the explicit page cache. The
  former mapped `MDBX_WRITEMAP` fast-cache path has been removed; cache access
  now asserts the open-time invariant that accepted environments cannot carry
  `MDBX_WRITEMAP`. Non-writemap stale entries and conservative not-found/ABA
  cases still fall back to a normal cursor search.
- `mdbx_is_dirty()` no longer requires a data-file mapping to classify public
  value pointers. It recognizes explicit page-cache ranges as clean or dirty by
  page txnid and recognizes dirty-list ranges in the current/parent write
  transaction as dirty. Pointers outside known explicit-I/O buffers now assert
  that `MDBX_WRITEMAP` is absent, return `MDBX_EINVAL` for read-only
  transactions, and keep the documented conservative dirty answer for
  write-transaction pointers.
- Readahead and data-file tail discard helpers now use fd-backed advice for
  accepted environments. Mapped `madvise()` and `MADV_REMOVE` data-file helper
  branches have been removed, so ordinary runs exercise the same storage-fd
  advisory path as forced no-data-mmap runs.
- `env_is_page_incore()` now reports not-resident without probing data-file
  mmap state, so callers fall back to explicit file I/O instead of invoking
  `mincore()` on a data mapping. The old lock-file `mincore()` cache fields are
  retained only as legacy layout. Defrag's move path now keeps
  transaction-owned dirty buffers when a moved page is already dirty, otherwise
  fixes the first moved page in `page_auxbuf` and writes it through
  `dxb_write_pages()`. Multi-page overflow tails now use `dxb_copy_pages()` or
  the explicit read/write fallback; there is no mapped destination copy branch
  left in `defrag_move()`.
- Dirty-page write completion and meta write/update paths no longer flush or
  read back a data-file mapping. `iov_callback4dirtypages()` now only releases
  explicit shadow buffers after write completion, `iov_init()` no longer carries
  the mapped-coherency flag/timestamp, and the old
  `osal_flush_incoherent_mmap()` helper has been removed.
- Normal environment close now routes DXB descriptor teardown through
  `dxb_storage_close()`, which closes the data/dsync descriptors, clears the
  storage descriptor state, and releases explicit page-cache state through the
  storage abstraction. POSIX data-file stat probes now enter through
  `dxb_storage_t`, while lock and restore operations still take the DXB fd
  through storage descriptor helpers where fcntl locking requires a descriptor. `lck_destroy()`
  still uses its direct close sequence because it must preserve the existing
  fcntl lock restoration order, but the fds it closes are taken from
  `dxb_storage_t` before resetting the storage state.
- Data-page sync now routes directly through storage-fd sync;
  the data-file `dxb_msync()` wrapper and its `osal_msync()` mapping branch
  have been removed.
- `dxb_resize()` now uses `dxb_storage_resize_bytes()` for data-file
  current/limit/filesize management. The OSAL data-file remap helper
  `osal_mresize()` and its remap flags have been removed.
- `dxb_setup()` now routes all accepted environments through the no-data-mapping
  open path. `MDBX_WRITEMAP` is rejected at open time with `MDBX_INCOMPATIBLE`.
  The old mmap-incoherent-file workaround no longer auto-adds `MDBX_WRITEMAP`
  for accede-mode writable opens before this explicit-I/O rejection policy can
  run.
  The former `MDBX_COMPAT_DATA_MMAP=1` comparison mapping has been removed from
  data-file setup, and `MDBX_FORCE_NO_DATA_MMAP=1` is now only a compatibility
  test/profile selector for callers that still set it.
  The legacy pre-0.9 `MDBX_MAPASYNC` bit is normalized to
  `MDBX_SAFE_NOSYNC`/`MDBX_NOMETASYNC` instead of surviving as a mapped-write
  compatibility flag in accepted explicit-I/O environments.
  This keeps the lock-file mmap, opens the DXB file through the existing
  descriptors, initializes storage current/limit/filesize state without
  `osal_mmap()`, and skips mapped-only `madvise()`, sanitizer poisoning,
  `munlock()`, and dirty-page mmap coherency checks. `mdbx_env_set_geometry()`
  now treats `ENV_ACTIVE` plus the storage fd as the opened-env signal, so
  no-map environments preserve the
  current meta geometry instead of recomputing defaults. No-map readers also
  refresh storage `current` from the fd size before accepting meta heads whose
  root pages are beyond the previous local file view.
- Remaining data-file mmap cleanup is now concentrated in unreachable
  `MDBX_WRITEMAP` compatibility checks and comments/history around old mapped
  behavior. Lock-file mmap remains intentionally live for phase 1.

The practical conclusion is that writes are already closer to explicit I/O than
reads. The hard part is replacing process-wide stable page addresses with
transaction/cursor-owned pinned page buffers without changing public value
lifetime semantics.

## Target Internal Interfaces

Introduce a data-file storage abstraction before changing page callers:

```c
typedef struct dxb_storage dxb_storage_t;
typedef struct dxb_byte_io dxb_byte_io_t;
typedef struct dxb_page_io dxb_page_io_t;

int dxb_storage_open(MDBX_env *env, dxb_storage_t **out);
int dxb_storage_read_bytes(dxb_storage_t *storage, const dxb_byte_io_t *request,
                           void *dst);
int dxb_storage_read_page_io(dxb_storage_t *storage,
                             const dxb_page_io_t *request, page_t *dst);
int dxb_storage_write_bytes(dxb_storage_t *storage,
                            const dxb_byte_io_t *request, const void *src);
int dxb_storage_writev_bytes(dxb_storage_t *storage,
                             const dxb_byte_io_t *request,
                             const struct iovec *iov, size_t sgvcnt);
int dxb_storage_write_page_io(dxb_storage_t *storage,
                              const dxb_page_io_t *request, const page_t *src);
int dxb_storage_prefetch_pages(dxb_storage_t *storage, pgno_t pgno,
                               size_t npages);
int dxb_storage_sync(dxb_storage_t *storage, enum osal_syncmode_bits mode);
int dxb_storage_resize(dxb_storage_t *storage, size_t size_bytes,
                       size_t limit_bytes);
dxb_close_result_t dxb_storage_close(dxb_storage_t *storage, bool env_active);
```

The first backend should use `pread()`, `pwrite()`/`pwritev()`,
`fsync()`/`fdatasync()`, and `posix_fadvise()` where available. An io_uring or
platform async backend can then implement the same interface and block at
existing synchronous API boundaries.

Add an explicit page-cache/pin layer above storage:

```c
typedef struct page_ref {
  page_t *page;
  pgno_t pgno;
  size_t npages;
  unsigned flags;
} page_ref_t;

int page_pin(MDBX_txn *txn, pgno_t pgno, txnid_t visible_front,
             unsigned intent, page_ref_t *out);
void page_unpin(MDBX_txn *txn, page_ref_t *ref);
```

The exact representation can differ, but the contract must be explicit:

- Read pages are immutable for the lifetime of the pin.
- Dirty pages remain transaction-owned as they do today.
- Cursor stacks pin pages when a page is pushed and unpin when popped, reset,
  closed, cloned, or replaced by `page_touch()`.
- `MDBX_val` returned from read APIs is backed by pinned cursor/transaction
  pages until the same invalidation points that the public API already allows.
- Clean page-cache eviction is allowed only for unpinned pages.
- Dirty/spilled pages are never evicted through the clean read cache.

## Migration Work Packages

1. Storage skeleton

   Split the data-file fd/current/limit/filesize state from `osal_mmap_t`.
   Accepted opens now use the explicit storage path and no longer compile a
   selectable data-file mmap backend. The current DXB helpers still delegate
   directly to OSAL and still expose some mmap-era shell state. Explicit writes
   no longer expose the selected file
   handle at call sites; they use an internal data/meta channel. Full-page
   operations are now expressed as page-numbered reads/writes. Readahead and
   data-file tail discard/deallocation hints now go through
   `dxb_storage_advise_range()`, `dxb_storage_prefetch_pages()`, and
   `dxb_storage_discard_range()` with explicit page geometry, using fd-backed
   advice/discard for accepted environments. Data-page sync now carries an
   explicit page-range intent to storage, and explicit meta writes keep their
   follow-up sync decision at the policy site before submitting through storage.
   Defrag
   file-range copies now go through storage-owned page-copy helpers, and the
   portable non-compacting copy fallback reads through storage-owned byte reads.
   `MDBX_env` now has a
   `dxb_storage_t`
   block for data/meta handle selection and file-size/current/limit state.
   Route remaining mmap-era storage state, sync/advisory operations, explicit
   reads/writes, and size changes through a complete `dxb_storage_t` facade
   while behavior remains unchanged. Public env/txn validation,
   environment info file-stat reads, close-time sync eligibility, reader-slot
   binding, and geometry setup routing now use `ENV_ACTIVE` plus the storage
   data fd as the generic opened-env signal instead of a mapped address.

2. Meta buffers

   Load the three meta pages into env-owned buffers and make metadata selection,
   `meta_validate_copy()`, `meta_sync()`, and `meta_override()` operate on those
   buffers plus explicit reads/writes. Meta refresh must preserve the current
   retry logic and commit ordering. The current branch has started this by
   carrying logical meta slot identity through `meta_ptr_t`, using slot-derived
   offsets for explicit meta writes, and
   maintaining an env-owned shadow copy of the three meta pages via explicit
   DXB reads plus post-write shadow updates. Transaction start paths can now use
   refreshed shadow snapshots for the selected meta head, env-info metadata is
   assembled from refreshed shadow pages, and public read-only transaction
   observer paths (`mdbx_txn_info()`, `mdbx_txn_straggler()`,
   `mdbx_txn_refresh()`, and stale checks in `mdbx_txn_amend()`) now use the
   refreshed shadow view where they can return errors. Open-time meta
   validation/rollback, meta geometry/signature upgrade checks, recovery meta
   turn-over, non-writemap steady-meta wiping, default `meta_override()` DB
   identity selection without a mapped fallback, opened-environment
   `mdbx_env_set_geometry()` defaults and
   meta-copy setup, `mdbx_reader_list()` lag accounting, non-writemap
   `dxb_sync_locked()` target/head/tail selection, `txn_basal_commit()`
   pending-meta construction, GC steady-checkpoint selection, `env_sync()`
   observed-head/writer-troika setup, active-writer `env_sync()` head
   selection, `env_open()` meta-sync txnid seeding, environment warmup range
   selection, debug open logging, read-only MVCC oldest/recent discovery, and
   write-side MVCC oldest/laggard accounting now also have shadow-backed read
   paths. The shadow-tap refresh
   path now rereads the three meta pages through explicit I/O instead of using
   the data-file mapping as a freshness oracle. The latest cleanup also routed
   `dxb_sync_locked()`, `env_sync()`, GC/MVCC steady-checkpoint selection,
   commit metadata selection, `meta_sync()`, and `meta_override()` through
   shadow metadata plus explicit I/O, then removed the mapped metadata helper
   functions entirely.

3. Page pinning

   Change `pgr_t`/`page_get_*()` to return or carry a pin token, then update
   cursor stack ownership. This is the main API-lifetime risk. Do not remove
   mmap yet; first make pinned pages valid while the source still happens to be
   mmap-backed. The current branch has started this by adding `page_ref_t` to
   `pgr_t` and tagging mapped committed reads, dirty-list overrides,
   transaction allocations, loose-page reuse, and unspill copies with their
   source/ownership class. Cursor stacks now carry parallel `pgref[]` metadata
   and the central push/pop, reset/drown, clone/copy, search/deepen, cache
   fallback search, new-root, page-split/root-split cursor adjustment, subtree
   cutoff, compacting-copy stack pages, rebalance sibling setup, and
   nested subcursor refresh/update, root-collapse stack shifts, plus
   range/cutoff cursor copies, tree-drop stack restore, node move/merge cursor
   redirection, post-merge stack restore, and `page_touch()` replacement paths
   preserve it. Cursor stack set/copy/pop/reset/drown now go through explicit
   retain-before-release and release-all helper paths, and nested transaction
   cursor backups retain and release their saved stacks. The branch now also has
   a real page-cache owner/refcount scaffold: cache entries have owner, page,
   page-I/O descriptor, and pin counters; cursor refs can point at those
   entries; env reset releases the cache; and page validation can recognize
   cached read pages without a mapped-file address range. Overflow value reads
   now retain their page result in `MDBX_cursor.value_ref`, which gives
   non-stack `MDBX_val` data the same
   future pin lifetime hook as cursor stack pages. Short-lived `pgr_t` users now
   have explicit release/consume handling after stack/value transfer or
   transient use, including temporary rebalance clone cleanup. The branch now
   populates that cache from `page_get_committed()` by handing a checked
   `dxb_page_io_t` into the cache and submitting that descriptor through
   `dxb_storage_read_io()` on misses. Cursorless public reads now retain
   stack-local result refs in the transaction. The branch now also reuses clean
   committed pages for read-only transactions when the cached entry's snapshot
   id matches the transaction basis. Unpinned entries are always eligible, and
   pinned entries can be shared when they are branch/leaf pages or already
   expanded overflow spans; pinned single-page overflow headers stay unshared
   because materializing the full span can replace their buffer. Writer reads
   stay private in normal builds to avoid taking the global cache lock on the
   write path, but page-checking paths request tracked private entries so
   validation can still recognize explicit-I/O page buffers. The next
   page-pinning steps are to narrow
   transaction-retained refs to only pages that back returned values where
   possible, strengthen eviction and invalidation policy, and redesign the
   legacy fast key/value cache for a no-mmap backend.

4. Read cache

   Non-writemap committed-page reads now flow through page-cache entries created
   by `page_get_committed()`. Read-only transactions can reuse clean entries
   when both pgno and `txn_basis_snapshot()` match; this keeps pgno reuse and
   snapshot visibility conservative while avoiding repeated `pread()` calls
   inside one snapshot generation. Unpinned entries are always reusable, and
   pinned entries are reusable when they are branch/leaf pages or already
   expanded overflow spans. Pinned single-page overflow headers stay unshared
   because `page_cache_read_large()` can replace that entry buffer when it
   expands the cached page to the full overflow span. The reusable cache is
   protected by an environment fast mutex and capped by an env-owned runtime
   limit initialized from `MDBX_EXPLICIT_PAGE_CACHE_LIMIT`, which defaults to
   64 MiB. Writer reads normally use private cache entries that are freed when
   their last pin drops, avoiding cache-list scans and mutex traffic on the
   write path. Checking and page-validation paths can opt into tracking private
   entries so ownership checks still work. Cache entries store the same
   `dxb_page_io_t` descriptor used by storage reads/writes, so overlap
   invalidation and large-page expansion no longer maintain separate
   pgno/span/byte fields. `page_cache_read()` now builds a checked
   single-page request descriptor before cache lookup, and cache miss fills plus
   overflow-span materialization submit reads through descriptors. Committed
   page lookup and fast key/value cache materialization now build the descriptor
   before calling into the read cache. Fast key/value cache hits now materialize
   through the explicit page cache. Dirty/spilled transaction-page checks remain
   ahead of committed-page request construction. The
   remaining read-cache work is
   stronger eviction/invalidation policy, reducing over-retention in cursorless
   public reads, and moving more value lifetime decisions to stable cache-owned
   references that can be pinned before returning.

5. Resize, readahead, and sync

   Replace `osal_mresize()`, `dxb_msync()`, `madvise()` on mapped data, mincore
   checks, and mmap coherency verification with file-size management,
   page-cache invalidation, optional `posix_fadvise()`, and explicit sync state.
   The current branch has explicit-only data-file sync and file-size resize,
   fd-backed readahead and tail-discard hints, forced warmup reads, and
   no-data-mapping incore checks. Any residual WRITEMAP-only checks should stay
   unreachable behind the open-time incompatibility policy or be removed.
   Geometry and oldest-reader rules must stay unchanged.

6. Remove data-file mmap

   Accepted data-file opens no longer call `osal_mmap()`, the data-file
   `osal_munmap()`/`osal_mresize()`/`osal_msync()` paths are gone, and the
   `MDBX_env` data-file mapping field has been removed. Keep OSAL mmap code for
   `lck_mmap`.

7. Async backend

   Add async submission/completion behind `dxb_storage_t`. The public API still
   blocks at transaction and cursor boundaries, but reads, spills, dirty writes,
   prefetch, and fsync preparation can be batched internally.

## Correctness Gates

The public amalgamated source no longer contains the old full stochastic test
suite, so it is not enough to rely on this repository alone before claiming the
migration correct. Use the public gates continuously and run the private/full
MDBX suite whenever it is available.

Current public gates:

- `make test`
- `make test-assertion`, using `@cmake-assertion-build` for its CTest half so
  the default CMake build cache stays at the baseline checking level
- `c_api_nommap` and `c++_api_nommap`, registered in CTest with
  `MDBX_FORCE_NO_DATA_MMAP=1` plus `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`.
  The C and C++ example tests share a CTest resource lock because the examples
  use fixed demonstration database names and `ctest --parallel` can otherwise
  run the compatibility-mapped and forced-no-map variants concurrently.
- `c++_api` and `pcrf_simulator_smoke`, now running the default no-data-mapping
  path because the example workloads no longer request `MDBX_WRITEMAP`.
- `migration_smoke_nommap`, registered in CTest and run with
  `MDBX_FORCE_NO_DATA_MMAP=1`
- `migration_smoke_nommap_tinycache`, registered in CTest and run with
  `MDBX_FORCE_NO_DATA_MMAP=1` plus `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`
- No-data-mmap warmup API coverage: default warmup and force/oomsafe warmup
  must keep succeeding through explicit reads, while `MDBX_warmup_lock` is
  expected to return `MDBX_ENOSYS` because there is no data-file mapping to
  lock.
- Manual sync API coverage: after post-copy write updates, the normal smoke
  harness round-trips `mdbx_env_set/get_syncbytes()` and
  `mdbx_env_set/get_syncperiod()`, runs `mdbx_env_sync_poll()` and forced
  `mdbx_env_sync_ex()` in blocking and nonblocking forms, and verifies the
  synced records through a fresh read transaction. The lck-less read-only case
  also asserts that sync is rejected with `MDBX_EACCESS`.
- Explicit-profile on-disk compatibility: the normal smoke harness now saves and
  restores `MDBX_FORCE_NO_DATA_MMAP` and `MDBX_EXPLICIT_PAGE_CACHE_LIMIT`, creates
  a database with the default explicit backend profile, reopens and updates it
  with the forced/tiny-cache explicit profile, then verifies it again with the
  default profile. It also runs the reverse direction: tiny-cache create, default
  update, and tiny-cache verification.
- Lck-less read-only no-map coverage: on POSIX, the normal smoke harness makes
  the `MDBX_NOSUBDIR` `-lck` file inaccessible, opens the database with
  `MDBX_RDONLY | MDBX_EXCLUSIVE` while forcing no-data-mmap and a 64K
  explicit-cache cap, then verifies read-only transactions, env/txn observer
  APIs, reader-list behavior without lock slots, and `mdbx_reader_check()`.
- Top-level sanitizer/memcheck gates: `make test-asan`, `make test-ubsan`,
  `make test-memcheck`, and `make test-leak` configure the public CTest suite
  in isolated `@cmake-asan-build`, `@cmake-ubsan-build`,
  `@cmake-memcheck-build`, and `@cmake-leak-build` directories, with ASAN,
  UBSAN, ENABLE_MEMCHECK, and LeakSanitizer enabled separately. If the
  historical `test/stochastic.sh` harness is present they also run it;
  otherwise they fall back to the public CTest gates instead of failing on a
  missing script.
- Legacy top-level suite entries advertised by the thunk `Makefile` are wired
  back to public gates in the amalgamated tree: `make test-long` runs repeated
  CTest plus the stochastic/public fallback, `make test-long-assertion` runs
  the same shape against `@cmake-assertion-build`, `make test-ci` chains public,
  repeated, assertion, fault-enabled, ASAN, and UBSAN gates, `make test-ci-extra`
  runs the full `mdbx_migration_check`, and the old smoke aliases route to the
  migration smoke/fault/assertion/memcheck gates instead of failing with a
  missing GNUmake rule.
- Audit/checking gate: `make mdbx_migration_audit_ctest` configures
  `@cmake-audit-build` with `MDBX_CHECKING=3`, runs the public CTest suite with
  `MDBX_DBG_AUDIT=1`, and therefore covers the forced no-data-mmap CTest gates
  under the strongest checking build currently wired into the public harness.
- Memcheck build gate: `make mdbx_migration_memcheck_ctest` wraps
  `make test-memcheck`, configures `@cmake-memcheck-build` with
  `ENABLE_MEMCHECK=ON`, and runs the 15-test public CTest fallback when the
  historical stochastic harness is absent.
- Leak-check gate: `make mdbx_migration_leak_ctest` wraps `make test-leak`,
  runs the 15-test public CTest suite with LeakSanitizer enabled, and covers the
  forced no-data-mmap smoke, tiny-cache, stress, crash/restart, CLI roundtrip,
  pcrf simulator, and C/C++ example gates.
- Repeat gate: `make mdbx_migration_repeat_ctest` runs the 15-test public
  CTest suite three times with randomized scheduling and `--repeat
  until-fail:3`, giving the no-data-mmap cursor/cache lifetime tests a
  short stability pass before the heavier sanitizer and audit wrappers run.
- Extended stress gate:
  `make mdbx_migration_extended_stress_nommap_tinycache` repeats the max-wave
  randomized and crash/restart no-map stress binaries
  `MIGRATION_EXTENDED_STRESS_REPEAT` times, defaulting to `3`, with
  `MDBX_FORCE_NO_DATA_MMAP=1` and `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`.
  `mdbx_migration_check` includes this repeat gate.
- Assertion gate: `make mdbx_migration_assertion_ctest` configures
  `@cmake-assertion-build` with `MDBX_CHECKING=2` and runs the 15-test public
  CTest suite, so assertion coverage is isolated from the default public CTest
  cache and can be repeated without changing the baseline build directory.
- Focused ASAN build of `migration_smoke`
- Focused UBSAN build of `migration_smoke`
- Focused `MDBX_CHECKING=3` audit build of `migration_smoke` run with
  `MDBX_DBG_AUDIT=1`
- Tool roundtrip: `mdbx_load` seeds a NOSUBDIR database with printable,
  escaped-newline, and generated overflow-size records, then `mdbx_chk`,
  `mdbx_stat -e` and `mdbx_stat -a` with explicit nonzero overflow-page
  assertions, a copied/dropped/`mdbx_defrag -1`/checked defrag subcase, a
  populated overflow defrag subcase that copies the source, drops and reloads
  the main DB to force CLI churn, verifies overflow pages before and after
  `mdbx_defrag -3`, and compares dumps before and after that defrag,
  `mdbx_dump`, reload via `mdbx_load`, dump comparison, regular `mdbx_copy`,
  compact `mdbx_copy`, and `mdbx_chk` plus overflow-page `mdbx_stat` checks on
  the reloaded, copied, and compact-copied databases run as the
  `migration_tool_roundtrip` CTest/GNUmake gate. The same roundtrip also runs as
  `migration_tool_roundtrip_nommap` with `MDBX_FORCE_NO_DATA_MMAP=1`, and as
  `migration_tool_roundtrip_nommap_tinycache` with
  `MDBX_FORCE_NO_DATA_MMAP=1` plus `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`.
- Reduced `ioarena` smoke benchmark, with at least one warm-up run before
  recording numbers. The `mdbx_migration_bench_lazy` GNUmake target automates
  the current paired `lazy` run, invokes `ioarena` directly for the default
  explicit backend profile and the forced explicit profile, writes separate logs,
  and rejects missing `batch`/`crud`/`iterate`/`get`/`delete` summary rows or
  cursor/restore/error diagnostics. It also parses the paired summaries and
  fails when forced-profile throughput drops below default-profile throughput by
  more than the configured threshold: `MIGRATION_BENCH_MIN_RATIO`, default
  `0.60`, for batch/crud/delete and `MIGRATION_BENCH_READ_MIN_RATIO`, default
  `0.70`, for iterate/get. The `mdbx_migration_bench_lazy_repeat` target reruns
  that paired benchmark `MIGRATION_BENCH_REPEAT` times, defaulting to `3`, and
  writes separate per-repeat default and forced-profile logs so read/write
  throughput stability can be checked without rerunning the full correctness
  aggregate.
- Heavier no-data-mmap multiprocess stress:
  `mdbx_migration_smoke_stress_nommap_tinycache` builds the smoke harness with
  four readers, four writers, and eight commit waves, then runs it with
  `MDBX_FORCE_NO_DATA_MMAP=1` and `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`. The
  stress seed also creates a large-value overflow table; every writer wave
  updates, deletes, and inserts overflow records while pinned readers verify the
  old overflow snapshot and renewed readers verify the final one.
- No-data-mmap multiprocess crash/restart stress:
  `mdbx_migration_smoke_crash_stress_nommap_tinycache` builds the smoke harness
  with four pinned reader processes and forced crash-stress coverage. For each
  durable, `MDBX_NOMETASYNC`, `MDBX_SAFE_NOSYNC`, and `MDBX_UTTERLY_NOSYNC`
  mode it keeps readers on an old snapshot while a child commits and exits
  without environment cleanup, the parent reopens and verifies the committed
  state, another child exits with an active dirty writer transaction, the parent
  verifies abandoned dirty writes are absent and commits a recovery write, then
  readers renew and verify the final snapshot under the 64 KiB explicit
  page-cache cap.
- Deterministic test-only DXB fault injection:
  `mdbx_migration_fault_injection_nommap` and
  `mdbx_migration_fault_injection_nommap_tinycache` link the smoke harness
  against a private `MDBX_ENABLE_DXB_FAULT_INJECTION=1` static object, then
  run with forced no-data-mmap mode; the tiny-cache variant also sets
  `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`. They inject `filesize:EIO`,
  `filesize:EINTR`, `filesize-complete:EIO`,
  `filesize-complete:CANCEL`, `read:EIO`, `read:SHORT`, `read:EINTR`,
  `read-complete:EIO`, `read-complete:CANCEL`, delayed
  `read-complete:EIO@1`, `setsize:ENOSPC`, `setsize-complete:EIO`,
  `setsize-complete:CANCEL`, `copy-complete:EIO`,
  `copy-complete:CANCEL`, `writev:EIO`, `writev:SHORT`,
  delayed `writev:EIO@1`, delayed `writev:ENOSPC@1`, delayed
  `writev:CANCEL@1`, `writev-partial:EIO`, delayed
  `writev-partial:EIO@1`, delayed `writev-partial:ENOSPC@1`, delayed
  `writev-partial:CANCEL@1`, POSIX reversed-order delayed
  `writev:EIO@1`/`writev:ENOSPC@1`/`writev:CANCEL@1`, POSIX reversed-order delayed
  `writev-partial:EIO@1`/`writev-partial:ENOSPC@1`/
  `writev-partial:CANCEL@1`, POSIX outside-in delayed
  `writev:EIO@1`/`writev:ENOSPC@1`/`writev:CANCEL@1`, POSIX outside-in
  delayed `writev-partial:EIO@1`/`writev-partial:ENOSPC@1`/
  `writev-partial:CANCEL@1`, `writev-complete:EIO`,
  `writev-complete:CANCEL`, POSIX outside-in delayed
  `writev-complete:EIO@1`/`writev-complete:CANCEL@1`,
  `write-complete:EIO`, `write-complete:CANCEL`, `sync:EIO`,
  `sync:CANCEL`, `sync-complete:EIO`, `sync-complete:CANCEL`,
  `write:ENOSPC`, and `write:EINTR`, and verifies that failed commits leave
  the previously committed seed snapshot readable.
  Fault tokens `CANCEL`, `CANCELED`, and `CANCELLED` all map to `MDBX_EINTR`
  for this test backend. The `filesize:*` operation fails before accepting a
  data-file size probe, while `filesize-complete:*` reports an injected error
  after the OS size probe succeeds but before the explicit storage view is
  refreshed. The `read-complete:*` operation reports an injected
  error after the explicit read syscall succeeds, covering async-style metadata
  and data-page read completion failures before the page cache accepts a page.
  The `setsize-complete:*` operation reports an injected error after the
  storage-fd file-size syscall succeeds, covering async-style resize completion
  failures before create-time geometry/open setup or runtime geometry growth is
  accepted.
  The `copy-complete:*` operation reports an injected error after an in-file
  `copy_file_range()` succeeds during no-map overflow defrag, covering
  async-style page-copy completion failures before moved pages and metadata are
  accepted. This gate exposed a real bug: the defrag multi-page copy path had
  assigned the `dxb_copy_pages()` result and then returned success after leaving
  the copy loop; it now propagates the copy error immediately.
  The `writev-partial:*` operation
  writes the first segment of a queued scatter/gather item before returning the
  injected error, so recovery sees a real prefix write without an advanced meta
  page. The `writev-complete:*` and `write-complete:*` operations report the
  injected error after the full write syscall succeeds, modelling an async
  completion error that is observed before commit metadata advances. The
  `write-complete:*` cases also cover the direct write/sync phase used outside
  the queued scatter/gather batch. The `sync:*` operation fails before an
  actual data/meta flush syscall, while `sync-complete:*` reports a test-only
  completion error after that flush syscall succeeds. Together they cover
  immediate and async-style failed flush completion before a transaction can be
  accepted as committed. The `MDBX_TEST_DXB_WRITE_ORDER=reverse` hook submits
  queued POSIX writes from the end of the batch first, so delayed failures can
  leave a later queued write durable before an earlier queued operation fails.
  The
  `MDBX_TEST_DXB_WRITE_ORDER=outside-in` hook, also accepted as `outside_in`,
  `outoforder`, or `out-of-order`, writes the newest queued item, then the
  oldest, then alternates inward. This gives a second deterministic proxy for
  async-style completion reordering. POSIX runs also fork a
  child that hits delayed `writev` `EIO`/`ENOSPC`/`CANCEL`, `writev-partial`
  `EIO`/`ENOSPC`/`CANCEL`, selected completion-after-full-write failures,
  selected sync/flush failures, and selected reversed-order plus outside-in
  delayed write failures during commit and exits without environment cleanup;
  the parent then reopens, verifies the old snapshot, runs
  `mdbx_reader_check()`, and commits a follow-up write. The `@N` suffix lets
  `N` matching I/O operations complete before the injected failure, covering
  delayed queued write and delayed completion-error failures.
- Aggregate migration gate: `mdbx_migration_check` runs the GNUmake no-map
  smoke gates, the heavier no-map stress gates, the crash/restart stress gate,
  the extended repeated no-map stress gate, the public CTest gate, repeated
  public CTest, assertion public CTest, direct fault injection with and without
  the 64K explicit cache limit, the fault-enabled public CTest suite, ASAN,
  UBSAN, ENABLE_MEMCHECK, LeakSanitizer, and `MDBX_CHECKING=3` audit public
  CTest wrappers, all CLI tool roundtrips, and the repeated paired reduced
  `ioarena` benchmark in sequence. It fails with an explicit `ioarena`
  requirement message when performance coverage cannot be collected.

The smoke harness includes a process-specific token in its relative database and
copy target names so focused ASAN and UBSAN runs can share a working directory.
Older fixed-name builds should be run sequentially; otherwise one run can delete
another run's copy target and report a spurious `ENOENT` during copy
verification.

The top-level `test-asan`, `test-ubsan`, `test-memcheck`, `test-leak`,
`test-long`, `test-long-assertion`, `test-ci`, `test-ci-extra`, and legacy smoke
alias targets preserve or route into the historical `test/stochastic.sh` path
when that harness is present. This source drop does not include it, so
`build-stochastic` and `test-stochastic` now fall back to the public CTest
gates. The fallback keeps those targets useful in the amalgamated tree, but it
is still not a substitute for running the full stochastic harness whenever it is
available.
The fetched `origin/master`, `origin/devel`, `origin/stable`, `origin/lts/0.13`,
and archive refs visible in this worktree also do not contain the historical
`test/` directory.

Latest public gate run for the aggregate migration-check checkpoint:

- `cmake --build @cmake-ninja-build --target mdbx_migration_smoke`
- `MDBX_FORCE_NO_DATA_MMAP=1 LD_LIBRARY_PATH=@cmake-ninja-build
  @cmake-ninja-build/mdbx_migration_smoke`
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K
  LD_LIBRARY_PATH=@cmake-ninja-build @cmake-ninja-build/mdbx_migration_smoke`
- `gmake -f GNUmakefile mdbx_migration_smoke_nommap`
- `gmake -f GNUmakefile mdbx_migration_smoke_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_smoke_stress_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_smoke_randomized_stress_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_smoke_crash_stress_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_extended_stress_nommap_tinycache`,
  which repeats the randomized and crash/restart no-map stress binaries with
  the 64K explicit page-cache limit
- `gmake -f GNUmakefile mdbx_migration_fault_injection_nommap`
- `gmake -f GNUmakefile mdbx_migration_fault_injection_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_public_ctest`, which runs the 15-test
  public CTest set with default build settings
- `gmake -f GNUmakefile mdbx_migration_repeat_ctest`, which reruns the 15-test
  public CTest set three times with randomized scheduling
- `gmake -f GNUmakefile mdbx_migration_assertion_ctest`, which configures
  `@cmake-assertion-build` with `MDBX_CHECKING=2` and runs the 15-test public
  CTest set without mutating the default public CTest cache
- `gmake -f GNUmakefile mdbx_migration_fault_ctest`, which configures
  `@cmake-fault-build` with `MDBX_ENABLE_DXB_FAULT_INJECTION=ON` and runs the
  17-test fault-enabled public CTest set, including
  `migration_fault_injection_nommap` and
  `migration_fault_injection_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_tool_roundtrip_nommap_tinycache`
- `make test`, now including `migration_smoke_nommap`,
  `migration_smoke_nommap_tinycache`,
  `migration_smoke_stress_nommap_tinycache`, and
  `migration_tool_roundtrip_nommap_tinycache`, plus `c_api_nommap` and
  `c++_api_nommap`, in the CTest set
- `make test-assertion`, now including the same CTest set in the isolated
  `@cmake-assertion-build` directory
- `make test-asan`, which now runs the 15-test CTest set in `@cmake-asan-build`
  with ASAN enabled instead of failing on the missing stochastic harness
- `make test-ubsan`, which now runs the 15-test CTest set in
  `@cmake-ubsan-build` with UBSAN enabled instead of failing on the missing
  stochastic harness
- `make test-memcheck`, which now runs the 15-test CTest set in
  `@cmake-memcheck-build` with `ENABLE_MEMCHECK=ON` instead of failing on the
  missing stochastic harness
- `make test-leak`, which now runs the 15-test CTest set in
  `@cmake-leak-build` with LeakSanitizer enabled instead of failing on the
  missing stochastic harness
- `make mdbx_migration_audit_ctest`, which runs the 15-test CTest set in
  `@cmake-audit-build` with `MDBX_CHECKING=3` and `MDBX_DBG_AUDIT=1`
- `make mdbx_migration_memcheck_ctest`, which wraps `make test-memcheck` for
  the aggregate migration gate
- `make mdbx_migration_leak_ctest`, which wraps `make test-leak` for the
  aggregate migration gate
- `cmake --build @cmake-audit-build --target mdbx_migration_smoke`
- `MDBX_FORCE_NO_DATA_MMAP=1 MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K
  MDBX_DBG_AUDIT=1 LD_LIBRARY_PATH=@cmake-audit-build
  @cmake-audit-build/mdbx_migration_smoke`
- ASAN, UBSAN, and `MDBX_CHECKING=3` audit CMake builds of
  `mdbx_migration_smoke`
- focused ASAN, UBSAN, and audit runs of `mdbx_migration_smoke` with
  `MDBX_FORCE_NO_DATA_MMAP=1` and `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`
- ASAN, UBSAN, and `MDBX_CHECKING=3` audit CMake builds of the new
  `migration_tool_roundtrip_nommap_tinycache` CTest gate
- `MDBX_WRITEMAP` smoke coverage under the default no-data-mapping policy and
  `MDBX_FORCE_NO_DATA_MMAP=1`, both of which now assert that `mdbx_env_open()`
  returns exactly `MDBX_INCOMPATIBLE` before skipping the mapped-write exercise
- default/forced explicit-profile on-disk compatibility smoke coverage in both
  directions: default create, forced tiny-cache update, default verify; and
  forced tiny-cache create, default update, forced tiny-cache verify
- lck-less read-only no-map smoke coverage, proving that forced no-data-mmap
  reads, observer APIs, `mdbx_reader_list()`, and `mdbx_reader_check()` work
  when `MDBX_RDONLY | MDBX_EXCLUSIVE` opens without an LCK mapping
- `make mdbx_migration_tool_roundtrip_nommap`
- `make mdbx_migration_tool_roundtrip_nommap_tinycache`
- `gmake -f GNUmakefile mdbx_migration_bench_lazy`, which performs paired
  `NN=10000`/`BENCH_CRUD_MODE=lazy` default explicit and forced explicit
  `ioarena` runs without depending on the shared `bench-mdbx_*.txt` stamp, then
  scans both logs for missing summary rows and cursor/restore/error diagnostics,
  and enforces the configured forced/default throughput ratio thresholds.
- `gmake -f GNUmakefile mdbx_migration_bench_lazy_repeat`, which reruns the
  paired benchmark gate three times by default with separate per-repeat default
  and forced explicit logs.
- `make mdbx_migration_check`, which now serializes the GNUmake migration
  smoke, multiprocess stress, crash/restart stress, extended repeated no-map
  stress, public CTest, repeated public CTest, assertion public CTest, direct
  fault injection with and without the 64K explicit cache limit, fault-enabled
  public CTest, ASAN, UBSAN, ENABLE_MEMCHECK, LeakSanitizer, and
  `MDBX_CHECKING=3` audit public CTest wrappers, CLI roundtrip, and the repeated
  paired benchmark gate as one command.
- `make CMAKE_BUILD_DIR=@cmake-ninja-build
  CTEST_OPT="--output-on-failure" ctest`, which passed the 15-test CTest set.
- `make mdbx_migration_fault_ctest`, which passed the 17-test fault-enabled
  public CTest set in `@cmake-fault-build`, including both normal forced
  no-map and 64K-cache fault-injection entries.
- `gmake -n -f GNUmakefile mdbx_migration_bench_lazy` plus before/after
  `sha256sum` checks on both perf logs, confirming dry-run output no longer
  rewrites the benchmark logs.
  The default `BENCH_CRUD_MODE=nosync` is a negative-policy check for the forced
  no-map backend because this `ioarena` MDBX driver maps `nosync` to
  `MDBX_WRITEMAP | MDBX_UTTERLY_NOSYNC`.

The CLI roundtrip is now automated, but it is still a focused public gate rather
than a substitute for the full private MDBX test suite.

The migration smoke test covers the minimum public surface that is especially
likely to break during page-cache conversion:

- stable read snapshots while a writer commits;
- returned `MDBX_val` bytes remain valid inside the read transaction;
- cursorless public get results from `mdbx_get()`, `mdbx_get_ex()`,
  `mdbx_get_equal_or_great()`, `mdbx_cache_get()`, and
  `mdbx_cache_get_SingleThreaded()` remain valid across later public gets in the
  same read transaction;
- `mdbx_cache_get()` refreshes an overflow-value cache entry and
  `mdbx_cache_get_SingleThreaded()` then serves it as `MDBX_CACHE_HIT`; the
  cache-hit `MDBX_val` is held while a read-only cursor scans the full
  primary-table contents and is verified again afterward, which pressures
  explicit-cache eviction under the tiny-cache no-map gate;
- forced no-map tiny explicit page-cache cap
  (`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`) to pressure clean-page eviction while
  retained and cursor-returned values remain pinned;
- `mdbx_is_dirty()` reports clean for cursorless read values backed by explicit
  page-cache pages and dirty for values read back from non-writemap dirty pages
  in a write transaction;
- the file-backed smoke matrix includes `MDBX_VALIDATION`, forcing normal cursor
  work through full `page_check()` validation;
- `mdbx_env_warmup()` public API coverage for default warmup and forced
  OOM-safe warmup;
- public manual sync API coverage after real writes:
  `mdbx_env_set/get_syncbytes()`, `mdbx_env_set/get_syncperiod()`,
  `mdbx_env_sync_poll()`, forced blocking `mdbx_env_sync_ex()`, forced
  nonblocking `mdbx_env_sync_ex()`, and post-sync read verification; the
  lck-less read-only subcase also verifies `mdbx_env_sync_ex()` returns
  `MDBX_EACCESS`;
- cursor-returned value bytes remain valid while independent read-only cursor
  operations happen in the same transaction;
- same-cursor invalidation churn across primary and dupsort cursors: repeated
  `MDBX_SET_KEY`, `MDBX_FIRST`/`MDBX_NEXT`, `MDBX_SET_RANGE`,
  `MDBX_LAST`/`MDBX_PREV`, `MDBX_GET_BOTH`, `MDBX_NEXT_DUP`,
  `MDBX_PREV_DUP`, and `MDBX_NEXT_NODUP` movements over overflow values,
  deleted-key boundaries, and hundreds of duplicate values, with each newly
  current value verified after the previous returned value is invalidated;
- write-side same-cursor invalidation and rebalance churn in non-writemap mode:
  a write transaction updates an overflow value with `MDBX_CURRENT`, deletes and
  reinserts two primary-table ranges through one cursor to force dirty-page
  rebalance/split paths, deletes and reinserts individual dupsort values,
  deletes all duplicates for a key with `MDBX_ALLDUPS`, reinserts a smaller
  duplicate set, aborts the transaction, then verifies the committed primary
  and dupsort contents were restored exactly. A separate permuted write-churn
  pass holds clean primary-table, dirty in-transaction primary-table, and
  dupsort returned values while independent cursors delete/reinsert a permuted
  primary-table range, update several overflow values with `MDBX_CURRENT`,
  delete/reinsert a permuted slice of key-44 duplicates, create and remove a
  temporary key with `MDBX_ALLDUPS`, then aborts and verifies the original
  primary and dupsort state;
- write-transaction cursor-returned value bytes remain valid while independent
  primary-table and dupsort cursors move through unrelated records; the same
  check covers a dirty value read after an in-transaction update and a
  duplicate-subcursor value while the write transaction is later aborted;
- read-only cursor copy, reset, reposition, and renew preserve pinned stack/value
  ownership across copied cursors and transaction renewal;
- a DUPSORT/DUPFIXED/INTEGERDUP subdatabase with 768 duplicate values per key,
  covering duplicate-subcursor reads, `MDBX_GET_BOTH`,
  `MDBX_GET_BOTH_RANGE`, `MDBX_NEXT_DUP`, `MDBX_NEXT_NODUP`, cursor copy/value
  retention, `MDBX_CURRENT` duplicate deletion, and `MDBX_ALLDUPS` key
  deletion;
- same-cursor write churn on the primary table, including `MDBX_CURRENT`
  cursor update of an overflow value, `mdbx_cursor_del(MDBX_CURRENT)`, current
  cursor reposition validation after deletion, deletion of a 200-key range
  through the same cursor to force rebalance/GC pressure, reinsertion, and
  copy verification of the resulting generations;
- explicit overflow-table churn: `MDBX_RESERVE`, `mdbx_replace()` old-value
  retrieval from a clean overflow page, delete/reinsert waves, aborted and
  committed nested transactions with overflow values, `MDBX_stat`
  `ms_overflow_pages` verification, `mdbx_env_defrag()`, and post-defrag
  content verification;
- deterministic randomized larger-overflow churn: a permuted 32-record table
  with 8K-73K values, mixed reserve/put/replace/delete/insert waves, retained
  pinned-reader visibility for a deleted overflow value across writer churn,
  aborted and committed nested overflow writes, exact final record-count checks,
  and `MDBX_stat` overflow-page verification;
- multiprocess reader/writer MVCC: a child process holds a read transaction
  across two parent writer commits and active geometry growth, then renews its
  snapshot and observes the final metadata plus data;
- multiprocess stress with two concurrent reader processes holding pinned
  snapshots while two writer processes run multiple commit waves, active
  geometry growth, primary-table inserts/updates/deletes, separate overflow
  table insert/update/delete churn, reader-list checks before and after lag,
  reader renewal, and a post-stress writer commit that covers both tables;
- heavier no-data-mmap/tiny-cache multiprocess stress with four reader
  processes, four writer processes, and eight commit waves per writer, using
  the same pinned-snapshot, reader-list, renewal, and post-stress verification;
- deterministic randomized no-data-mmap/tiny-cache multiprocess stress with the
  same four-reader/four-writer/eight-wave shape, but each writer's committed
  update/delete/insert keys are driven through a distinct permuted wave schedule
  while final snapshot verification remains exact for both primary-table keys
  and overflow-table large values;
- no-data-mmap/tiny-cache multiprocess crash/restart stress that combines four
  pinned readers, reader-list lag checks, reopen verification after a child
  commits and exits without `mdbx_env_close()`, recovery after a second child
  exits with an active dirty writer transaction, reader renewal, and post-release
  writer verification across durable, `MDBX_NOMETASYNC`, `MDBX_SAFE_NOSYNC`, and
  `MDBX_UTTERLY_NOSYNC` modes;
- process-death/restart coverage for durable, `MDBX_NOMETASYNC`,
  `MDBX_SAFE_NOSYNC`, and `MDBX_UTTERLY_NOSYNC` modes: a child commits and
  exits without `mdbx_env_close()`, then another child exits with an active
  writer transaction after dirty-page pressure; the parent reopens, runs
  `mdbx_reader_check()`, verifies committed data remains visible, abandoned
  writes are absent, and a new writer can commit. The same mode matrix now also
  has parent-driven `SIGKILL` timing coverage: one child is killed after a
  successful commit but before environment close, another is killed after
  filling a spill-prone dirty transaction before commit, and restart checks
  prove the committed key survives, the dirty range is absent, and a follow-up
  writer can commit;
- lck-less read-only no-map open coverage: a forced no-data-mmap `MDBX_NOSUBDIR`
  database is created, its `-lck` file is made inaccessible on POSIX, and a
  `MDBX_RDONLY | MDBX_EXCLUSIVE` reopen verifies public reads, env/txn observer
  APIs, empty lock-slot enumeration through `mdbx_reader_list()`, and
  `mdbx_reader_check()` without relying on a lock-file mapping;
- large/overflow values;
- `MDBX_RESERVE`;
- forced dirty-page spilling in non-writemap mode;
- nested transactions in non-writemap modes, including a forced
  parent-spill-for-child path that verifies aborted child updates, committed
  child updates, unspill accounting, and outer-abort preservation of the
  original committed data;
- permuted temporary-table nested spill churn in no-data-mmap/tiny-cache mode:
  a 768-record parent update wave forces spill pressure, an aborted child mixes
  deletes/updates/inserts, a committed child verifies `mdbx_replace()` old-value
  bytes from spilled parent pages, checkpoints and restarts the same nested
  handle, then applies a post-checkpoint commit wave with exact final count plus
  spill/unspill counter verification;
- explicit geometry growth and active geometry shrink after GC/reuse, including
  post-shrink reads from the same environment;
- delete/reinsert/update waves that exercise GC/reuse and COW behavior;
- `mdbx_gc_info()`-backed GC retention/reuse coverage: a pinned reader keeps
  deleted overflow-heavy table pages non-reclaimable, releasing the reader makes
  them reclaimable, and a follow-up insert wave verifies bounded
  `pages_allocated` growth while checking the reused records;
- regular and compact public `mdbx_env_copy()` verification against copied
  contents;
- durable, validation, pure `MDBX_NOMETASYNC`, pure `MDBX_SAFE_NOSYNC`,
  combined lazy, and `MDBX_UTTERLY_NOSYNC` file-write modes;
- read-only transaction observer APIs: `mdbx_txn_info()`,
  `mdbx_txn_straggler()`, `mdbx_txn_refresh()`, and stale
  `mdbx_txn_amend()` rejection;
- reader-list enumeration and lag accounting while a read transaction is current
  and after it becomes stale;
- deterministic no-map DXB/commit-queue fault injection, with and without the
  64K explicit page-cache limit, for read `EIO`, short read, read `EINTR`,
  size-probe `EIO`/`EINTR`, completion-after-size-probe
  `EIO`/cancellation failures,
  completion-after-read metadata and data-page
  `EIO`/cancellation failures, delayed completion-after-read `EIO`,
  create/resize `ENOSPC`, create-time and runtime-growth
  completion-after-setsize `EIO`/cancellation failures, commit `writev` `EIO`,
  short commit write, delayed partial queued `writev` `EIO`, delayed partial
  queued `writev` `ENOSPC`, delayed queued `writev` cancellation, real prefix
  `writev-partial` `EIO`/`ENOSPC`/cancellation failures, deterministic
  reversed-order and outside-in delayed queued `writev`/`writev-partial`
  `EIO`/`ENOSPC`/cancellation failures, completion-after-full-write queued
  `writev` and direct `write` `EIO`/cancellation failures, outside-in delayed
  queued completion failures, completion-after-defrag-copy
  `EIO`/cancellation failures for no-map overflow defrag, crash/restart
  coverage for selected completion failures, sync and sync-completion
  `EIO`/cancellation failures with selected crash/restart coverage, commit
  `ENOSPC`, commit `EINTR`, and post-failed-commit reopen/read verification of
  the old snapshot;
- exact `MDBX_INCOMPATIBLE` rejection for `MDBX_WRITEMAP` under the default
  no-data-mapping policy, when the forced no-data-mmap backend is enabled, and
  when `MDBX_COMPAT_DATA_MMAP=1` requests the comparison mapping.

Additional gates needed before accepting the backend:

- true crash-consistency/power-loss matrix for durable, `MDBX_NOMETASYNC`,
  `MDBX_SAFE_NOSYNC`, and `MDBX_UTTERLY_NOSYNC`;
- longer randomized multi-process stress with crash/restart interleavings,
  additional process-kill timing variation, and run durations beyond the
  deterministic/permuted 4-reader/4-writer/8-wave, repeated public stress,
  process-kill, and crash-stress gates;
- longer duration and randomly seeded cursor invalidation coverage, deeper
  rebalance/root-shape diversity, and broader subcursor update/delete matrices
  beyond the deterministic and permuted write-side churn gates;
- longer duration and randomly seeded overflow churn with broader page-size,
  geometry, and CLI defrag/copy/load matrices beyond the deterministic
  populated-overflow and larger-overflow churn gates;
- deeper GC refund tests and longer randomized GC reuse/reclaim stress;
- longer duration and randomly seeded spill/unspill stress across broader
  nested checkpoint/rollback/commit and geometry matrices;
- broader fault injection for true async backend completion ordering/state
  beyond deterministic completion-after-read, completion-after-write,
  completion-after-sync, completion-after-setsize/resize, completion-after-copy,
  and reversed/outside-in POSIX submission hooks, true async backend cancellation
  state beyond deterministic queued-write
  cancellation, arbitrary partial multi-operation batches beyond deterministic
  queued-prefix and reordered writes, and broader crash/restart interleavings
  while faults are active.

## Performance Gates

Record baselines before replacing mmap reads and compare every milestone:

- `ioarena` `batch`, `crud`, `iterate`, `get`, and `delete`;
- read-heavy cursor scans and point lookups, since these lose the mmap page
  fault fast path;
- write-heavy non-writemap commits, which should stay close because the dirty
  write path is already explicit;
- page-cache memory footprint, pin count, hit ratio, dirty page count, spill
  count, and async queue depth once the new backend exists.

For the current tree after adding the thin DXB I/O facade, copy-path routing,
env-owned `dxb_storage_t` handle plus size state, the env-owned meta shadow
buffer, refreshed shadow snapshots for transaction starts, and shadow-backed
env-info plus read-only transaction observer metadata, open-time meta
validation/rollback reads, env-sync/open recent-meta reads, geometry plus
reader-list observer meta reads, and non-writemap commit/GC checkpoint meta
reads, plus `page_ref_t`
metadata on `pgr_t` page results and cursor stacks, and helper-routed
page-split/root-split cursor adjustments plus compacting-copy and rebalance
neighbor cursor-stack routing plus nested subcursor refresh/update and
root-collapse stack routing plus range/cutoff cursor copy routing and tree-drop
stack restore routing plus node move/merge cursor redirection, plus cursor ref
retain/release scaffolding and the merge-restore fix caught by the delete phase,
plus page-cache owner/refcount scaffolding, explicit non-writemap committed-page
reads through snapshot-safe reusable read-only cache entries and private writer
cache entries, cursor-held overflow value refs,
transaction-retained refs for cursorless public get/cache results, non-writemap
`mdbx_cache_get*()` data-file-offset refresh/materialization for cache hits,
`mdbx_is_dirty()` explicit page-cache/dirty-list pointer classification,
non-writemap mapped-pointer classification through explicit page-header reads,
defrag non-writemap dirty-page moves that keep transaction-owned buffers instead
of copying through the compatibility mapping,
`page_check()` explicit-I/O ownership validation for non-mapped cached, dirty,
and txn-owned buffers, retained audit-validation refs for branch-child pages
fetched through the explicit page cache,
explicit release/consume handling for short-lived `pgr_t` and transferred
`page_ref_t` results, LeakSanitizer-validated cleanup for temporary DBI/open,
stat, rename, public put/delete, and GC-info cursor stacks plus GC-row big-value
refs, and the sibling-search temporary-pop ref fix caught by ASAN, plus
shadow-backed environment warmup range selection, former sanitizer poison-tail
boundary selection that later became removable, removal of retired-page mapped
payload poisoning, and active-writer `env_sync()` head selection, debug open
logging, and write-side MVCC oldest/laggard accounting, plus
error-propagating read-only MVCC oldest/recent discovery, and removal of the
mapped freshness oracle from `meta_shadow_tap()`, plus opened-env/txn/read-slot/
info/geometry/fixed-option guards using `ENV_ACTIVE` and the storage data fd
instead of `dxb_mmap.base` as a generic open-state sentinel, plus
retry-protected cached initial shadow taps for read-transaction seize,
read-only transaction observer,
and reader-list loops, plus a no-data-mapping forced-warmup fallback that reads
through the DXB facade instead of touching `dxb_mmap.base`, plus a
`coherency_check()` root-txnid probe that first reads the GC/Main root page
header field through `dxb_read()` for normal non-writemap snapshots when the
explicit storage view covers the root page, with the later mapped root-page
fallback removed so root validation is now explicit-storage-only,
plus an
`env_is_page_incore()` no-map/out-of-range fallback and defrag dirty-page move
routing that keeps transaction-owned page buffers for normal non-writemap
transactions, with later defrag move cleanup removing mapped destination copies
entirely, plus
close-time DXB descriptor teardown through `dxb_storage_close()`,
plus explicit storage-fd sync and resize fallbacks that later became the only
accepted data-file path, plus default no-data-mapping `dxb_setup()` selection
for normal non-writemap environments, active no-map geometry handling, no-map
sanitizer/munlock and dirty-write coherency guards, storage-current refresh for
read transactions that observe writer-grown meta heads, and open-time rejection
of `MDBX_WRITEMAP`, plus
conservative
snapshot-safe read-only cache reuse capped by an env-owned runtime
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT` setting, selective sharing of pinned clean
read-only entries when their buffers cannot be replaced by overflow-span
materialization, and private writer cache entries to avoid write-path lock/list
overhead, defrag `dxb_copy_pages()` error propagation for in-file overflow page
copies, paired reduced `NN=10000` `ioarena` runs with `BENCH_CRUD_MODE=lazy`
produced:

| Workload | compatibility-mapped | forced no-data-mmap |
| --- | ---: | ---: |
| batch | `974.872ops/s` | `1.115Kops/s` |
| crud | `52.415Kops/s` | `59.656Kops/s` |
| iterate | `27.877Mops/s` | `20.970Mops/s` |
| get | `280.730Kops/s` | `282.176Kops/s` |
| delete | `66.476Kops/s` | `68.886Kops/s` |

The no-map/mapped ratios for this run were `1.144` batch, `1.138` crud,
`0.752` iterate, `1.005` get, and `1.036` delete.

The benchmark logs are saved as `perf-mdbx_10000_mapped-lazy.log` and
`perf-mdbx_10000_nomap-lazy.log`. The latest 2026-06-17 checkpoint was produced
by a direct `make -f GNUmakefile mdbx_migration_check` run: direct no-map smoke,
stress, crash/restart, extended repeated stress, fault injection with and
without the 64K explicit cache limit, public CTest, repeated public CTest,
assertion public CTest, fault-enabled public CTest, ASAN, UBSAN,
ENABLE_MEMCHECK, LeakSanitizer, `MDBX_CHECKING=3` audit public CTest wrappers,
all CLI tool roundtrips, and the paired reduced benchmark all passed. The
benchmark target invokes `ioarena` directly for both benchmark halves and scans
both logs for missing summary rows plus cursor/restore/error diagnostics, then
enforces no-map/mapped throughput ratios
of at least `0.60` for batch/crud/delete and `0.70` for iterate/get unless
overridden by make variables. An earlier cache-reuse
attempt that put normal writer reads on the tracked cache list dropped `crud` to
`1.093Kops/s`; keeping writer reads private recovered write throughput while
preserving validation tracking under `page_check()`. The previous
`BENCH_CRUD_MODE=nosync` numbers are no longer a valid forced no-map benchmark
because this `ioarena` MDBX driver opens `nosync` workloads with
`MDBX_WRITEMAP`, which the explicit-I/O backend now rejects with
`MDBX_INCOMPATIBLE`. For the same policy reason, the C++ example no longer runs
its `write_mapped_io` exercise; the file-I/O durability cases and the
API/nested-transaction checks still run.

A follow-up 2026-06-17 check isolated the historical `make test-assertion`
target's CTest half in `@cmake-assertion-build`: `make test-assertion
CTEST_OPT=--output-on-failure` passed its 15-test assertion CTest suite, a
default `make mdbx_migration_smoke_nommap` rebuilt and passed the root GNUmake
smoke with baseline flags, and `make mdbx_migration_public_ctest` reconfirmed
the default `@cmake-ninja-build` cache at `MDBX_CHECKING=0` with 15/15 CTest
passes.

The legacy target restoration was checked on 2026-06-17 by probing
`test-long`, `test-long-assertion`, `test-ci`, `test-ci-extra`,
`smoke-assertion`, `smoke-memcheck`, `smoke-fault`, `smoke-singleprocess`,
`test-singleprocess`, `memcheck`, and `mdbx_test` with GNUmake. Each now
resolves to a real rule. `make test-long` then passed the repeated 15-test CTest
gate and the public CTest fallback for the absent stochastic harness, both with
the default `@cmake-ninja-build` cache at `MDBX_CHECKING=0`. A subsequent
`make test-long-assertion` run passed the isolated 15-test assertion CTest gate
and the repeated public CTest fallback in `@cmake-assertion-build` with
`MDBX_CHECKING=2`. `make smoke-fault` also passed through the restored alias,
linking the fault-injection object and running the forced no-data-mmap fault
matrix. `make test-ci` then passed through the restored CI alias, covering the
default public 15-test CTest suite, repeated public CTest, isolated assertion
CTest, fault-enabled 16-test CTest including `migration_fault_injection_nommap`,
ASAN public CTest, and UBSAN public CTest. `make test-ci-extra` also passed
through the restored alias, covering the full `mdbx_migration_check` aggregate:
direct no-map smoke/stress/crash/fault runs, extended repeated no-map stress,
public/repeated/assertion/fault CTest gates, ASAN, UBSAN, audit, memcheck,
leak, tool roundtrips, and the paired reduced benchmark gate.

A follow-up 2026-06-17 focused smoke pass tightened the no-map compatibility
policy check: `exercise_mode()` now asserts that an optional
`MDBX_WRITEMAP` open returns exactly `MDBX_INCOMPATIBLE` when
`MDBX_FORCE_NO_DATA_MMAP=1` is enabled. A later WRITEMAP removal checkpoint
extended that exact rejection to default and compatibility-mapped opens as well.
`make -f GNUmakefile mdbx_migration_smoke_nommap_tinycache` passed this forced
no-map/tiny-cache path, and the later default `LD_LIBRARY_PATH=.
./mdbx_migration_smoke` run passed with ordinary `MDBX_WRITEMAP` skipped after
the exact rejection.

A subsequent 2026-06-17 focused smoke update expanded the normal file-mode
matrix with separate pure `MDBX_NOMETASYNC` and pure `MDBX_SAFE_NOSYNC` cases,
in addition to the existing durable, validation, combined lazy, utterly-nosync,
and optional writemap cases. `make -f GNUmakefile
mdbx_migration_smoke_nommap_tinycache`, `LD_LIBRARY_PATH=.
./mdbx_migration_smoke`, and `make -f GNUmakefile ctest
CTEST_OPT="--output-on-failure -R migration_smoke"` all passed; the CTest regex
rebuilt and ran the six registered migration smoke entries in
`@cmake-ninja-build`.

A later 2026-06-17 stress-gate update added
`mdbx_migration_extended_stress_nommap_tinycache`, a configurable repeated gate
over the max-wave randomized and crash/restart forced no-map stress binaries.
With default `MIGRATION_EXTENDED_STRESS_REPEAT=3`, `make -f GNUmakefile
mdbx_migration_extended_stress_nommap_tinycache` passed, covering three
randomized and three crash-stress no-map runs under the 64K explicit page-cache
limit. This does not replace true long stochastic/power-loss testing, but it
gives `mdbx_migration_check` a public repeat-stress gate.

A later 2026-06-17 fault-gate update added
`mdbx_migration_fault_injection_nommap_tinycache`, so the deterministic
DXB/commit-queue fault matrix also runs with
`MDBX_FORCE_NO_DATA_MMAP=1` and `MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`. The direct
`make -f GNUmakefile mdbx_migration_fault_injection_nommap_tinycache` target
passed, and `make -f GNUmakefile mdbx_migration_fault_ctest` rebuilt
`@cmake-fault-build` and passed the 17-test CTest suite, including both
`migration_fault_injection_nommap` and
`migration_fault_injection_nommap_tinycache`.

A later 2026-06-17 aggregate rerun of
`make -f GNUmakefile mdbx_migration_check` passed end to end after adding the
tiny-cache fault-injection gate. This covered direct forced no-map smoke,
stress, crash/restart, extended repeated stress, direct fault injection with and
without the 64K explicit cache limit, public/repeated/assertion/fault CTest
wrappers, ASAN, UBSAN, audit, memcheck, leak, all CLI roundtrips, and the paired
`ioarena` benchmark. The fault-enabled CTest suite passed 17/17, and the paired
benchmark ratios were `1.144` batch, `1.138` crud, `0.752` iterate, `1.005`
get, and `1.036` delete against configured thresholds of `0.60` for
batch/crud/delete and `0.70` for iterate/get.

A later 2026-06-17 performance-stability update added
`mdbx_migration_bench_lazy_repeat`, which reruns the paired mapped/default and
forced no-data-mmap lazy `ioarena` gate with separate per-repeat logs. With the
default `MIGRATION_BENCH_REPEAT=3`, the `mdbx_migration_bench_lazy_repeat`
GNUmake target passed all three samples. The observed no-map/mapped ratio
ranges were `1.010`-`1.136` batch, `0.980`-`1.129` crud, `0.785`-`1.363`
iterate, `0.909`-`1.002` get, and `0.959`-`1.053` delete; every sample cleared
the configured `0.60` batch/crud/delete and `0.70` iterate/get thresholds.

A later 2026-06-17 compatibility update added bidirectional mapped/no-map
on-disk checks to the normal smoke harness. The test creates a mapped/default
database and verifies plus updates it through forced no-data-mmap with
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, then verifies the update again through the
mapped/default backend. It then creates a forced no-data-mmap database, verifies
plus updates it through the mapped/default backend, and verifies the update
again through forced no-data-mmap. The
`mdbx_migration_smoke_nommap_tinycache` GNUmake target and a default
`./mdbx_migration_smoke` run both passed with this coverage included, followed
by `make -f GNUmakefile ctest` with the `migration_smoke` regex, which rebuilt
and passed the six registered migration-smoke CTest entries. The shared
environment-helper change was also checked with the
`mdbx_migration_fault_injection_nommap_tinycache` GNUmake target, which passed.

A later 2026-06-17 lck-less read-only update added POSIX smoke coverage for the
forced no-data-mmap path when the data file is readable but the lock file cannot
be opened. The harness creates a forced no-map database, removes access to the
`MDBX_NOSUBDIR` `-lck` file, then reopens with `MDBX_RDONLY | MDBX_EXCLUSIVE`
and verifies read-only records, env/txn observer APIs, `mdbx_reader_list()`'s
lock-free return path, and `mdbx_reader_check()`. The
`mdbx_migration_smoke_nommap_tinycache` and
`mdbx_migration_fault_injection_nommap_tinycache` GNUmake targets passed, a
default `LD_LIBRARY_PATH=. ./mdbx_migration_smoke` run passed and logged
`continue ./migration-smoke-...-lckless within without-lck mode`, and
`make -f GNUmakefile ctest CTEST_OPT="--output-on-failure -R migration_smoke"`
rebuilt and passed the six registered migration-smoke CTest entries.

A later 2026-06-17 manual-sync update added public sync API coverage to the
normal smoke harness. Each successful file-mode smoke now performs post-copy
write updates, round-trips sync byte/period thresholds, calls poll sync and
forced sync in blocking and nonblocking forms, then verifies the synced records
through a fresh read transaction. The lck-less read-only no-map subcase also
checks that manual sync is rejected with `MDBX_EACCESS`. The
`mdbx_migration_smoke_nommap_tinycache` and
`mdbx_migration_fault_injection_nommap_tinycache` GNUmake targets passed, and
`make -f GNUmakefile ctest CTEST_OPT="--output-on-failure -R migration_smoke"`
rebuilt and passed the six registered migration-smoke CTest entries.

A later 2026-06-17 performance revalidation reran
`make -f GNUmakefile mdbx_migration_bench_lazy_repeat` after the smoke-harness
sync coverage update. The default three paired mapped/default and forced
no-data-mmap lazy `ioarena` samples all passed. The saved per-repeat logs showed
no-map/mapped ratio ranges of `1.010`-`1.141` batch, `0.964`-`1.155` crud,
`0.746`-`0.985` iterate, `0.968`-`1.052` get, and `0.963`-`1.040` delete;
each sample cleared the configured `0.60` batch/crud/delete and `0.70`
iterate/get thresholds.

A later 2026-06-17 aggregate revalidation reran
`make -f GNUmakefile mdbx_migration_check` after the manual-sync and lck-less
read-only smoke additions. The aggregate passed direct forced no-data-mmap smoke
with the default and `64K` cache limits, heavy stress, randomized stress, crash
stress, extended stress, direct fault injection, public/repeated/assertion/fault
CTest gates, ASAN, UBSAN, audit, memcheck, leak checks, CLI tool roundtrips, and
the paired lazy benchmark. The benchmark sample reported no-map/mapped ratios of
`1.114` batch, `1.150` crud, `0.741` iterate, `1.036` get, and `1.050` delete,
again clearing the configured migration performance thresholds.

A later 2026-06-17 storage-teardown cleanup made `dxb_storage_t` the
authoritative DXB descriptor source for close/reset while preserving the
then-legacy `lazy_fd`/`fd4meta`/`dsync_fd` aliases for remaining compatibility
code. After the change, focused forced no-data-mmap smoke with
`MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`, deterministic no-map tiny-cache fault
injection, and the six registered `migration_smoke` CTest entries passed. A
paired `mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.138`
batch, `1.144` crud, `1.042` iterate, `0.982` get, and `1.044` delete, clearing
the migration performance thresholds.

A later 2026-06-17 recovery-open sentinel cleanup replaced another
`dxb_mmap.base` open-state check with the storage/active-state predicate. The
smoke harness now calls `mdbx_env_open_for_recovery()` on an already-open
environment, requires `MDBX_EPERM`, and then performs a normal write/read to
prove the rejected call did not leave the no-map env in recovery mode. Focused
forced no-data-mmap tiny-cache smoke, deterministic no-map tiny-cache fault
injection, and the six registered `migration_smoke` CTest entries passed. A
paired `mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.128`
batch, `1.140` crud, `0.872` iterate, `1.060` get, and `1.054` delete, clearing
the migration performance thresholds.

A later 2026-06-17 close-path sentinel cleanup removed the last data-map pointer
fallback from the public close-time writer-owner check. `mdbx_env_close_ex()`
now treats `ENV_ACTIVE` plus an open DXB storage descriptor as the generic signal
that the environment is open, while `env_close()` still unmaps only when a real
compatibility mapping exists. Focused forced no-data-mmap tiny-cache smoke,
deterministic no-map tiny-cache fault injection, and the six registered
`migration_smoke` CTest entries passed. A paired `mdbx_migration_bench_lazy`
run reported no-map/mapped ratios of `1.136` batch, `1.162` crud, `0.938`
iterate, `0.881` get, and `1.050` delete, clearing the migration performance
thresholds.

A later 2026-06-17 POSIX locking cleanup routed `check_fstat()`,
`lck_seize()`, `lck_downgrade()`, `lck_upgrade()`, SysV lock initialization,
and, at that checkpoint, `lck_setup()` read-only-filesystem probing through
the storage descriptor helpers instead of the then-legacy `lazy_fd` alias.
POSIX `lck_destroy()` still closes descriptors manually in the existing order so
fcntl lock restoration semantics stay unchanged, but it now chooses those
descriptors from `dxb_storage_t` before resetting storage state. Focused forced
no-data-mmap tiny-cache smoke,
deterministic no-map tiny-cache fault injection, and the six registered
`migration_smoke` CTest entries passed. A paired `mdbx_migration_bench_lazy`
run reported no-map/mapped ratios of `1.115` batch, `1.139` crud, `0.862`
iterate, `1.018` get, and `1.046` delete, clearing the migration performance
thresholds.

A later 2026-06-17 coherency checkpoint made GC/Main root-page `mod_txnid`
validation prefer explicit storage reads even when the compatibility data
mapping exists. The first narrow version made mapped non-writemap snapshots fail
the default `migration_smoke` CTest when a root page was inside the mapped file
view but beyond the storage-current size after shrink/remap behavior; the final
version keeps the explicit read-first path when storage-current covers the root
page and falls back to the mapped probe for WRITEMAP or mapped compatibility
snapshots outside the explicit storage view. Focused forced no-data-mmap
tiny-cache smoke, deterministic no-map tiny-cache fault injection, and the six
registered `migration_smoke` CTest entries passed after the correction. A paired
`mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.126` batch,
`1.177` crud, `0.727` iterate, `0.982` get, and `1.071` delete, clearing the
migration performance thresholds.

A later 2026-06-17 advisory/discard checkpoint restricted mapped `madvise()`
and `MADV_REMOVE` use in `dxb_advise_range()`/`dxb_discard_range()` to
`MDBX_WRITEMAP` compatibility. Normal non-writemap environments now take the
storage-fd advice/discard path even when the default compatibility mapping
exists, so mapped/default smoke runs no longer hide those helper paths behind
process-address hints. The six registered `migration_smoke` CTest entries
passed, including the default mapped smoke and forced no-data-mmap variants.
Deterministic no-map tiny-cache fault injection also passed. A paired
`mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.133` batch,
`1.167` crud, `0.901` iterate, `1.054` get, and `1.072` delete, clearing the
migration performance thresholds.

A later 2026-06-17 option open-state checkpoint replaced the remaining
`dxb_mmap.base` sentinel in `mdbx_env_set_option()` for `MDBX_opt_max_db` and
`MDBX_opt_max_readers` with the generic DXB-open/`ENV_ACTIVE` predicate. The
smoke harness now checks that both fixed-size options return `MDBX_EPERM` after
open, which catches the no-data-mmap case where no data mapping exists. Focused
forced no-data-mmap tiny-cache smoke, deterministic no-map tiny-cache fault
injection, and the six registered `migration_smoke` CTest entries passed. A
paired `mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.143`
batch, `1.163` crud, `0.959` iterate, `1.034` get, and `1.073` delete, clearing
the migration performance thresholds.

A later 2026-06-17 sanitizer-retire checkpoint first guarded the retired
dirty-page mapped payload poisoning in `page_retire_ex()` behind
`dxb_mmap.base`. A subsequent cleanup removed that mapped poisoning block
entirely. Forced no-data-mmap sanitizer/memcheck builds no longer call
`pgno2page()` just to poison a retired mapped page; malloc-backed dirty-page
cleanup remains handled by shadow-page release. Focused ASAN forced no-map
smoke, forced no-data-mmap tiny-cache smoke, deterministic no-map tiny-cache
fault injection, and the six registered `migration_smoke` CTest entries passed.
A paired `mdbx_migration_bench_lazy` run reported no-map/mapped ratios of
`1.114` batch, `1.165` crud, `1.008` iterate, `1.024` get, and `1.059` delete,
clearing the migration performance thresholds.

A later 2026-06-17 mapped-helper quarantine removed the generic
`pgno2page()`/`ptr2page()` helper names from the code and replaced them with
`mapped_pgno2page()`/`mapped_ptr2page()`, both of which assert that a data
mapping exists. At that checkpoint, remaining mapped pointer arithmetic was
visibly limited to WRITEMAP, mapped meta compatibility, mapped coherency
fallback, defrag WRITEMAP shortcuts, and compatibility dirty-pointer
classification. Focused forced
no-data-mmap tiny-cache smoke, deterministic no-map tiny-cache fault injection,
and the six registered `migration_smoke` CTest entries passed. A paired
`mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.129` batch,
`1.166` crud, `1.092` iterate, `0.965` get, and `1.068` delete, clearing the
migration performance thresholds.

A later 2026-06-17 mapped-metadata helper checkpoint renamed the generic
mapped metadata helpers to `MAPPED_METAPAGE()`, `meta_tap_mapped()`,
`meta_recent_mapped()`, `meta_prefer_steady_mapped()`, `meta_tail_mapped()`,
and `meta_ptr_mapped()`. The shared `meta_ptr_t` type remains neutral because
both mapped and shadow metadata selectors use it, while normal non-writemap
metadata selection continues through the shadow-buffer helpers. Focused forced
no-data-mmap tiny-cache smoke, deterministic no-map tiny-cache fault injection,
and the six registered `migration_smoke` CTest entries passed. A paired
`mdbx_migration_bench_lazy` run reported no-map/mapped ratios of `1.106` batch,
`1.160` crud, `0.998` iterate, `0.995` get, and `1.075` delete, clearing the
migration performance thresholds.

A later 2026-06-17 `mdbx_is_dirty()` mapped-pointer checkpoint routed normal
non-writemap mapped compatibility pointers through an explicit `dxb_read()`
page-header probe whenever storage-current covers the page. `MDBX_WRITEMAP`
and mapped compatibility snapshots beyond the explicit storage view still copy
the mapped header because dirty pages can be mmap-resident or outside the
storage-current view. Focused forced no-data-mmap tiny-cache smoke,
deterministic no-map tiny-cache fault injection, and the six registered
`migration_smoke` CTest entries passed. A paired `mdbx_migration_bench_lazy` run
reported no-map/mapped ratios of `1.135` batch, `1.170` crud, `0.740` iterate,
`1.079` get, and `1.079` delete, clearing the migration performance thresholds.

A later 2026-06-17 defrag dirty-page move checkpoint stopped copying
already-dirty source pages into the data mapping for normal non-writemap
transactions. Those moves now keep the transaction-owned dirty buffer in the DPL
under the remapped page number; at that checkpoint, only `MDBX_WRITEMAP`
compatibility still used a mapped destination copy. Focused forced
no-data-mmap tiny-cache smoke,
deterministic no-map tiny-cache fault injection, and the six registered
`migration_smoke` CTest entries passed. A paired `mdbx_migration_bench_lazy` run
reported no-map/mapped ratios of `1.114` batch, `1.179` crud, `1.338` iterate,
`0.953` get, and `1.074` delete, clearing the migration performance thresholds.

A later 2026-06-17 default no-data-mapping checkpoint made normal
non-`MDBX_WRITEMAP` opens select the explicit storage path by default, while
`MDBX_COMPAT_DATA_MMAP=1` keeps a temporary compatibility-mapped baseline for
comparisons and `MDBX_FORCE_NO_DATA_MMAP=1` continues to reject `MDBX_WRITEMAP`.
The default no-env smoke binary, focused forced no-data-mmap tiny-cache smoke,
deterministic no-map tiny-cache fault injection, and the six registered
`migration_smoke` CTest entries passed. A paired `mdbx_migration_bench_lazy` run,
with the mapped half explicitly using `MDBX_COMPAT_DATA_MMAP=1`, reported
no-map/mapped ratios of `1.132` batch, `1.160` crud, `1.141` iterate, `0.931`
get, and `1.090` delete, clearing the migration performance thresholds.

A later 2026-06-17 WRITEMAP removal checkpoint made `MDBX_WRITEMAP`
incompatible even when `MDBX_COMPAT_DATA_MMAP=1`, leaving the compatibility data
mapping only as a non-writemap comparison backend. The smoke harness now asserts
exact `MDBX_INCOMPATIBLE` rejection for default, forced no-map, and
compatibility-mapped WRITEMAP opens. The C++ example no longer runs
`write_mapped_io`, and the PCRF simulator no longer adds `MDBX_WRITEMAP`, so
their normal CTest entries exercise the default no-data-mapping path. Default
no-env smoke, explicit compatibility-mapped smoke, focused forced
no-data-mmap tiny-cache smoke, the six registered `migration_smoke` CTest
entries, deterministic no-map tiny-cache fault injection, and the full 15-test
public CTest suite passed. A paired `mdbx_migration_bench_lazy` run reported
no-map/mapped ratios of `1.128` batch, `1.166` crud, `0.865` iterate, `1.109`
get, and `1.078` delete, clearing the migration performance thresholds.

A subsequent 2026-06-17 aggregate gate run passed
`make -f GNUmakefile mdbx_migration_check` end to end with the WRITEMAP removal
policy in place. This covered direct no-map smoke, tiny-cache smoke, stress,
randomized stress, crash stress, extended stress, direct fault injection with and
without the tiny-cache limit, public CTest, repeated CTest, assertion CTest,
fault-enabled CTest, ASAN, UBSAN, audit CTest, memcheck-configured CTest,
LeakSanitizer CTest, default/no-map/tiny-cache CLI tool roundtrips, and the
paired lazy performance benchmark. The benchmark in that aggregate run reported
no-map/mapped ratios of `1.112` batch, `1.162` crud, `0.955` iterate, `0.988`
get, and `1.083` delete, clearing the configured migration thresholds.

A follow-up repeated performance gate passed
`make -f GNUmakefile mdbx_migration_bench_lazy_repeat` with the default three
paired mapped/no-map runs. The observed no-map/mapped ratio ranges were
`0.993..1.063` batch, `0.991..1.164` crud, `0.831..1.291` iterate,
`0.934..1.103` get, and `0.997..1.080` delete; the averages were `1.028`,
`1.052`, `1.012`, `1.037`, and `1.028`, respectively. The aggregate
`mdbx_migration_check` target now invokes this repeated performance gate instead
of a single benchmark sample.

A later 2026-06-17 full aggregate run passed
`make -f GNUmakefile mdbx_migration_check` after that target was wired to the
repeated performance gate. The run completed direct no-map smoke, tiny-cache
smoke, stress, randomized stress, crash stress, extended stress, fault
injection, public/repeated/assertion/fault/sanitizer/audit/memcheck/leak CTest
legs, CLI tool roundtrips, and the default three paired mapped/no-map benchmark
repeats. The observed no-map/mapped ratio ranges were `0.994..1.120` batch,
`0.988..1.168` crud, `0.762..1.319` iterate, `0.905..1.038` get, and
`0.995..1.087` delete; the averages were `1.040`, `1.054`, `1.027`, `0.980`,
and `1.028`, respectively.

A later 2026-06-17 explicit-only setup checkpoint removed the
`MDBX_COMPAT_DATA_MMAP=1` data-file backend selection path. `dxb_setup()` now
always initializes the data file through the explicit storage path, so accepted
opens no longer call `osal_mmap()` for the DXB file. The smoke harness now
checks default/tiny-cache explicit profile compatibility instead of
compatibility-mapped/no-map roundtrips, and the reduced `ioarena` migration
benchmark now compares forced/default explicit profiles while preserving the log
sanity and throughput-ratio gates. Default smoke, forced tiny-cache smoke, the
six registered `migration_smoke` CTest entries, deterministic forced tiny-cache
fault injection, and the full 15-test public CTest suite passed. The updated
paired `mdbx_migration_bench_lazy` gate reported forced/default ratios of
`1.123` batch, `1.182` crud, `0.966` iterate, `1.000` get, and `1.084` delete.

A later 2026-06-17 explicit-only resize/sync/teardown checkpoint removed the
remaining data-file remap/sync OSAL surface from active code. `dxb_sync_data()`
now delegates to storage-fd sync, the data-file `dxb_msync()` wrapper is gone,
`dxb_resize()` always manages geometry through `dxb_storage_resize()`, and the
unused `osal_mresize()` implementation and remap flags have been deleted.
Environment close, lock destruction, and after-fork cleanup no longer call
`osal_munmap()` for `env->dxb_mmap`; lock-file mmap teardown remains unchanged.
Focused default smoke, forced tiny-cache smoke, the six `migration_smoke` CTest
entries, deterministic forced tiny-cache fault injection, and the full 15-test
public CTest suite passed. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.123` batch, `1.169` crud, `0.917` iterate, `1.082`
get, and `1.082` delete.

A later 2026-06-17 mapped-tail sanitizer cleanup removed the data-file
`dxb_sanitize_tail()` hook, its `poison_edge` environment state, the open-time
poison-boundary initialization, and the commit-time mapped-tail ASAN/Valgrind
poisoning block. Transaction start/end and read-only reset paths no longer call
a sanitizer helper that depends on `env->dxb_mmap.base`; explicit page buffers
remain covered by their normal allocation lifetime and sanitizer instrumentation.
Focused default smoke, forced tiny-cache smoke, the six `migration_smoke` CTest
entries, deterministic forced tiny-cache fault injection, the full 15-test
public CTest suite, and the ASAN `test-asan` CTest fallback passed. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.148`
batch, `1.174` crud, `0.789` iterate, `1.033` get, and `1.085` delete.

A later 2026-06-17 mapped advice/locking cleanup removed data-file
`madvise()`/`posix_madvise()`/`MADV_REMOVE` branches from `dxb_advise_range()`
and `dxb_discard_range()`. Readahead, warmup, resize tail discard, open-time
tail discard, and commit-time shrink discard now use storage-fd advice or
explicit reads only. Data-file `MDBX_warmup_lock` now reports `MDBX_ENOSYS`
without trying to `mlock()` a missing mapping, and the legacy data-file
`mlocked_pgno`/`munlock_after()` accounting has been removed while the lock-file
layout field is retained for compatibility. Verification for this checkpoint
passed `mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs,
the six focused `migration_smoke` CTest entries, deterministic forced tiny-cache
fault injection, the full 15-test public CTest suite, and a focused ASAN
`migration_smoke` CTest run. The paired `mdbx_migration_bench_lazy` gate
reported forced/default ratios of `1.117` batch, `1.156` crud, `1.009` iterate,
`0.978` get, and `1.075` delete. Hygiene scans found no whitespace issues, no
removed mlock/mapped-advice helpers, no data-file mmap setup/teardown calls, and
only lock-file mmap setup/teardown references.

A later 2026-06-17 descriptor-ownership cleanup removed the legacy `lazy_fd`
macro that stored the data-file descriptor in `env->dxb_mmap.fd`. At that
checkpoint, `MDBX_env` carried an explicit `data_fd`, the environment DXB
descriptor helper returned that field directly, and `dxb_storage_bind()`
mirrored it into the explicit storage facade. Open,
probe, close/reset, POSIX fstat/incore checks, Windows DXB locking, spill writes,
and commit write-context selection now refer to `data_fd` instead of the mapping
shell. Hygiene scans found no `lazy_fd`, no `dxb_mmap.fd`, no data-file mmap
setup/teardown/sync/resize calls, and no whitespace issues. Verification passed
`mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the full
15-test public CTest suite, deterministic forced tiny-cache fault injection, and
a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.117`
batch, `1.168` crud, `0.809` iterate, `1.051` get, and `1.089` delete.

A later 2026-06-17 residency cleanup removed the remaining data-file
`mincore()` probe path, including the local bitmap helper, `mincore_fetch()`,
and the `MDBX_USE_MINCORE` CMake/config/version-reporting surface.
`env_is_page_incore()` now explicitly reports not-resident, so accepted callers
take their existing explicit-I/O fallback paths instead of probing a missing
data mapping. The lock-file `pgops.mincore` statistic and
`mincore_cache` layout fields remain as legacy compatibility fields only.
Verification passed direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, deterministic forced tiny-cache fault
injection, the full 15-test public CTest suite, and a focused ASAN
`migration_smoke` CTest run. The paired `mdbx_migration_bench_lazy` gate
reported forced/default ratios of `0.998` batch, `0.991` crud, `0.882`
iterate, `1.030` get, and `1.002` delete. Hygiene scans found no
`MDBX_USE_MINCORE`, no live data-file `mincore()` calls, no `lazy_fd`, no
`dxb_mmap.fd`, and no data-file mmap setup/teardown/sync/resize calls; the only
residual `mincore` source references are legacy lock-file/stat comments.

A later 2026-06-17 mapped-flush cleanup removed the remaining data-file
`osal_flush_incoherent_mmap()` path and the dirty-page mapped readback/coherency
state. Meta commit, meta wipe, and meta override paths no longer flush the old
data mapping after explicit `dxb_write()`/`dxb_write_pages()` calls, and
`iov_callback4dirtypages()` now releases written shadow buffers without trying
to compare them against `env->dxb_mmap.base`. `iov_init()` no longer takes a
mapped-coherency flag or stores a coherency timestamp; the remaining coherency
protection is the explicit meta/root validation path. Verification passed
direct default and forced tiny-cache smoke runs, the six focused
`migration_smoke` CTest entries, deterministic forced tiny-cache fault
injection, the full 15-test public CTest suite, and a focused ASAN
`migration_smoke` CTest run. The paired `mdbx_migration_bench_lazy` gate
reported forced/default ratios of `1.092` batch, `1.165` crud, `0.954`
iterate, `0.959` get, and `1.076` delete. Hygiene scans found no
`osal_flush_incoherent_mmap`, no mapped-coherency timestamp/flag, no
`MDBX_FORCE_CHECK_MMAP_COHERENCY`, no `lazy_fd`, no `dxb_mmap.fd`, and no
data-file mmap setup/teardown/sync/resize calls.

A later 2026-06-17 root-coherency cleanup removed the mapped root-page fallback
from `coherency_probe_root_txnid()`. Root `mod_txnid` validation now reads the
root-page txnid only through the explicit DXB storage facade, and
`coherency_fetch_head()` refreshes the storage size whenever the accepted meta
geometry extends past the cached storage-current size instead of gating that
refresh on `!env->dxb_mmap.base`. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.135`
batch, `1.181` crud, `1.014` iterate, `1.010` get, and `1.059` delete. Hygiene
scans found no mapped root-page coherency fallback and no data-file mmap
setup/teardown/sync/resize calls.

A later 2026-06-17 cache-WRITEMAP cleanup removed the legacy mapped fast-cache
path. `cache_get()` no longer performs the WRITEMAP-only early-exit b-tree walk,
`cache_materialize_entry()` no longer materializes cached values with
`dxb_mmap.base + offset`, and `cache_value_offset()` no longer subtracts public
value pointers from the mapped data-file base. The old mapped cache pointer
helper functions were removed as well. Cache hits now use the explicit page
cache/read path with transaction-retained pins, and `MDBX_WRITEMAP` cache
access returns `MDBX_INCOMPATIBLE`, which matches the open-time rejection
policy. Verification passed direct default and forced tiny-cache smoke runs, the
six focused `migration_smoke` CTest entries, deterministic forced tiny-cache
fault injection, the full 15-test public CTest suite, and a focused ASAN
`migration_smoke` CTest run. The paired `mdbx_migration_bench_lazy` gate
reported forced/default ratios of `1.115` batch, `1.179` crud, `0.847`
iterate, `0.841` get, and `1.071` delete. Hygiene scans found no mapped cache
helper symbols and no data-file mmap setup/teardown/sync/resize calls.

A later 2026-06-17 defrag mapped-destination cleanup removed the remaining
mapped copies from `defrag_move()`. Dirty moved pages now stay in their
transaction-owned dirty buffers and are reinserted into the dirty-list under the
new page number. Clean moved pages are fixed up in `page_auxbuf` and written
through `dxb_write_pages()`, while overflow tails use `dxb_copy_pages()` or the
explicit read/write fallback. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.133`
batch, `1.198` crud, `1.020` iterate, `1.003` get, and `1.105` delete. Hygiene
scans found no defrag references to `dxb_mmap.base`/`mapped_pgno2page()` and no
data-file mmap setup/teardown/sync/resize calls.

A later 2026-06-17 committed-page WRITEMAP cleanup removed the mapped read
result from `page_get_committed()`. A WRITEMAP transaction now gets
`MDBX_INCOMPATIBLE` instead of a `PAGE_REF_MAPPED` wrapper around
`mapped_pgno2page()`, so committed-page reads can no longer manufacture a
data-file mmap pointer. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.116`
batch, `1.167` crud, `1.058` iterate, `0.963` get, and `1.065` delete. Hygiene
scans found no `page_get_committed()`/committed-read mapped page return and no
data-file mmap setup/teardown/sync/resize calls.

A later 2026-06-17 allocation-WRITEMAP cleanup removed the mapped allocation
arm from `page_alloc_finalize()`. A WRITEMAP allocation attempt now returns
`MDBX_INCOMPATIBLE`, while accepted transactions allocate owned dirty page
buffers and retag successful results as `PAGE_REF_TXN_DIRTY | PAGE_REF_OWNED`.
This removes the allocation-time `mapped_pgno2page()` result and the legacy
mapped-prefault write branch. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.131`
batch, `1.181` crud, `1.273` iterate, `0.954` get, and `1.072` delete. Hygiene
scans found no allocation-time mapped page return and no data-file mmap
setup/teardown/sync/resize calls.

A later 2026-06-17 retire-sanitizer removal deleted the remaining
`page_retire_ex()` block that looked up the retired page with
`mapped_pgno2page()` to poison a mapped payload after `page_kill()`. Dirty-page
retirement now invalidates only the actual transaction-owned buffer before
`page_wash()`; accepted explicit-I/O environments no longer carry a mapped
sanitizer side path there. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.127`
batch, `1.177` crud, `1.047` iterate, `0.977` get, and `1.079` delete. Hygiene
scans found no `page_retire_ex()` mapped payload poisoning and no data-file mmap
setup/teardown/sync/resize calls.

The selective pinned-entry reuse checkpoint keeps the existing single-page
overflow-header guard. Focused no-map tiny-cache smoke, deterministic fault
injection, focused ASAN no-map tiny-cache smoke, and `mdbx_migration_check`
passed at this checkpoint. The public CTest suite now also registers
`c_api_nommap`, `c++_api_nommap`, and bounded `pcrf_simulator` mapped/no-map
smoke cases with `MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K`
where appropriate, so `make test` exercises the C/C++ examples plus a bounded
multi-DB insert/delete/stat workload against the forced no-data-mmap backend.
The example CTest entries also share a resource lock to keep their fixed
example database names from racing under parallel CTest runs.

Earlier reduced measurements around the read-only MVCC shadow-meta checkpoint
showed point lookup throughput dropping from `8.065Mops/s` to `1.783Mops/s`
after every shadow tap began rereading metadata through explicit I/O, then
recovering to `3.129Mops/s` with the retry-protected cached initial tap. The
next meta-buffer milestone still needs both a stronger mmap-free freshness
signal or refresh policy and larger benchmark runs with an explicit read
throughput gate before making performance claims.

The benchmark initially exposed a cursor stack restore regression in delete
after helper-based stack clearing; `page_merge()` now retains the saved top ref
across `cursor_pop()`/`tree_rebalance()` and restores from the saved page when
the computed destination stack slot has been cleared.

After explicit committed-page reads were enabled, ASAN exposed the sibling
search undo-pop path: `sibling()` restored `mc->top` after `MDBX_NOTFOUND` but
had already released the old child page's cache pin. `cursor_pop_keep_ref()` now
preserves that temporary stack slot ref until the old position is restored or a
new sibling child replaces it.

The `page_check()` ownership checkpoint exposed an audit-only dangling explicit
page-cache pointer: `cursor_validate()` fetched branch children with the legacy
pointer-only `page_get()` wrapper, then `page_check()` could fetch an overflow
page and release/reuse that unpinned child buffer. Audit validation now uses a
retained `pgr_t` for those child pages and releases it only after the child
`page_check()` completes.

The explicit overflow/defrag smoke exposed two more mmap-era lifetime
assumptions. First, `mdbx_replace()` returned old clean-page values directly
through `old_data`; this was stable with mmap, but cache-backed no-map values
could be released by the replacement cursor before the API returned. The replace
path now retains the cursor's cache-backed stack/value refs into the transaction
before updating, and releases the stack cursor on all exits. Second,
`walk_pgno()` used the pointer-only `page_get()` wrapper before
`mdbx_env_defrag()` walked the page, so ASAN caught a use-after-free in
`walk_page_type()`. The walker now keeps a retained `pgr_t` until the current
page traversal is complete.

Deterministic `copy-complete:*` fault injection later exposed a defrag
storage-error propagation bug in the no-map `copy_file_range()` path:
`defrag_move()` copied the remaining pages of a moved overflow span through
`dxb_copy_pages()`, left the loop, and then returned success even when the copy
reported an error. The path now returns that error immediately, and the fault
gate verifies that the previous overflow snapshot remains readable after the
failed defrag.

The reusable-cache checkpoint exposed two smaller ownership/sentinel traps.
First, ASAN caught a null-page path after cache lookup used `MDBX_RESULT_FALSE`
as an internal miss sentinel even though `MDBX_RESULT_FALSE` aliases
`MDBX_SUCCESS`; cache miss now uses `MDBX_RESULT_TRUE`. Second, validation caught
private writer cache pages that were invisible to `page_check()`; normal writer
reads remain private, but `page_get_inline()` asks for tracked private entries
when `z_pagecheck` is enabled.

The permuted nested checkpoint/spill smoke exposed two spill-list ownership
bugs. First, `nested_merge()` left the child `wr.spilled.list` pointing at a
list that had been transferred to the parent or merged and freed, so a restarted
nested checkpoint could reuse stale storage. Second, `nested_free()` did not
free a still-owned spill list for aborted nested children. `nested_merge()` now
clears child ownership after merge/transfer, and nested cleanup frees any
remaining child spill list.

A later storage-size/open-state sentinel cleanup removed two more hidden
`dxb_mmap.base` dependencies. `dxb_fetch_filesize()` now refreshes
the storage current, limit, and filesize values from the data fd whenever a
storage limit is known, without treating the absence of a data mapping as a
special mode. Nested resize-undo failure now marks the environment fatal
unconditionally after breaking the parent transaction, instead of branching on
whether the data file was mapped. Verification passed direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries,
deterministic forced tiny-cache fault injection, the full 15-test public CTest
suite, and a focused ASAN `migration_smoke` CTest run. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.130`
batch, `1.188` crud, `0.870` iterate, `1.025` get, and `1.075` delete. Hygiene
scans found no old `dxb_fetch_filesize()` data-map guard, no standalone
`if (!env->dxb_mmap.base)` branch for this cleanup, and no data-file mmap
setup/teardown/sync/resize calls.

A later pointer-classifier cleanup removed the legacy mapped-address branches
from `mdbx_is_dirty()` and `page_check()`. `mdbx_is_dirty()` now classifies
only explicit page-cache pages and transaction dirty-list pages, falling back
to the existing conservative dirty answer for unknown write-transaction
pointers. `page_check()` no longer computes an address offset from
`dxb_mmap.base`; audit validation now accepts subpages, shadowed pages,
transaction-owned dirty/spilled pages, and explicit page-cache or dirty-list
buffers. Verification passed direct default and forced tiny-cache smoke runs,
the six focused `migration_smoke` CTest entries, the same six entries under
`MDBX_CHECKING=3` with `MDBX_DBG_AUDIT=1`, deterministic forced tiny-cache
fault injection, the full 15-test public CTest suite, and a focused ASAN
`migration_smoke` CTest run. The paired `mdbx_migration_bench_lazy` gate
reported forced/default ratios of `1.095` batch, `1.170` crud, `1.010`
iterate, `0.971` get, and `1.074` delete. Hygiene scans found `dxb_mmap.base`
only in the remaining mapped metadata/helper island and found no data-file mmap
setup/teardown/sync/resize calls.

A later mapped-metadata removal checkpoint collapsed that remaining metadata
island onto the shadow/meta-I/O path. `dxb_sync_locked()`, `env_sync()`,
defrag GC loading, GC allocation checkpoint selection, read/write-side MVCC
oldest/laggard selection, `txn_basal_commit()`, `meta_unsteady()`,
`meta_sync()`, and `meta_override()` now assert the accepted no-WRITEMAP policy
and select shadow metadata only. Metadata writes go through explicit write
helpers plus storage-backed sync, and successful writes refresh the env-owned
shadow pages. The cleanup removed
`MAPPED_METAPAGE()`, `mapped_pgno2page()`, `mapped_ptr2page()`,
`meta_tap_mapped()`, `meta_ptr_mapped()`, `meta_recent_mapped()`,
`meta_prefer_steady_mapped()`, `meta_tail_mapped()`,
`meta_update_begin()`, `meta_update_end()`, and the old
`MDBX_NOMETASYNC_LAZY_WRITEMAP` constant. Verification passed
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the
full 15-test public CTest suite, deterministic forced tiny-cache fault
injection, focused ASAN `migration_smoke` CTest, and the same six focused
entries under `MDBX_CHECKING=3` with `MDBX_DBG_AUDIT=1`. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.136`
batch, `1.174` crud, `0.901` iterate, `1.236` get, and `1.086` delete. Hygiene
scans found no `dxb_mmap.base`, mapped metadata helpers, legacy mapped page
helpers, `MDBX_NOMETASYNC_LAZY_WRITEMAP`, data-file mmap setup/teardown, data
msync/resize, mapped coherency flush, or `lazy_fd` references in the checked
core files.

A later data-file mmap shell removal checkpoint removed `osal_mmap_t dxb_mmap`
from `MDBX_env` entirely. `dxb_storage_set_filesize()`,
`dxb_storage_set_current()`, and `dxb_storage_set_size()` now update only the
explicit storage state, and the unused `PAGE_REF_MAPPED` page-ref class was
removed. The only remaining OSAL mmap calls in the checked core files are
lock-file/reader-table operations and the OSAL mmap implementation itself.
Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.163`
batch, `1.176` crud, `0.934` iterate, `1.096` get, and `1.075` delete. Hygiene
scans found no `dxb_mmap`, `PAGE_REF_MAPPED`, mapped metadata helpers, legacy
mapped page helpers, data-file mmap setup/teardown/sync/resize calls, mapped
coherency flush, or `lazy_fd` references in the checked core files.

A later basal write-transaction WRITEMAP branch-removal checkpoint collapsed the
root write transaction lifecycle onto the explicit dirty-list path. `txn_write()`
now asserts that `MDBX_WRITEMAP` is absent. `basal_start_locked()` always
allocates a dirty list and initializes dirty-room accounting, and
`txn_basal_start()`/`txn_basal_end()` no longer propagate or preserve
`MDBX_WRITEMAP` from environment flags. `txn_basal_commit()` now requires the
dirty list, uses the dirty-list length for pure-commit detection, and always
enters the existing `iov_init()`/`txn_write()` explicit page-write path instead
of carrying the old no-dirtylist writemap accounting branch. Verification
passed direct default and forced tiny-cache smoke runs, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.133` batch, `1.166` crud, `1.004` iterate, `1.007`
get, and `1.078` delete. Hygiene scans found no `dxb_mmap`, `PAGE_REF_MAPPED`,
mapped metadata helpers, legacy mapped page helpers, data-file mmap
setup/teardown/sync/resize calls, mapped coherency flush, or `lazy_fd`
references in the checked core files.

A later writemap dirty-accounting cleanup removed the old
`writemap_dirty_npages` and `writemap_spilled_npages` transaction counters.
Write transactions now carry only explicit dirty-list and spilled-list state.
`txn_spill()`, `spill_slowpath()`, `page_dirty()`, `page_wash()`,
`refund_loose()`, and write-transaction info reporting now assert/use the dirty
list instead of branching to no-dirtylist writemap accounting. The spill
slowpath has only the explicit write-spilling route, and loose-page releases
return explicit shadow buffers unconditionally. Verification passed direct
default and forced tiny-cache smoke runs, the six focused `migration_smoke`
CTest entries, the full 15-test public CTest suite, deterministic forced
tiny-cache fault injection, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.112`
batch, `1.160` crud, `1.138` iterate, `0.994` get, and `1.076` delete. Hygiene
scans found no `writemap_dirty_npages`, no `writemap_spilled_npages`, no
`dxb_mmap`, no `PAGE_REF_MAPPED`, no mapped metadata helpers, no legacy mapped
page helpers, no data-file mmap setup/teardown/sync/resize calls, no mapped
coherency flush, and no `lazy_fd` references in the checked core files.

A later dirty-list/page-touch WRITEMAP cleanup removed the remaining
`MDBX_AVOID_MSYNC` alternatives from the explicit dirty-page helpers.
`txn_dpl_sort()`, `txn_dpl_search()`, `txn_dpl_exist()`,
`txn_dpl_append()`, `txn_dpl_check()`, `txn_dpl_sift()`, and
`txn_dpl_clear()` now assert that `MDBX_WRITEMAP` is absent and operate only
on explicit dirty lists. `page_touch_modifable()` no longer re-dirties a page
through the old writemap unspill branch; a modifiable page must already be in
the dirty list. `gc_merge_loose()`, `page_retire_ex()`, `defrag_move()`, and
`iov_page()` now follow the same explicit shadow-buffer invariant and release
shadow pages unconditionally where the old code skipped releases for writemap.
Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.164`
batch, `1.155` crud, `1.004` iterate, `0.991` get, and `1.063` delete.
Hygiene scans found no `writemap_dirty_npages`, no
`writemap_spilled_npages`, no `dxb_mmap`, no `PAGE_REF_MAPPED`, no mapped
metadata helpers, no legacy mapped page helpers, no data-file mmap
setup/teardown/sync/resize calls, no mapped coherency flush, and no `lazy_fd`
references in the checked core files. The only remaining `MDBX_AVOID_MSYNC`
hits in `mdbx.c` are sync-policy/build-info references outside this dirty-page
cleanup surface.

A later `MDBX_AVOID_MSYNC` removal checkpoint deleted the now-dead
configuration and sync-policy surface for data-file writemap/msync avoidance.
The CMake option, generated config define, internal macro block, build-info
field, and Windows durable-WRITEMAP direct-open probe were removed.
`MDBX_OPEN_DXB_OVERLAPPED_DIRECT` was also removed because no data-file path can
reach direct overlapped writes after `MDBX_WRITEMAP` is rejected by the
explicit-I/O backend. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.127` batch, `1.163` crud, `0.992` iterate, `0.972`
get, and `1.083` delete. Hygiene scans found no `MDBX_AVOID_MSYNC`, no
`AVOID_MSYNC`, no `MDBX_OPEN_DXB_OVERLAPPED_DIRECT`, and no `ior_direct` in the
checked core/build sources, plus no `dxb_mmap`, no `PAGE_REF_MAPPED`, no mapped
metadata helpers, no legacy mapped page helpers, no data-file mmap
setup/teardown/sync/resize calls, no mapped coherency flush, and no `lazy_fd`
references in the checked core files.

A later shared-envmode cleanup removed the remaining lock-file mode negotiation
treatment for data-file `MDBX_WRITEMAP`. `env_open()` no longer includes
`MDBX_WRITEMAP` in the shared `envmode` mask, accede mode can no longer inherit
that bit from another process, and a live or stale lock-file `envmode` carrying
`MDBX_WRITEMAP` is rejected as incompatible with the explicit-I/O storage
backend. The old mixed-writemap lazy-durability compatibility comment was
replaced with the current explicit-I/O invariant: writemap cannot reach this
backend, and strict shared-writer compatibility is only about durability modes
that affect steady checkpoints. `migration-smoke.c` now also asserts through
the public `MDBX_envinfo.mi_mode` field that shared envmode does not advertise
`MDBX_WRITEMAP` after normal explicit-I/O opens. Verification passed `make -f
GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.106` batch, `1.144` crud, `0.997` iterate, `0.983`
get, and `1.087` delete. Hygiene scans found no `MDBX_WRITEMAP` in the checked
`mode_flags` assignment and no stale mixed-writemap envmode commentary, while
the existing data-file mmap removal scans remain clean.

A later transaction-WRITEMAP propagation cleanup removed the remaining internal
transaction flag carry-through for data-file `MDBX_WRITEMAP`. Transaction begin
assertions, read-transaction start/clone setup, nested transaction creation, and
nested checkpoint flag preservation now all treat `MDBX_WRITEMAP` as impossible
after open-time rejection. Committed page reads, unchecked page lookup,
allocation finalization, cached-get, dirty-pointer classification, and
non-frozen `page_kill()` writes now follow the explicit-I/O invariant directly
instead of carrying active mapped-write alternatives. The default prefault-write
hook no longer derives a true result from the environment writemap flag, and
`txn_setup_primal()` no longer adjusts `front_txnid` for writemap mode. The
remaining `MDBX_WRITEMAP` references in this surface are public flag
normalization/rejection, lock-file mmap/OSAL support, legacy MAPASYNC
normalization, and invariant assertions. Verification passed `make -f
GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, the six focused `migration_smoke` CTest entries, the full 15-test public
CTest suite, deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.100` batch, `1.166` crud, `1.165` iterate, `0.987`
get, and `1.087` delete.

A later page-check WRITEMAP cleanup removed the last active `MDBX_WRITEMAP`
branch from `page_check()`. The validator now asserts the open-time no-writemap
invariant and always uses the explicit buffer ownership test for normal
non-subpages, so accepted pages must come from shadowed transaction pages,
transaction-owned dirty/spilled pages, subpages, page-cache buffers, or dirty
lists. The nearby data-file open notes were also updated to describe future
direct/no-buffering I/O as an explicit storage-backend policy rather than as a
data-mmap/WRITEMAP coherency issue. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.133` batch, `1.183` crud, `0.927` iterate, `1.005`
get, and `1.084` delete.

A later open-policy WRITEMAP cleanup removed the mmap-era
`MDBX_MMAP_INCOHERENT_FILE_WRITE` branch from `mdbx_env_open()`. Writable
`MDBX_ACCEDE` opens now keep their requested explicit-I/O flags instead of
being converted to `MDBX_WRITEMAP`, and direct `MDBX_WRITEMAP` requests still
return the documented `MDBX_INCOMPATIBLE` result. `migration-smoke.c` now has a
focused `MDBX_ACCEDE` open-policy check that opens a small writable environment
and verifies public `MDBX_envinfo.mi_mode` does not advertise `MDBX_WRITEMAP`.
Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.142`
batch, `1.149` crud, `1.031` iterate, `1.003` get, and `1.076` delete.

A later legacy-MAPASYNC cleanup changed `combine_durability_flags()` so the
pre-0.9 `DEPRECATED_MAPASYNC` bit is converted to `MDBX_SAFE_NOSYNC` whenever
the combined flags are not a real `MDBX_UTTERLY_NOSYNC`. The data-file setup
path no longer treats `DEPRECATED_MAPASYNC` as a separate reason to skip the
durable sync descriptor; accepted environments now carry the modern
`MDBX_SAFE_NOSYNC`/`MDBX_NOMETASYNC` state instead. `migration-smoke.c` now
checks both `mdbx_env_open()` and `mdbx_env_set_flags()` with the legacy numeric
bit, verifying that the deprecated bit is cleared and the explicit-I/O lazy
durability bits are present. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.125` batch, `1.160` crud, `0.949` iterate, `0.993`
get, and `1.079` delete.

A later public set-flags WRITEMAP cleanup made `mdbx_env_set_flags()` reject
`MDBX_WRITEMAP` with the same explicit-I/O `MDBX_INCOMPATIBLE` policy used by
`mdbx_env_open()`. This closes the inactive-env staging path where a caller
could previously save `MDBX_WRITEMAP` on the environment and only fail later at
open time. The active setter path also reports `MDBX_INCOMPATIBLE` for direct
`MDBX_WRITEMAP` enable attempts before any writer lock is taken. The new smoke
coverage verifies both inactive and active `mdbx_env_set_flags()` rejection,
checks that `mdbx_env_get_flags()` keeps `MDBX_WRITEMAP` clear after rejection,
and proves the same environment remains openable through the normal
explicit-I/O path without advertising `MDBX_WRITEMAP` in
`MDBX_envinfo.mi_mode`. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.127` batch, `1.178` crud, `0.825` iterate, `0.990`
get, and `1.082` delete.

A later public-header policy cleanup aligned `mdbx.h` with the explicit-I/O
branch. `MDBX_WRITEMAP` is now documented as a legacy writable data-mapping
mode that is rejected with `MDBX_INCOMPATIBLE` for writable explicit-I/O
environments; read-only opens are documented as ignoring it. The stale
`MDBX_MAPASYNC`/`MDBX_SAFE_NOSYNC` wording about asynchronous mmap flushes was
removed, dirty-page options now describe the explicit malloc-backed dirty-page
path, returned-value warnings no longer promise a SIGSEGV from read-only mapped
pages, and public geometry/envinfo text now describes database file sizing
rather than a data-file memory map. Lock-file mmap wording remains where it is
still part of phase 1. Verification passed `git diff --check`, `make -f
GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, and the full 15-test public CTest suite. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.101`
batch, `1.157` crud, `1.179` iterate, `0.949` get, and `1.058` delete. A
targeted header scan now finds only intentional `MDBX_WRITEMAP` policy text,
the deprecated `MDBX_MAPASYNC` alias note, and lock-file memory-mapping
wording.

A later dirty-write storage-channel cleanup moved the remaining
`iov_ctx` setup for spilled and commit-time dirty page writes away from
caller-selected file descriptors. `iov_ctx` now records a `dxb_io_channel`, the
storage layer resolves the concrete descriptor through `dxb_storage_iov_fd()`,
and durable POSIX dirty writes select the new `dxb_io_data_dsync` channel when
the O_DSYNC descriptor is usable. Windows overlapped dirty writes remain behind
the same storage helper. This keeps the synchronous `osal_ioring_*` batching
intact while making the dirty-write path depend on storage-channel intent
instead of raw `env->data_fd`/`env->dsync_fd` choices, which is a cleaner
boundary for a later async backend behind `dxb_storage_t`. Verification passed
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the
full 15-test public CTest suite, deterministic forced tiny-cache fault
injection, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.146` batch, `1.162` crud, `0.780` iterate, `0.993`
get, and `1.084` delete.

A later storage sync range-intent cleanup introduced `dxb_sync_range_t` and
`dxb_sync_data_range()` so data durability calls now carry an explicit page
range through the storage boundary. The current backend still performs the same
whole-file `fsync()` behavior, and `dxb_sync_locked()` intentionally keeps using
the full committed data range because the shared `unsynced_pages` state may
include earlier lazy writes outside the just-written dirty-page extent. The
dirty-write `iov_ctx.flush_begin`/`flush_end` tracking is now documented as
future range-sync input rather than a safe immediate narrowing signal.
Verification passed `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, and the stale data-file mmap symbol scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.117`
batch, `1.174` crud, `1.029` iterate, `1.012` get, and `1.080` delete.

A later page-cache write-invalidation cleanup added explicit page-cache
invalidation hooks for storage mutations. Ordinary data writes, vectored
writes, and file-range copies retire only private/non-reusable cache entries
while preserving snapshot-keyed reusable read-cache entries, because MVCC keeps
those pages immutable for the reader snapshot. Destructive truncate/remove
operations invalidate overlapping reusable entries too. This gives future async
write completions a cache-coherency boundary without sacrificing snapshot
read-cache performance. Verification passed the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.132` batch, `1.174` crud, `2.951` iterate,
`1.044` get, and `0.987` delete.

A later storage descriptor ownership cleanup removed the legacy
`MDBX_env.data_fd`/`dsync_fd`/`fd4meta` aliases. The data, durable-sync, and
meta descriptors now live directly in `dxb_storage_t`; open/setup, read-only
info probing, meta writes, sync decisions, close-after-fork handling, and
Windows lock helpers all select handles from the storage facade instead of
shadow environment fields. This keeps file-handle ownership in the same place
future async backends will hang their queues/completions. Verification passed
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the
full 15-test public CTest suite, deterministic forced tiny-cache fault
injection, focused ASAN `migration_smoke` CTest, `git diff --check`, the stale
data-file mmap symbol scan, and a legacy descriptor-alias scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.090`
batch, `1.164` crud, `1.144` iterate, `0.925` get, and `1.075` delete.

A later storage I/O-queue ownership cleanup moved the dirty-write
`osal_ioring_t` from `MDBX_env` into `dxb_storage_t`. Dirty page batching,
Windows overlapped data writes, and Windows DXB lock helper selection now use
the storage-owned queue/overlapped handle, while the existing close ordering
still destroys the queue before closing descriptors. This places queued write
state beside the data/meta/dsync handles so future async backends can attach
submission/completion state to the storage facade instead of the environment
root. Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct
default and forced tiny-cache smoke runs, the six focused `migration_smoke`
CTest entries, the full 15-test public CTest suite, deterministic forced
tiny-cache fault injection, focused ASAN `migration_smoke` CTest,
`git diff --check`, the stale data-file mmap symbol scan, and a legacy
environment `ioring` scan. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.134` batch, `1.167` crud, `1.038` iterate, `1.009`
get, and `1.080` delete.

A later storage page-cache ownership cleanup moved `page_cache_t`, the explicit
cache limit, and the page-cache mutex from `MDBX_env` into `dxb_storage_t`.
Reusable read-only cache entries, private tracked writer read entries, pointer
classification, range invalidation, prune/release, and env teardown now reach
cache state through the storage facade. This completes the first ownership pass
that places descriptors, file size, cached clean pages, and queued dirty writes
inside the same storage object for future async backends. Verification passed
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the
full 15-test public CTest suite, deterministic forced tiny-cache fault
injection, focused ASAN `migration_smoke` CTest, `git diff --check`, the stale
data-file mmap symbol scan, and a legacy root page-cache field scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.118`
batch, `1.146` crud, `0.939` iterate, `1.090` get, and `1.068` delete.

A later storage-owned page-cache helper cleanup gave tracked cache entries a
storage back-pointer and changed cache lock/unlock/prune/list traversal helpers
to operate on `dxb_storage_t` instead of root `MDBX_env`. Page-size and assertion
context still use the environment where needed, but cache ownership and
synchronization now flow through the storage facade. This narrows the boundary
future async backends must implement around cached clean pages and cache
invalidation. Verification passed `make -f GNUmakefile mdbx_migration_smoke`,
direct default and forced tiny-cache smoke runs, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and a legacy root cache-lock helper scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.122`
batch, `1.165` crud, `1.050` iterate, `0.962` get, and `1.077` delete.

A later storage-only page-cache invalidation cleanup removed the unused
`MDBX_env *` back-pointer from `page_cache_entry_t` and made
the page-cache range invalidator accept `dxb_storage_t` directly. Byte-range
invalidation still uses the environment for page-size conversion, but
destructive and write-completion invalidation now enter the cache list through
storage-owned state. This removes another cache-entry dependency on the
environment root and leaves environment usage in the page-cache layer limited to
page geometry/assertion context. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and stale page-cache env-owner/invalidation scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.122`
batch, `1.161` crud, `1.045` iterate, `0.933` get, and `1.097` delete.

A later storage descriptor-helper cleanup changed the descriptor routing helpers
to operate on `dxb_storage_t` instead of root `MDBX_env`: data/meta/dsync
channel selection, Windows overlapped data-handle detection, Windows DXB lock
handle selection, and dirty-write ioring fd selection now receive storage
directly. Higher-level callers still use the environment for page-size
conversion, lock events, stats, and policy assertions, but raw descriptor choice
no longer reaches through the environment root. Verification passed `make -f
GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, the six focused `migration_smoke` CTest entries, the full 15-test public
CTest suite, deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and a descriptor-helper env-argument scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.132`
batch, `1.150` crud, `0.831` iterate, `0.988` get, and `1.068` delete.

A later storage geometry-state setter cleanup changed the `filesize`,
`current`, and `limit` update helpers to operate on `dxb_storage_t` instead of
root `MDBX_env`. Fetch, resize, setup, checker, and remap-lock paths still use
the environment for page geometry, policy, and cache invalidation context, but
the actual storage size state is now mutated through the storage facade. This
keeps the file-size/current/limit ownership with the same object that owns DXB
descriptors, page cache state, and dirty-write queue state. Verification passed
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the
full 15-test public CTest suite, deterministic forced tiny-cache fault
injection, focused ASAN `migration_smoke` CTest, `git diff --check`, the stale
data-file mmap symbol scan, and a storage-setter env-argument scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.089`
batch, `1.159` crud, `0.997` iterate, `0.991` get, and `1.083` delete.

A later storage teardown ownership cleanup changed `dxb_storage_reset()`,
`dxb_storage_close()`, and full page-cache release to operate on
`dxb_storage_t` instead of root `MDBX_env`. Callers now pass the storage object
and an explicit active-environment bit for the pinned-cache teardown assertion,
while descriptor reset, file-size/current/limit clearing, and cached-page
release no longer reach through the environment root. This keeps close/reset
state transitions inside the same storage facade that will eventually own async
queue teardown and cache lifetime. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and storage-teardown env-argument scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.119`
batch, `1.159` crud, `1.005` iterate, `0.968` get, and `1.070` delete.

A later storage byte-I/O helper cleanup added `dxb_storage_read()`,
`dxb_storage_write()`, `dxb_storage_writev()`, and
`dxb_storage_copy_bytes()`. At that checkpoint, the env-shaped `dxb_read()`,
`dxb_write()`, page-write, page-writev, and `dxb_copy_pages()` wrappers still
kept environment-owned conversion, page-cache invalidation, and copy-page
policy, but raw DXB descriptor I/O entered through `dxb_storage_t`. At this
checkpoint,
the external export/copy path that copies from DXB to a caller-provided output fd
remained outside this in-file storage helper because it has two different
endpoints. This narrows the
future async backend surface to storage-owned byte operations without changing
public synchronous API boundaries. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and the storage byte-I/O routing scan. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.125`
batch, `1.180` crud, `0.982` iterate, `1.080` get, and `1.078` delete.

A later storage sync/size/advice helper cleanup added `dxb_storage_sync()`,
`dxb_storage_fetch_filesize()`, `dxb_storage_set_filesize_on_disk()`,
`dxb_storage_advise_range()`, `dxb_storage_discard_clean_range()`, and
`dxb_storage_set_readahead()`. At that checkpoint, the env-shaped wrappers still
owned pgop statistics, fault injection, page-cache invalidation, page-number
conversion, and lock-file readahead state, but the raw data-fd `fsync()`,
filesize, file-size update, `posix_fadvise()`/`F_RDADVISE`, and `F_RDAHEAD`
calls entered through `dxb_storage_t`. At this checkpoint, the remaining direct
`sendfile()` and `copy_file_range()` uses read from DXB into a caller-provided
external copy fd and stayed outside the in-file storage helpers. Verification
passed `make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, the six focused `migration_smoke` CTest entries, the full 15-test public
CTest suite, deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and storage sync/size/advice routing scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.100`
batch, `1.159` crud, `1.272` iterate, `0.956` get, and `1.078` delete.

A later storage lifecycle cleanup added `dxb_storage_init()` and
`dxb_storage_deinit()` so the explicit page-cache limit, page-cache mutex, and
cache teardown/reset sequencing are initialized and destroyed through
`dxb_storage_t` instead of root environment setup code. Environment creation and
close still own the surrounding DBI/remap/lock primitives, but storage-owned
cache state now has a single lifecycle entry/exit point that can later grow
async queue initialization and teardown. Verification passed `make -f
GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, the six focused `migration_smoke` CTest entries, the full 15-test public
CTest suite, deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and storage lifecycle ownership scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.010`
batch, `0.978` crud, `0.977` iterate, `0.968` get, and `1.008` delete.

A later dirty-write queue storage cleanup added `dxb_storage_write_queue()`,
`dxb_storage_create_write_queue()`, `dxb_storage_destroy_write_queue()`,
`dxb_storage_iov_channel_is_primary_data()`, `dxb_storage_has_dsync_fd()`, and
`dxb_storage_can_lazy_meta_sync_with_data()`. The existing `iov_*` and commit
code still choose channels and update sync accounting at their current policy
layer, but dirty-write queue access, queue lifecycle, primary-data
classification, dsync availability, and Windows overlapped lazy-meta-sync
eligibility now enter through `dxb_storage_t`. The remaining direct
`ioring.overlapped_fd` and `dsync_fd` touches are Windows/open-time descriptor
setup assertions and assignments. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, the stale data-file mmap symbol
scan, and dirty-write queue routing scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.124`
batch, `1.162` crud, `0.827` iterate, `1.065` get, and `1.069` delete.

A later external-copy/source-descriptor cleanup added
`dxb_storage_data_fd()`, `dxb_storage_current_size()`,
`dxb_storage_copy_to_fd()`, and `dxb_storage_sendfile_to_fd()`.
`mdbx_env_get_fd()`, warmup range clipping, storage-owned
read/write/sync/filesize/advice helpers, and env-copy `sendfile()`/
`copy_file_range()` fast paths now ask `dxb_storage_t` for the DXB source
descriptor instead of reaching through the raw storage data-fd member. The
remaining direct `data_fd` touches are storage lifecycle internals and
open/setup/incore checks. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration-tool copy roundtrips, deterministic forced tiny-cache fault
injection, focused ASAN `migration_smoke` CTest, `git diff --check`, stale
data-file mmap symbol scans, and source-descriptor routing scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.108`
batch, `1.168` crud, `0.820` iterate, `1.103` get, and `1.085` delete.

A later storage geometry observer cleanup added `dxb_storage_limit_size()`,
`dxb_storage_filesize()`, and `dxb_storage_contains_range()`. Public
`env_info`, `mdbx_chk` bookkeeping/printing, and coherency root probing now
read current size, limit size, file size, and range containment through
`dxb_storage_t` helpers instead of peeking at storage geometry fields. Resize,
open/setup, and commit growth paths still own the remaining direct geometry
state mutations and assertions. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, stale data-file mmap symbol scans,
and storage geometry observer routing scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.062`
batch, `1.176` crud, `0.875` iterate, `1.093` get, and `1.082` delete.

A later storage filesize-refresh cleanup added
`dxb_storage_current_from_filesize()`,
`dxb_storage_set_size_with_known_filesize()`,
`dxb_storage_set_limit_from_filesize()`, and
`dxb_storage_note_filesize()`. `dxb_fetch_filesize()`,
`dxb_setup_storage()`, and `dxb_storage_resize()` now delegate the
filesize/current/limit refresh calculation to `dxb_storage_t` helpers, and
header/meta validation now reads the cached DXB filesize through
`dxb_storage_filesize()`. Remaining direct geometry field accesses are resize,
open/setup, and commit invariants/mutations that still own policy decisions.
Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection, focused ASAN `migration_smoke` CTest, `git diff --check`,
stale data-file mmap symbol scans, and storage filesize-refresh routing scans.
The paired `mdbx_migration_bench_lazy` gate was rerun after a noisy first
attempt and then reported forced/default ratios of `1.090` batch, `1.039` crud,
`1.895` iterate, `1.625` get, and `1.019` delete.

A later storage invariant helper cleanup added
`dxb_storage_current_within_limit()`, `dxb_storage_current_is()`,
`dxb_storage_current_covers()`, `dxb_storage_limit_is()`,
`dxb_storage_filesize_is()`, and `dxb_storage_filesize_covers()`. Resize,
readahead, open-time tail discard, write/read transaction start assertions, and
map-resize checks now validate current/limit/filesize relationships through
`dxb_storage_t` instead of reading the raw current, limit, or filesize members
directly. Direct current/limit/filesize field access is now confined to
storage helper internals. Verification passed `make -f GNUmakefile
mdbx_migration_smoke`,
direct default and forced tiny-cache smoke runs, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, focused ASAN
`migration_smoke` CTest, `git diff --check`, stale data-file mmap symbol scans,
and storage invariant/direct-field routing scans. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.149`
batch, `1.159` crud, `1.180` iterate, `0.923` get, and `1.072` delete.

A later descriptor lifecycle helper cleanup added `dxb_storage_open_data()`,
`dxb_storage_open_dsync()`, Windows-only `dxb_storage_open_overlapped()`, and
parking helpers for the storage-owned data, dsync, and overlapped descriptors.
Preopen snap-info, normal environment open, dsync-meta descriptor selection,
Windows overlapped open setup, close-time overlapped assertions, and POSIX
fork-recovery dsync close handling now go through `dxb_storage_t` helpers instead
of reaching into raw descriptor members. Direct descriptor-member access is now
confined to storage helper internals and documented lock-file mmap paths.
Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, focused ASAN `migration_smoke` CTest, `git diff --check`,
stale data-file mmap symbol scans, and descriptor-field routing scans. The
paired `mdbx_migration_bench_lazy` gate reported forced/default ratios of
`1.142` batch, `1.169` crud, `1.181` iterate, `0.995` get, and `1.080` delete.

A later cache invalidation facade cleanup added
`dxb_storage_invalidate_cached_pages()` and
`dxb_storage_invalidate_cached_bytes()`. Explicit write, writev,
`copy_file_range()` page moves, file truncation, and destructive discard
callers now invalidate cached pages through `dxb_storage_t` instead of passing
through an `MDBX_env`-shaped helper or casting from `const MDBX_env` back to
storage. Verification passed `make -f GNUmakefile mdbx_migration_smoke`, direct
default and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`,
the six focused `migration_smoke` CTest entries, the full 15-test public CTest
suite, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, focused ASAN `migration_smoke` CTest, `git diff --check`,
stale data-file mmap symbol scans, and cache invalidation routing scans. The
paired `mdbx_migration_bench_lazy` gate reported forced/default ratios of
`1.135` batch, `1.149` crud, `1.263` iterate, `0.945` get, and `1.052` delete.

A later page-cache pointer-classification cleanup replaced the remaining
env-shaped cached-page pointer helpers with
`dxb_storage_cached_page_from_ptr()` and
`dxb_storage_cached_page_contains()`. Cached-page scans now take
`dxb_storage_t` plus explicit page geometry, while dirty-list pointer
classification keeps using transaction-owned dirty buffers and page-size
geometry from the owning transaction environment. This confines the last
`const MDBX_env`-to-storage casts in pointer classification to storage-shaped
helpers and keeps the page-cache list behind the storage facade. Verification
passed `git diff --check`, stale data-file mmap symbol scans, cache pointer
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.123`
batch, `1.168` crud, `0.964` iterate, `0.964` get, and `1.070` delete.

A later byte-I/O storage facade cleanup split the raw OS byte operations into
`dxb_storage_pread()`, `dxb_storage_pwrite()`, and
`dxb_storage_pwritev()`, then made `dxb_storage_read()`,
`dxb_storage_write()`, and `dxb_storage_writev()` own the fault-injected
storage operations. The env-shaped `dxb_read()` and `dxb_write*()` wrappers now
only supply page geometry and cache invalidation policy around storage-level
byte I/O. This keeps raw descriptor selection, raw byte submission, and
test-fault sequencing closer to the `dxb_storage_t` facade that an async backend
will replace. Verification passed `git diff --check`, stale data-file mmap
symbol scans, storage byte-I/O routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.135` batch, `1.174` crud, `0.957` iterate, `0.981`
get, and `1.083` delete.

A later sync/filesize storage facade cleanup split raw `fsync` and file-size
extension/truncation into `dxb_storage_fsync()` and
`dxb_storage_fsetsize()`, then made storage-shaped `dxb_storage_sync()` and
`dxb_storage_set_filesize_on_disk()` own the related fault-injection sequence.
At that checkpoint, `dxb_fsync()` kept only the environment pgop statistic
update before delegating to storage sync, while `dxb_set_filesize()` kept cached
filesize state and page-cache invalidation around the storage-level resize
operation.
This moves durable-sync and geometry-changing file operations closer to the same
storage facade boundary as byte read/write submission. Verification passed
`git diff --check`, stale data-file mmap symbol scans, storage sync/filesize
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.140`
batch, `1.158` crud, `1.104` iterate, `0.958` get, and `1.087` delete.

A later copy-range storage facade cleanup split same-file
`copy_file_range()` page moves into a raw `dxb_storage_copy_file_range()` call
and a storage-shaped `dxb_storage_copy_bytes()` operation that owns `copy` and
`copy-complete` fault injection plus short-copy normalization. At that
checkpoint, `dxb_copy_pages()` still converted page numbers to byte offsets,
delegated the copy to storage, and invalidated the destination cache range. The
fault-injection smoke table now
also covers pre-copy `copy:EIO` and `copy:CANCEL` defrag failures alongside the
existing post-copy cases. Verification passed `git diff --check`, stale
data-file mmap symbol scans, storage copy routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite, deterministic forced tiny-cache
fault injection with the expanded copy cases, `cmake --build @cmake-asan-build`,
and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.126`
batch, `1.169` crud, `1.013` iterate, `0.973` get, and `1.078` delete.

A later env-copy storage facade cleanup split external destination fast paths
into raw `dxb_storage_copy_file_range_to_fd()` and
`dxb_storage_sendfile_to_fd_raw()` helpers plus storage-shaped
`dxb_storage_copy_to_fd()` and `dxb_storage_sendfile_to_fd()` classifiers. Those
storage operations now normalize copied, unavailable, cross-device, EOF, and
error outcomes from `copy_file_range()` and `sendfile()`. `copy_asis()` keeps
the MVCC reader parking/unparking and portable fallback loop, but no longer
decodes raw syscall results from the DXB source fast paths. Verification passed
`git diff --check`, stale data-file mmap symbol scans, storage external-copy
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip copy coverage, deterministic forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.063` batch, `1.154` crud, `0.987` iterate, `0.977`
get, and `1.073` delete.

A later storage probe cleanup added `dxb_storage_stat()` and
`dxb_storage_check_incore()` so POSIX data-file liveness/mode probes and the
in-core filesystem check enter through `dxb_storage_t`. Environment close,
open-time lock-file mode inheritance, DXB/LCK validation, and SysV IPC
permission setup no longer call `fstat()` directly on the DXB descriptor, and
`env_open()` no longer calls `osal_check_fs_incore()` directly on the data-fd.
Lock-file mmap and lock-range operations remain at their existing layer. This
keeps descriptor probing and filesystem capability checks beside the storage
facade that future explicit async backends must emulate. Verification passed
`git diff --check`, stale data-file mmap symbol scans, storage probe routing
scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite including
migration tool roundtrip copy coverage, deterministic forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.136` batch, `1.176` crud, `0.829` iterate, `1.045`
get, and `1.074` delete.

A later storage file-info/capability cleanup added
`dxb_storage_fetch_sysinfo()` and `dxb_storage_check_readonly()`. `env_info_sys()`
now asks the storage facade for data-file size/allocation/I/O-block metadata
instead of decoding the DXB descriptor directly, while `lck_setup()` checks
read-only filesystem state through storage before deciding whether it can
continue without a lock file. The public `mdbx_env_get_fd()` and lock-range
code still expose/use the data fd where that is the contract or lock primitive,
but data-file metadata and filesystem capability probes are now part of the
storage boundary future async backends must emulate. Verification passed `git
diff --check`, stale data-file mmap symbol scans, storage file-info/read-only
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip copy coverage, deterministic forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.119` batch, `1.154` crud, `1.022` iterate, `0.998`
get, and `1.080` delete.

A later POSIX DXB lock-routing cleanup added `dxb_storage_lock_op()` and
`dxb_storage_setlk_with3retries()` as storage-shaped wrappers over the existing
fcntl lock helpers. DXB lock ranges in `lck_seize()`, `lck_downgrade()`,
`lck_upgrade()`, and the DXB exclusive/restore checks in `lck_destroy()` now go
through `dxb_storage_t` instead of carrying a local raw data-file descriptor.
Lock-file mmap and lock-file descriptor locking remain unchanged for phase 1.
At this checkpoint, the public `mdbx_env_get_fd()` contract and
`lck_destroy()` close-order comparison still intentionally used the DXB fd
directly. Verification passed `git diff --check`, stale data-file mmap symbol
scans, storage DXB lock-routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.120` batch, `1.195` crud, `0.743` iterate, `0.942`
get, and `1.071` delete.

A later POSIX DXB close-teardown cleanup added `dxb_storage_close_handles()` so
the special `lck_destroy()` close sequence is storage-owned too. Normal
`dxb_storage_close()` now reuses the same helper, while `lck_destroy()` asks it
for close completion state before restoring the in-process neighbor's fcntl
lock. This preserves the required order, close dsync first, close DXB second,
restore the neighbor lock after the current DXB handle is closed, then reset
storage, without pulling DXB/dsync descriptors apart in the lock teardown code.
The now-unused `dxb_storage_dsync_fd()` accessor was removed, and
`mdbx_env_get_fd()` remained the only environment-level DXB descriptor bridge at
that checkpoint. Verification passed `git diff --check`, stale data-file mmap
symbol scans, DXB close/lock routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.092`
batch, `1.173` crud, `0.823` iterate, `1.001` get, and `1.071` delete.

A later dirty-write queue submission cleanup moved the queued write target
selection behind `dxb_storage_t`. `iov_ctx` no longer carries a raw file
descriptor; it stores the logical `dxb_io_channel`, validates readiness through
`dxb_storage_iov_channel_is_ready()`, submits batches through
`dxb_storage_write_queued()`, and uses
`dxb_storage_iov_channel_is_primary_data()` for unsynced-page accounting. This
keeps the existing `osal_ioring_*` batching and dirty-page completion behavior,
but makes the storage facade responsible for choosing the data, dsync, or
overlapped write handle. Verification passed `git diff --check`, stale
data-file mmap symbol scans, dirty-write fd routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.136`
batch, `1.186` crud, `1.258` iterate, `0.943` get, and `1.094` delete.

A later dirty-write queue ownership cleanup moved queue prepare, empty, add,
walk, and reset operations behind `dxb_storage_t`. `iov_ctx` no longer stores an
`osal_ioring_t` pointer; it keeps only the environment, logical
`dxb_io_channel`, error state, and optional dirty-write range. At that
checkpoint, the `iov_*` implementation used
`dxb_storage_prepare_write_queue()`, `dxb_storage_write_queue_is_empty()`,
`dxb_storage_add_queued_write()`, `dxb_storage_walk_write_queue()`, and
`dxb_storage_reset_write_queue()`. Direct `osal_ioring_*` calls for dirty-page
writes are confined to storage wrappers and the OSAL implementation, leaving
future async backends one storage-owned queue surface to replace. Verification
passed `git diff --check`, stale data-file
mmap symbol scans, dirty-write queue ownership scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.141`
batch, `1.183` crud, `0.961` iterate, `0.964` get, and `1.079` delete.

A later dirty-write channel policy cleanup added
`dxb_storage_dirty_write_channel()`. `txn_basal_commit()` now passes policy
inputs, the lazy-meta flush need, dirty-entry count, write-through threshold,
and shared unsynced-page count, instead of inspecting storage descriptor
availability directly. Windows behavior remains unchanged and always selects
the primary data channel. POSIX behavior remains unchanged and uses the dsync
channel only when no forced nometasync flush is pending, the dsync descriptor is
available, the dirty-entry count is within the write-through threshold, and no
previous unsynced pages are pending. Verification passed `git diff --check`,
stale data-file mmap symbol scans, dirty-write channel policy scans, `make -f
GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public CTest suite including migration tool
roundtrip copy coverage, deterministic forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and focused ASAN `migration_smoke` CTest.
The paired `mdbx_migration_bench_lazy` gate reported forced/default ratios of
`1.114` batch, `1.184` crud, `1.038` iterate, `0.984` get, and `1.079` delete.

A later storage open-state cleanup added `dxb_storage_is_opened()` and removed
the internal `env_dxb_is_opened()` helper. Environment active checks,
pre-open guards, and DXB lock/setup assertions now ask `dxb_storage_t` whether
the data file is open, while the remaining public descriptor getter stays out of
normal internal control flow. This keeps open-state ownership with the storage
facade and leaves environment-level descriptor access out of
normal control flow. Verification passed `git diff --check`, stale data-file
mmap symbol scans, storage open-state scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.139`
batch, `1.181` crud, `0.988` iterate, `0.982` get, and `1.079` delete.

A later metadata sync predicate cleanup replaced the descriptor-named
`dxb_storage_meta_on_data_fd()` with storage-owned meta-write durability
predicates. `dxb_storage_meta_write_uses_data_sync()` keeps the raw
`meta_fd == data_fd` relationship inside the storage block, while
`dxb_storage_meta_write_needs_sync()` folds in the in-core database exemption
used by `dxb_sync_locked()`. The commit path now asks whether the meta write
needs a follow-up sync instead of testing descriptor identity directly.
Verification passed `git diff --check`, stale data-file mmap symbol scans,
metadata-sync storage predicate scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.116`
batch, `1.179` crud, `1.061` iterate, `1.105` get, and `1.096` delete.

A later storage range-sync cleanup added `dxb_storage_sync_range()` behind
`dxb_sync_data_range()`. Data sync callers already pass a page-range intent;
that range now reaches the storage facade instead of being discarded in the
environment wrapper. The current backend still performs the same whole-file
sync and ignores the range inside storage, while `dxb_note_fsync_pgop()` keeps
the existing environment pgop accounting out of the storage backend. This gives
future async/range-capable implementations a storage-owned hook without
changing durability behavior. Verification passed `git diff --check`, stale
data-file mmap symbol scans, storage range-sync scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.139`
batch, `1.159` crud, `0.968` iterate, `1.024` get, and `1.082` delete.

A later storage filesize-refresh fault-boundary cleanup moved the cached-size
refresh into `dxb_storage_fetch_filesize()`. The environment wrapper now only
delegates, while storage owns the raw size probe, `filesize` pre-call fault
injection, `filesize-complete` post-call fault injection, and acceptance of the
new `current`/`limit`/`filesize` view. The migration smoke fault matrix now
exercises open-time `filesize:EIO`, `filesize:EINTR`,
`filesize-complete:EIO`, and `filesize-complete:CANCEL`, covering immediate and
async-style size-probe completion failures before metadata validation accepts
the storage view. Verification passed `git diff --check`, stale data-file mmap
symbol scans, storage filesize-refresh fault scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, the 17-test
fault-enabled public CTest suite, `cmake --build @cmake-asan-build`, and
focused ASAN `migration_smoke` CTest. The paired `mdbx_migration_bench_lazy`
gate reported forced/default ratios of `1.118` batch, `1.171` crud, `0.998`
iterate, `0.934` get, and `1.047` delete.

A later storage page-read helper cleanup added `dxb_storage_read_pages()` with
an explicit page-size shift. At that point, `meta_shadow_refresh()`,
page-cache single-page misses, and page-cache overflow-span materialization
entered storage through that page-addressed helper instead of the
environment-shaped `dxb_read_pages()` wrapper. The public/internal wrapper
remains for callers that still need environment-owned page conversion, and
later descriptor cleanups moved committed-page cache fills and metadata shadow
refreshes onto checked `dxb_page_io_t` request handoff. Verification passed
`git diff --check`, stale data-file mmap symbol scans, storage page-read
routing scans,
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite including
migration tool roundtrip copy coverage, deterministic forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.144` batch, `1.164` crud, `0.986` iterate, `1.027`
get, and `1.071` delete.

A later storage page-write helper cleanup added `dxb_storage_write_pages()` and
`dxb_storage_writev_pages()` with page-size shifts, page-number offsets, and
storage-owned data-cache invalidation. The env-shaped `dxb_write_pages()` and
`dxb_writev_pages()` wrappers now delegate directly to those helpers, while the
unused env-level byte-vector `dxb_writev()` wrapper was removed. Byte-addressed
single-buffer writes still use `dxb_write()` for callers that need exact byte
offsets, but page-addressed writes now present the same storage-owned surface as
page reads. Verification passed `git diff --check`, stale data-file mmap symbol
scans, storage page-read/write routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip copy
coverage, deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.108`
batch, `1.153` crud, `1.112` iterate, `1.074` get, and `1.068` delete.

A later storage page-copy helper cleanup added `dxb_storage_copy_pages()` under
`MDBX_USE_COPYFILERANGE`. Same-file page copies now have a storage-owned helper
that performs page-to-offset conversion, delegates to `dxb_storage_copy_bytes()`,
normalizes copy faults through the existing storage copy path, and invalidates
the destination data-cache pages through `dxb_storage_invalidate_written_pages()`.
The env-shaped `dxb_copy_pages()` wrapper now only supplies the environment page
size shift. `defrag_move()` also passes page numbers directly to `dxb_copy_pages()`
instead of round-tripping through `pgno2bytes()` and `bytes2pgno()`. Verification
passed `git diff --check`, stale data-file mmap symbol scans, storage page-copy
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including `mdbx_defrag` overflow tool roundtrip coverage, deterministic forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.132` batch, `1.167` crud, `0.976` iterate, `0.990`
get, and `1.081` delete.

A later storage page-prefetch helper cleanup added `dxb_storage_prefetch_pages()`
beside the byte-range advisory helper. `dxb_prefetch()` now supplies the
environment page-size shift and delegates to storage for page-to-byte conversion
and `dxb_advice_willneed` submission. At that checkpoint,
`dxb_advise_range()` remained the byte-range wrapper for callers that already
operated in bytes and still owned the zero-length fast path. Verification passed
`git diff --check`, stale data-file mmap symbol scans, storage prefetch routing
scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.105`
batch, `1.167` crud, `0.880` iterate, `0.798` get, and `1.077` delete.

A later dirty-write queued-pages helper cleanup added
`dxb_storage_add_queued_pages()` beside the byte-addressed queue helper.
`iov_page()` now submits dirty pages by page number and page count, leaving
page-to-offset and page-count-to-byte conversion inside the storage-owned queue
surface. At that checkpoint the byte-addressed
`dxb_storage_add_queued_write()` remained available for lower-level queue
internals, but dirty-page commit submission already matched
the storage-owned page read, write, copy, and prefetch helpers. Verification
passed `git diff --check`, stale data-file mmap symbol scans, dirty-write
queued-page routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct
default and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`,
the six focused `migration_smoke` CTest entries, the full 15-test public CTest
suite including migration tool roundtrip coverage, deterministic forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.142` batch, `1.172` crud, `1.055` iterate, `1.078`
get, and `1.076` delete.

A later dirty-write queue preparation cleanup added
`dxb_storage_prepare_write_queue_pages()`. `iov_init()` now passes dirty-page
item count and page count to storage, and the storage helper performs the same
page-count-to-byte conversion plus system-page rounding previously done with
`pgno_ceil2sp_bytes()` in the transaction layer. Together with
`dxb_storage_add_queued_pages()`, dirty-page queue capacity and submission are
now page-addressed at the storage boundary. Verification passed `git diff --check`,
stale data-file mmap symbol scans, dirty-write queue preparation routing scans,
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite including
migration tool roundtrip coverage, deterministic forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.103` batch, `1.166` crud, `1.034` iterate, `0.977`
get, and `1.090` delete.

A later discard-path cleanup added `dxb_storage_discard_range()` and
`dxb_storage_discard_remove_range()`. The env-shaped `dxb_discard_range()` now
only adapts environment page geometry, while storage owns the clean/remove mode
dispatch and cache invalidation that follows a successful remove-style discard.
That leaves file-space discard policy beside the storage-owned advisory and
cache-invalidation helpers future async backends must replace. Verification
passed `git diff --check`, stale data-file mmap symbol scans, storage discard
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip coverage, deterministic forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.140` batch, `1.165` crud, `0.835` iterate, `1.075`
get, and `1.070` delete.

A later filesize-set bookkeeping cleanup added `dxb_storage_set_filesize_bytes()`.
The env-shaped `dxb_set_filesize()` now only adapts environment page geometry,
while storage owns the cached old-size read, `setsize` fault-injected
file-size change, truncated-cache invalidation, and cached filesize update.
This puts explicit file-size mutation bookkeeping beside storage-owned
file-size fetch and resize fault boundaries. Verification passed `git diff
--check`, stale data-file mmap symbol scans, filesize-set routing scans, `make
-f GNUmakefile mdbx_migration_smoke`, direct default and forced tiny-cache smoke
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public CTest suite including migration tool
roundtrip coverage, deterministic forced tiny-cache fault injection, `cmake
--build @cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.100`
batch, `1.172` crud, `0.726` iterate, `1.064` get, and `1.077` delete.

A later byte-write cache invalidation cleanup added `dxb_storage_write_bytes()`.
The env-shaped `dxb_write()` now only adapts environment page geometry, while
storage owns the byte-addressed write operation and the data-cache invalidation
that follows successful data-channel writes. Page-addressed writes still use the
page helper invalidation path, but single-buffer byte writes now match the
storage-owned cache-coherency boundary used by page writes, copies, discards,
and file truncation. Verification passed `git diff --check`, stale data-file
mmap symbol scans, byte-write routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.144`
batch, `1.184` crud, `0.997` iterate, `0.972` get, and `1.068` delete.

A later advisory-range cleanup moved the zero-length fast path into
`dxb_storage_advise_range()`. The env-shaped `dxb_advise_range()` now only
delegates byte ranges to storage, so advisory behavior, no-op range handling,
and platform-specific `fcntl()`/`posix_fadvise()` fallback policy sit behind
one storage-owned surface. Verification passed `git diff --check`, stale
data-file mmap symbol scans, advisory routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.105`
batch, `1.164` crud, `0.815` iterate, `1.070` get, and `1.088` delete.

A later resize bookkeeping cleanup added `dxb_storage_resize_bytes()`. The
existing env-shaped `dxb_storage_resize()` now only adapts environment page
geometry, while storage owns filesize refresh, read-only resize checks,
cache-aware file-size mutation, and current/limit/filesize acceptance for
byte-sized resize requests. This keeps the outer `dxb_resize()` responsible for
locks, page-number geometry, tail discard, and readahead policy, while the
storage facade owns the byte-level file-size state transition future async
backends must emulate. Verification passed `git diff --check`, stale data-file
mmap symbol scans, resize routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.125`
batch, `1.161` crud, `0.992` iterate, `1.026` get, and `1.079` delete.

A later setup bookkeeping cleanup added `dxb_storage_setup_bytes()`. The
env-shaped `dxb_setup_storage()` now only checks the byte geometry invariant and
adapts environment flags/page geometry, while storage owns setup-time
file-size creation, file-size refresh, current/limit/filesize acceptance, and
cache invalidation for any setup truncation. This makes initial data-file setup
match the storage-owned resize state transition instead of preserving a separate
env-owned byte path. Verification passed `git diff --check`, stale data-file
mmap symbol scans, setup routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.100`
batch, `1.168` crud, `1.204` iterate, `0.950` get, and `1.073` delete.

A later creation-size cleanup added `dxb_storage_set_filesize_as_current()` and
removed the now-unused env-shaped `dxb_set_filesize()` wrapper. New database
creation now asks storage to establish the initial file length and accept that
length as the current storage size in one storage-owned transition, instead of
setting file length through an env adapter and mutating storage current size
from the setup path. Verification passed `git diff --check`, stale data-file
mmap symbol scans, creation-size routing scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public CTest suite including migration tool roundtrip coverage,
deterministic forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, and focused ASAN `migration_smoke` CTest. The paired
`mdbx_migration_bench_lazy` gate reported forced/default ratios of `1.082`
batch, `1.182` crud, `1.226` iterate, `0.930` get, and `1.076` delete.

A later filesize-refresh cleanup removed the remaining env-shaped
`dxb_fetch_filesize()` wrapper. Header reads, meta validation, and shrink-side
transaction checks now call `dxb_storage_fetch_filesize()` directly, while the
coherency head path uses `dxb_storage_fetch_filesize_if_current_lacks()` for the
common "refresh only if current storage size does not cover this byte range"
case. This leaves real file-size refresh and current/limit/filesize bookkeeping
behind storage-owned helpers instead of an environment adapter. Verification
passed `git diff --check`, stale data-file mmap symbol scans, filesize-refresh
routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default and
forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip coverage, deterministic forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.123` batch, `1.177` crud, `0.933` iterate, `1.120`
get, and `1.099` delete.

A later read-routing cleanup removed the env-shaped `dxb_read()` and
`dxb_read_pages()` wrappers. At that point, warmup reads, environment copy
fallback reads, root-txnid probes, defrag page reads, and meta-page probing
called `dxb_storage_read()` or `dxb_storage_read_pages()` directly with the
environment storage handle and page geometry. Later descriptor cleanups removed
the page-read helper layer for committed-page, metadata-shadow, and defrag page
reads, leaving explicit data-file reads behind storage helpers instead of
preserving extra env adapters for byte/page reads.
Verification passed `git diff --check`, stale data-file mmap symbol scans,
read-routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip coverage, deterministic forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.121` batch, `1.188` crud, `1.050` iterate, `1.006`
get, and `1.087` delete.

A later write-routing cleanup removed the env-shaped `dxb_write()`,
`dxb_write_pages()`, `dxb_writev_pages()`, and `dxb_copy_pages()` adapters.
Defrag page moves/copies, new-database meta triplet writes, commit meta writes
and undo rewrites, steady-meta wiping, meta override writes, and killed-page
poison writes now call `dxb_storage_write_bytes()`,
`dxb_storage_write_pages()`, `dxb_storage_writev_pages()`, or
`dxb_storage_copy_pages()` directly with explicit storage and page geometry.
This leaves direct explicit writes/copies behind storage-owned helpers instead
of keeping extra environment adapters for byte/page write calls. Verification
passed `git diff --check`, stale data-file mmap symbol scans,
write-routing scans, `make -f GNUmakefile mdbx_migration_smoke`, direct default
and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public CTest suite
including migration tool roundtrip coverage, deterministic forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.122` batch, `1.176` crud, `1.017` iterate, `1.012`
get, and `1.085` delete.

A later advisory-routing cleanup removed the env-shaped `dxb_advise_range()`,
`dxb_prefetch()`, and `dxb_discard_range()` adapters. Resize shrink discard,
readahead advice/prefetch, open-time tail discard, and commit-time shrink
discard now call `dxb_storage_advise_range()`,
`dxb_storage_prefetch_pages()`, or `dxb_storage_discard_range()` directly with
explicit storage and page geometry. This keeps advisory and discard behavior
behind the same storage facade as byte/page read, write, copy, sync, and size
operations. Verification passed `git diff --check`, stale wrapper scans, stale
data-file mmap symbol scans, `make -f GNUmakefile mdbx_migration_smoke`, direct
default and forced tiny-cache smoke runs, `cmake --build @cmake-ninja-build`,
the six focused `migration_smoke` CTest entries, the full 15-test public CTest
suite including migration tool roundtrip coverage, deterministic forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.116` batch, `1.165` crud, `0.975` iterate, `1.015`
get, and `1.076` delete.

A later setup/resize adapter cleanup removed the env-shaped
`dxb_setup_storage()` and `dxb_storage_resize()` shims. `dxb_setup()` now checks
the setup byte-geometry invariant and calls `dxb_storage_setup_bytes()`
directly with explicit storage, flags, options, and page geometry, while
`dxb_resize()` calls `dxb_storage_resize_bytes()` directly after computing the
locked byte-size transition and resize flags. This leaves setup and resize
state transitions behind storage-owned byte helpers instead of preserving
one-call environment adapters. Verification passed `git diff --check`, stale
adapter scans, stale data-file mmap symbol scans, `make -f GNUmakefile
mdbx_migration_smoke`, direct default and forced tiny-cache smoke runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public CTest suite including migration tool
roundtrip coverage, deterministic forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and focused ASAN `migration_smoke` CTest.
The paired `mdbx_migration_bench_lazy` gate reported forced/default ratios of
`1.089` batch, `1.156` crud, `0.982` iterate, `0.998` get, and `1.073`
delete.

A later sync-adapter cleanup removed the env-shaped `dxb_fsync()` and
`dxb_sync_data()` wrappers. Metadata sync paths now keep pgop accounting at the
environment layer and call `dxb_storage_sync()` directly, while the pre-writer
data sync path calls `dxb_sync_data_range()` with an explicit
`dxb_sync_range_all()` range instead of passing only a page count through a
one-call adapter. This leaves range-bearing data sync and storage-owned meta
sync submission as the remaining explicit sync surfaces. Verification passed
`git diff --check`, stale sync-wrapper scans, stale data-file mmap symbol scans,
`make -f GNUmakefile mdbx_migration_smoke`, direct default and forced
tiny-cache smoke runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public CTest suite including
migration tool roundtrip coverage, deterministic forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, and focused ASAN
`migration_smoke` CTest. The paired `mdbx_migration_bench_lazy` gate reported
forced/default ratios of `1.123` batch, `1.178` crud, `1.045` iterate, `1.021`
get, and `1.094` delete.

A later data-sync range wrapper cleanup removed the env-shaped
`dxb_sync_data_range()` adapter. `dxb_sync_locked()` and the pre-writer
`env_sync()` path now build explicit `dxb_sync_range_t` values at their policy
sites, keep the no-WRITEMAP invariants and pgop accounting there, and submit
the range directly through `dxb_storage_sync_range()`. This leaves data sync
range submission behind storage without preserving a separate environment
forwarder. Verification passed stale sync-wrapper
scans, stale data-file mmap symbol scans, `mdbx_migration_smoke` default and
forced tiny-cache runs, Ninja build plus focused CTest, public migration CTest,
fault injection, ASAN build plus focused ASAN CTest, and the paired lazy
benchmark gate. The lazy gate passed with forced/default ratios of `1.255`
batch, `1.338` crud, `0.834` iterate, `1.205` get, and `1.152` delete.

A later C++ layout mirror cleanup synchronized the shipped `mdbx.c++` private
cursor and environment declarations with the explicit-I/O core layout. The C++
translation unit no longer carries the stale data-file `dxb_mmap` shell,
`lazy_fd`, `fd4meta`, `mlocked_pgno`, or env-owned dirty-write queue field; it
now declares the page-cache/page-ref types, `dxb_storage_t`, cursor page refs,
and meta-shadow fields needed to keep wrapper dereferences such as
`handle_->userctx` and `handle_->txn` aligned with `mdbx.c`. Verification passed
`git diff --check`, stale data-file mmap and stale sync-wrapper scans across
the shipped core sources, GNUmake C++ object/shared-library/example builds,
`LD_LIBRARY_PATH=. ./mdbx_modern_example`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, and the full 15-test public migration CTest
suite including both C++ API variants. The paired `mdbx_migration_bench_lazy`
gate passed with forced/default ratios of `1.088` batch, `1.169` crud, `1.140`
iterate, `0.958` get, and `1.061` delete.

A later metadata-sync adapter cleanup removed the env-shaped metadata sync
forwarder. `dxb_sync_locked()`, `meta_wipe_steady()`, and `meta_override()`
now keep the `meta_fd == data_fd` follow-up sync predicate, pgop accounting,
and storage submission at their policy sites before calling
`dxb_storage_sync()` directly. This leaves metadata durability decisions
visible at the write sites while keeping the raw sync operation behind the
storage facade. Verification passed stale
metadata-sync adapter scans, stale data-file mmap and sync-wrapper scans across
the shipped core sources, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, and the six focused ASAN
`migration_smoke` CTest entries. The paired `mdbx_migration_bench_lazy` gate
passed with forced/default ratios of `1.113` batch, `1.179` crud, `1.265`
iterate, `0.987` get, and `1.071` delete.

A later public DXB descriptor bridge cleanup removed the last environment DXB
descriptor helper. `mdbx_env_get_fd()` now returns the data descriptor directly
from `dxb_storage_t` through `dxb_storage_data_fd()`, so even the public
descriptor getter no longer preserves a separate environment-shaped forwarding
layer over storage. Verification passed stale descriptor-bridge scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The first paired
`mdbx_migration_bench_lazy` attempt had a noisy batch miss at `0.581` versus the
`0.600` gate; the repeat passed with forced/default ratios of `1.381` batch,
`1.192` crud, `0.969` iterate, `0.947` get, and `1.048` delete.

A later descriptor-open target cleanup made descriptor creation mutate explicit
`dxb_storage_t` targets. `dxb_storage_open_data()`,
`dxb_storage_open_dsync()`, and Windows-only
`dxb_storage_open_overlapped()` now receive the storage object, environment
open-policy context, and DXB pathname separately, so descriptor mutation is
visibly storage-owned while OSAL still receives the environment/path context it
needs for platform-specific open rules. Verification passed explicit open-helper
target scans, stale data-file mmap and removed sync-adapter scans across the
shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.102`
batch, `1.144` crud, `0.991` iterate, `1.011` get, and `1.087` delete.

A later page-cache invalidation mutability cleanup made cache-mutating storage
operations take mutable `dxb_storage_t` targets. Data-page writes, writev
submission, same-file page copies, destructive discard, and the page/byte cache
invalidation helpers no longer accept `const dxb_storage_t` before mutating
storage-owned cache state. Read-only file operations still keep const storage
parameters, so the helper signatures now distinguish raw observation from cache
state mutation. Verification passed mutating-storage const-signature scans,
stale data-file mmap and removed sync-adapter scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.148` batch, `1.179` crud, `1.908` iterate, `1.090`
get, and `1.200` delete.

A later page-cache lookup boundary cleanup split transaction policy from
storage-owned cache lookup. `dxb_storage_lookup_cached_page()` now receives the
storage object, page number, snapshot id, and reuse eligibility explicitly,
while `page_cache_read()` remains responsible for deriving those values from
the owning transaction. Overflow-span materialization now reads through the
cache entry's owning storage object instead of rediscovering storage from the
environment. Verification passed stale lookup-helper scans, stale data-file mmap
and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.089` batch, `1.155` crud, `0.995` iterate, `1.000`
get, and `1.084` delete.

A later large-page cache materialization cleanup split transaction validation
from the storage-owned overflow-span read and cache-entry replacement.
`page_cache_read_large()` still checks the transaction snapshot bounds, while
`dxb_storage_materialize_cached_large_page()` now owns the explicit storage read,
buffer replacement, and cache accounting for expanding a cached overflow header
to its full page span. Verification passed stale large-page env-storage scans,
stale data-file mmap and removed sync-adapter scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.076` batch, `1.116` crud, `0.967` iterate, `1.045`
get, and `1.077` delete.

A later page-cache ref pin accounting cleanup made retained cache refs pass the
storage target explicitly. Cursor/page-result retain and release paths now call
`dxb_storage_retain_cached_entry()` and `dxb_storage_release_cached_ref()` with
the cache entry's owning `dxb_storage_t`, so pin-count mutations and the
storage-owned pinned counter no longer hide the storage target behind the cache
entry alone. Verification passed stale page-cache ref-helper scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.121` batch, `1.134` crud, `1.018` iterate, `0.965`
get, and `1.058` delete.

A later single-page cache-miss cleanup moved cache entry allocation, explicit
page read, and cache-list registration into `dxb_storage_read_cached_page()`.
`page_cache_read()` still derives the transaction policy inputs (`snapshot`,
`reusable`, and private tracking) and passes page geometry explicitly, but the
miss path now has one storage-owned helper that future async backends can
replace or queue behind. Verification passed stale read-cache helper scans,
stale data-file mmap and removed sync-adapter scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.127` batch, `1.134` crud, `1.223` iterate, `1.016`
get, and `1.064` delete.

A later page-cache observation constness cleanup made cache locking logical-const.
`page_cache_lock()` and `page_cache_unlock()` now take `const dxb_storage_t *`
and centralize the mutex mutability cast, so read-only storage observation
helpers no longer cast their storage target before scanning cached page ranges.
This keeps public const transaction paths on const storage-shaped helpers while
leaving the mutable cache accounting paths explicit. Verification passed
logical-const cache-lock scans, stale data-file mmap and removed sync-adapter
scans across the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, and the six focused ASAN
`migration_smoke` CTest entries. The paired `mdbx_migration_bench_lazy` gate
passed with forced/default ratios of `1.139` batch, `1.143` crud, `0.997`
iterate, `0.901` get, and `1.144` delete.

A later cache-entry geometry cleanup made explicit page-cache entries remember
their page-size shift. Cached-pointer classification and cached overflow-span
materialization now derive page geometry from the cache entry instead of taking
`env->ps`/`env->ps2ln` from higher-level transaction code. This keeps page-cache
range scans and overflow reads closer to storage-owned state while leaving
transaction code responsible only for snapshot bounds and reuse policy.
Verification passed stale cache-geometry helper scans, stale data-file mmap and
removed sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.079`
batch, `1.155` crud, `0.903` iterate, `1.078` get, and `1.066` delete.

A later storage geometry signature cleanup removed redundant page-size byte
arguments from cache invalidation, byte-write, discard, filesize, setup, resize,
and cache-miss read helpers. These storage-owned helpers now receive the
page-size shift and derive byte-sized page geometry internally, so callers no
longer thread both `env->ps` and `env->ps2ln` through the same explicit-I/O
boundary. Verification passed stale storage-geometry signature scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.108` batch, `1.152` crud, `0.935` iterate, `0.977`
get, and `1.074` delete.

A later storage-owned page-size cleanup added the data-file page-size shift to
`dxb_storage_t` and configured it when the explicit-I/O storage backend is set
up, with the new-database bootstrap path setting it before its initial meta-page
write. Cache invalidation, byte writes, filesize changes, discard operations,
resize operations, and cached single-page reads now derive their page geometry
from the storage object instead of accepting `env->ps2ln` at each call site.
Verification passed stale storage-owned geometry scans, stale data-file mmap and
removed sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.105`
batch, `1.167` crud, `0.995` iterate, `1.034` get, and `1.071` delete.

A later page-numbered storage API cleanup removed the page-size shift parameter
from explicit page read/write/writev/prefetch/copy and write-queue helpers.
Those helpers now derive page offsets, byte counts, and queue reservation sizes
from `dxb_storage_t`, leaving callers to pass only page numbers, page counts,
and I/O channel intent. This moves the storage abstraction closer to the
async-capable backend boundary where geometry is storage state instead of
transaction call-site state. Verification passed stale page-I/O signature scans,
stale data-file mmap and removed sync-adapter scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.081` batch, `1.153` crud, `1.048` iterate, `0.943`
get, and `1.129` delete.

A later storage conversion helper cleanup centralized storage-owned page/byte
math behind `dxb_storage_pgno2bytes()`, `dxb_storage_npages2bytes()`, and
`dxb_storage_bytes2pgno()`. Page-cache invalidation, page I/O offset/length
calculation, write-queue reservations, copy-file-range offsets, writev
invalidation accounting, and `mdbx_chk` file/backed page accounting now use the
storage-owned conversion helpers instead of open-coded shifts. Verification
passed stale storage conversion scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.116`
batch, `1.154` crud, `0.793` iterate, `1.032` get, and `1.073` delete.

A later storage setup geometry cleanup configures the storage page-size shift as
soon as `dxb_setup()` accepts the meta page size, before the explicit-I/O
storage setup helper is called. `dxb_storage_setup_bytes()` now requires storage
geometry to be preconfigured, and the remaining open-time filesize diagnostic
uses `dxb_storage_bytes2pgno()` instead of an environment-local page-size shift.
Verification passed storage-geometry ordering scans, stale data-file mmap and
removed sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.083`
batch, `1.164` crud, `1.010` iterate, `1.014` get, and `1.071` delete.

A later storage-aligned geometry cleanup added storage-owned OS/allocation
alignment helpers and moved warmup, resize, readahead, and open/setup geometry
range math onto `dxb_storage_t`. The data-file resize and prefetch paths now
derive aligned byte ranges and page-number diagnostics from storage geometry,
while the public/env geometry helpers remain available for API-facing page
format calculations. Verification passed stale storage-alignment scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.124` batch, `1.158` crud, `1.279` iterate, `1.008`
get, and `1.075` delete.

A later queued-write geometry cleanup moved dirty write completion accounting
and `page_kill()` auxiliary writev chunking onto storage-owned page conversion
helpers. Queued dirty-page release now derives page numbers and page spans from
`dxb_storage_t`, and killed-page writev batches track page numbers directly
instead of round-tripping through env-local byte offsets. Verification passed
stale queued-write conversion scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.122`
batch, `1.142` crud, `1.245` iterate, `0.951` get, and `1.078` delete.

A later sync and transaction setup geometry cleanup moved shrink discard
alignment, meta sync/write calls, and transaction current-size checks onto
storage-owned geometry. `dxb_sync_locked()` now derives discard ranges and
shrink alignment from `dxb_storage_t`, while basal and read transaction setup
validate storage coverage with storage page conversions instead of env-local
byte math. Verification passed stale sync/txn storage conversion scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.129` batch, `1.161` crud, `1.110` iterate, `0.945`
get, and `1.085` delete.

A later meta-offset geometry cleanup moved commit metadata payload offsets and
steady-sign wipe offsets under `dxb_storage_t`. The old env-shaped
`meta_*_dxb_offset()` helpers are gone; storage now owns the meta slot
page/payload/field byte-offset calculations used by explicit meta writes, while
the env shadow-page helpers remain memory-layout helpers. Verification passed
stale meta-offset scans, stale data-file mmap and removed sync-adapter scans
across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.125`
batch, `1.185` crud, `1.004` iterate, `0.984` get, and `1.082` delete.

A later coherency storage-geometry cleanup moved root-page txnid probe offsets
and transaction-head required-size checks onto `dxb_storage_t`. The root probe
now derives the `page_t.txnid` byte offset through a storage-owned page-field
helper before issuing the explicit read, and `coherency_fetch_head()` refreshes
the current storage view using storage page conversion instead of env-local
byte math. Verification passed stale coherency conversion scans, stale data-file
mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.091` batch, `1.150` crud, `1.128` iterate, `0.981`
get, and `1.084` delete.

A later fast-cache geometry cleanup moved public cache-entry offset handling
onto `dxb_storage_t`. `cache_offset_from_ref()` now derives cached value
data-file offsets from storage page geometry, and `cache_materialize_entry()`
uses storage conversions to validate cached offsets, locate the page number,
compute the in-page offset, and bound the materialized cache hit. Verification
passed stale fast-cache conversion scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.114`
batch, `1.153` crud, `1.279` iterate, `1.223` get, and `1.084` delete.

A later copy/export geometry cleanup moved environment-copy meta spans and
output range sizing onto `dxb_storage_t`. `copy_asis()`,
`copy_with_compacting()`, and `copy2fd()` now derive meta stub/write sizes,
whole/used copy ranges, and compacted output extension ranges through storage
conversions before using explicit storage read/sendfile/copy calls.
Verification passed stale copy-range conversion scans, stale data-file mmap and
removed sync-adapter scans across the shipped core sources, `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.118`
batch, `1.156` crud, `0.866` iterate, `1.059` get, and `1.081` delete.

A later allocator-growth geometry cleanup moved write-transaction data-file
extension alignment onto `dxb_storage_t`. `gc_alloc_ex()` now computes the
candidate growth boundary with storage-owned OS/allocation geometry before
calling `dxb_resize()`, leaving API/reporting geometry conversions unchanged.
Verification passed stale allocator-growth conversion scans, stale data-file
mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.128` batch, `1.167` crud, `0.999` iterate, `1.017`
get, and `1.076` delete.

A later meta-shadow geometry cleanup moved env-owned meta shadow buffer sizing
and slot addressing onto `dxb_storage_t`. The shadow triplet now allocates and
addresses meta page slots through storage page geometry, refreshes via a local
storage handle, and asserts that storage/env page sizes match before copying
shadow pages. Verification passed stale meta-shadow conversion scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.101` batch, `1.156` crud, `1.015` iterate, `0.969`
get, and `1.070` delete.

A later defrag page-move cleanup routed defragmentation data-page movement
through a single storage handle. `defrag_move()` now binds the env and
`dxb_storage_t` once, then uses that handle for explicit source-page reads,
destination-page writes, and copy-range fallback when relocating overflow
pages. Verification passed stale defrag storage-call scans, stale data-file
mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.101` batch, `1.164` crud, `0.887` iterate, `1.101`
get, and `1.068` delete.

A later header-read cleanup routed open-time meta probing through a local
storage handle. `dxb_read_header()` now binds `dxb_storage_t` once and uses it
for filesize refresh, empty-file detection, and both meta-page read attempts in
the retry loop. Verification passed stale header-read storage-call scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.119` batch, `1.167` crud, `1.073` iterate, `1.013`
get, and `1.071` delete.

A later dirty-write queue cleanup bound transaction I/O contexts to the storage
object they submit through. `iov_ctx_t` now carries `dxb_storage_t`, and
`iov_init()`, `iov_empty()`, `iov_complete()`, `iov_write()`, `iov_page()`, and
the primary-data channel accounting in `txn_write()` use that handle for queue
prepare/reset/add/write/walk decisions. Verification passed stale write-context
storage-call scans, stale data-file mmap and removed sync-adapter scans across
the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.133`
batch, `1.174` crud, `0.909` iterate, `0.988` get, and `1.081` delete.

A later metadata storage-handle cleanup routed setup and metadata maintenance
through bound storage handles. `dxb_setup()` now binds `dxb_storage_t` for
initial meta-triplet writes and setup filesize checks, `meta_wipe_steady()`
passes that handle through `meta_unsteady()`, `meta_sync()` and
`meta_override()` use local storage handles for metadata write/sync decisions,
`meta_validate()` uses a local storage handle for filesize refreshes, and
`txn_basal_commit()` uses one for lazy meta-sync and dirty-write channel
selection. Verification passed stale metadata storage-call scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.136` batch, `1.172` crud, `0.985` iterate, `0.983`
get, and `1.087` delete.

A later environment lifecycle cleanup routed data-file open and close plumbing
through bound storage handles. `env_open()` now binds `dxb_storage_t` once for
data, overlapped, and dsync handle open/park operations, mode/stat probing,
incore checks, and write-queue creation. `env_close()` now uses a bound storage
handle for write-queue destruction, overlapped-handle assertions, and data-file
close. Verification passed stale lifecycle storage-call scans, stale data-file
mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.169` batch, `1.214` crud, `0.857` iterate, `0.976`
get, and `1.079` delete.

A later warmup read cleanup routed forced warmup and fd lookup through bound
storage handles. `warmup_force_read()` now accepts `dxb_storage_t` directly for
current-size clamping and chunked data-file reads, `mdbx_env_warmup()` binds
storage once for range sizing and forced-read dispatch, and `mdbx_env_get_fd()`
uses a local storage handle before returning the data descriptor. Verification
passed stale warmup storage-call scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`, `make
-f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.116`
batch, `1.174` crud, `0.927` iterate, `1.012` get, and `1.071` delete.

A later checker/reporting cleanup routed environment-check geometry reporting
through a bound storage handle. `env_chk()` now binds `dxb_storage_t` once and
uses that handle for checker filesize refreshes, file/backed page conversions,
and mapsize reporting. Verification passed stale checker storage-call scans,
stale data-file mmap and removed sync-adapter scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.138` batch, `1.157` crud, `0.805` iterate, `1.042`
get, and `1.074` delete.

A later environment state/info cleanup routed public state, option, info, and
pre-sync checks through bound storage handles. `env_is_active()`,
`check_env()`, `mdbx_env_create()`, recovery/open guards,
`mdbx_env_close_ex()`, `env_info_sys()`, `env_info_snap()`,
`mdbx_env_set_option()`, and the `env_sync()` data preflush path now avoid
direct `env->dxb_storage` call arguments. Verification passed stale
state/info/sync storage-call scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`, `make
-f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.141`
batch, `1.170` crud, `1.014` iterate, `0.910` get, and `1.078` delete.

A later locking cleanup routed data-file lock operations through bound storage
handles while leaving the lock-file mmap untouched. POSIX locking helpers now
bind storage in `check_fstat()`, `lck_seize()`, `lck_downgrade()`,
`lck_upgrade()`, `lck_destroy()`, and SYSV `lck_init()` setup, while Windows
locking helpers bind storage in `flock_dxb()`, `lck_txn_unlock()`,
`lck_unlock()`, and the Windows `lck_seize()` unlock path. `lck_setup()` also
uses a local storage handle for its data-file-open assertion and read-only
filesystem check. Verification passed stale locking storage-call scans, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.084` batch, `1.181` crud, `0.968` iterate, `1.004`
get, and `1.084` delete.

A later pointer-origin cleanup routed explicit page-cache pointer validation
through bound storage handles. `page_ptr_is_explicit_io_buffer()` and
`mdbx_is_dirty()` now bind `dxb_storage_t` before checking whether a pointer
belongs to the explicit page cache, preserving the existing dirty-list fallback
and pointer validation behavior. Verification passed direct `env->dxb_storage`
call-argument scans, stale data-file mmap and removed sync-adapter scans across
the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.072`
batch, `1.164` crud, `1.026` iterate, `1.073` get, and `1.084` delete.

A later preopen snapshot cleanup routed the temporary stack environment through
a bound storage handle. `mdbx_preopen_snapinfo()` now binds the stack
`MDBX_env` storage once for reset, Windows overlapped-handle marking, and
read-only data-file open. Verification passed direct storage call-argument
scans, stale data-file mmap and removed sync-adapter scans across the shipped
core sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.100` batch, `1.190` crud, `1.072` iterate, `0.922`
get, and `1.100` delete.

A later large-page materialization cleanup hardened page-cache pointer
stability for the async-capable read path. When
`page_cache_read_large()` expands a tracked overflow-header entry and discovers
that the entry has more than one pin, it now leaves the shared header buffer
untouched and returns the expanded span through a private unlisted cache ref.
This preserves existing pinned `page_t *` values even if a future async read
window allows an expandable entry to gain another pin before materialization.
Verification passed stale data-file mmap and removed sync-adapter scans across
the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.163`
batch, `1.140` crud, `1.037` iterate, `1.025` get, and `1.071` delete.

A later page-I/O descriptor cleanup added `dxb_page_io_t` and
`dxb_storage_page_io()` as the common checked conversion for page-addressed
storage operations. Queued dirty writes, committed page reads, page writes,
prefetch hints, writev page offsets, and same-file page copies now derive their
storage byte ranges through that descriptor, including range and host-size
checks before crossing into raw byte I/O. This narrows the future async backend
surface to one page-request shape while keeping the public API synchronous.
Verification passed stale data-file mmap and removed sync-adapter scans across
the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.099`
batch, `1.169` crud, `1.001` iterate, `0.965` get, and `1.080` delete.

A later page-I/O helper cleanup made the descriptor the operation handoff
inside the storage facade. `dxb_page_io_t` now carries `end_pgno`, and
descriptor-taking helpers handle queued writes, prefetch, reads, writes, and
write invalidation before raw byte I/O is reached. Page writes and same-file
page copies now invalidate cache entries from the same descriptor used for the
I/O request, while writev page invalidation uses a descriptor when its iovec
span maps cleanly to pages and keeps the defensive clamp fallback for overflow.
Verification passed stale data-file mmap and removed sync-adapter scans across
the shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced tiny-cache
runs, `cmake --build @cmake-ninja-build`, the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the six
focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.139`
batch, `1.149` crud, `0.952` iterate, `0.989` get, and `1.080` delete.

A later page-cache descriptor cleanup stored `dxb_page_io_t` directly in
`page_cache_entry_t`. Single-page cache misses now obtain their descriptor from
`dxb_storage_page_io()` before allocation, large-page materialization copies the
same descriptor into either the expanded tracked entry or the detached private
entry, and cache accounting/invalidation now uses descriptor `npages`, `bytes`,
and `end_pgno` instead of maintaining parallel scalar fields. Verification
passed stale page-cache scalar-field scans, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `git diff --check`, `make
-f GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.116`
batch, `1.151` crud, `0.833` iterate, `1.052` get, and `1.082` delete.

A later cache-read descriptor handoff cleanup made the cache entry descriptor
the actual storage read request for cache misses and overflow-span
materialization. `dxb_storage_read_cached_page()` and
`dxb_storage_materialize_cached_large_page()` now pass their existing
`dxb_page_io_t` into `dxb_storage_read_io()`, and cached result construction is
centralized around the entry descriptor. This removes another duplicate
page-to-byte conversion before the future async read submission point.
Verification passed stale duplicate cache-read scans, stale data-file mmap
scans across the shipped core sources, `git diff --check`, `make -f
GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.103`
batch, `1.151` crud, `0.995` iterate, `0.993` get, and `1.076` delete.

A later cache-read request cleanup moved descriptor construction to the
`page_cache_read()` boundary. Lookup and miss-fill helpers now receive the same
checked single-page `dxb_page_io_t` request instead of raw `pgno`, so cache-hit
matching, miss allocation, and the eventual storage read use one page-I/O
shape. This keeps the public API synchronous while making the read-cache entry
point closer to an async request submission boundary. Verification passed stale
pgno-based cache-read scans, stale data-file mmap scans across the shipped core
sources, `git diff --check`, `make -f GNUmakefile mdbx_migration_smoke`,
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, and the six focused ASAN `migration_smoke`
CTest entries. The paired `mdbx_migration_bench_lazy` gate passed with
forced/default ratios of `1.099` batch, `1.161` crud, `0.971` iterate, `0.976`
get, and `1.087` delete.

A later committed-read descriptor cleanup moved the single-page descriptor
construction out of the cache helper and into its callers. `page_get_committed()`
now builds the checked `dxb_page_io_t` before entering `page_cache_read()`, and
fast key/value cache materialization does the same before resolving a cached
data-file offset. This makes the committed-page API hand a storage request into
the pinned cache layer instead of asking the cache layer to derive one from raw
`pgno`. Verification passed stale raw-pgno cache-read scans, stale data-file
mmap scans across the shipped core sources, `git diff --check`, `make -f
GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.094`
batch, `1.153` crud, `1.132` iterate, `1.030` get, and `1.064` delete.

A later unchecked-read descriptor cleanup moved committed-page request
construction out to the caller that has already checked transaction-owned dirty
and spilled pages. `page_get_unchecked_ex()` now builds the checked
`dxb_page_io_t` only after dirty-list and parent-spill lookup fall through, then
hands that request into `page_get_committed()`. This keeps explicit storage
requests limited to pages that actually need committed storage/cache lookup.
Verification passed stale raw-pgno committed/cache-read scans, stale data-file
mmap scans across the shipped core sources, `git diff --check`, `make -f
GNUmakefile mdbx_migration_smoke`, `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, and the
six focused ASAN `migration_smoke` CTest entries. The paired
`mdbx_migration_bench_lazy` gate passed with forced/default ratios of `1.108`
batch, `1.149` crud, `0.850` iterate, `0.813` get, and `1.082` delete.

A later meta-shadow refresh descriptor cleanup removed the helper-local
page-read conversion from metadata refresh. `meta_shadow_refresh()` now builds a
checked `dxb_page_io_t` for `(0, NUM_METAS)` and submits that descriptor through
`dxb_storage_read_io()` after ensuring the shadow buffer exists. Metadata
rereads now use the same descriptor handoff shape as committed-page cache
fills, leaving the next async-capable read submission point independent of raw
page-number conversion at the caller. Verification passed the stale metadata
read scan, stale data-file mmap and removed sync-adapter scans across the
shipped core sources, `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.111` batch, `1.161` crud, `0.980` iterate, `1.010`
get, and `1.070` delete.

A later defrag descriptor-read cleanup removed the remaining
`dxb_storage_read_pages()` helper from the C source. `defrag_move()` now builds
one-page `dxb_page_io_t` requests for its fallback source-page reads and submits
those descriptors through `dxb_storage_read_io()` before writing the moved
destination pages. This leaves direct page-addressed storage reads on the same
checked descriptor handoff used by metadata refresh and committed-page cache
fills, without preserving a helper-local page-to-byte conversion layer.
Verification passed `git diff --check`, source scans proving
`dxb_storage_read_pages()` is gone from the shipped core sources, stale
data-file mmap and removed sync-adapter scans across the shipped core sources,
`make -f GNUmakefile mdbx_migration_smoke`, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite including no-mmap tool roundtrip/defrag coverage, forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.112` batch, `1.154`
crud, `0.824` iterate, `1.041` get, and `1.080` delete.

A later byte-read descriptor cleanup added `dxb_byte_io_t` and
`dxb_storage_byte_io()` for checked byte-addressed read requests. Warmup forced
range reads, portable environment-copy fallback chunks, coherency root-txnid
probes, and startup meta-header double-reads now build a byte descriptor before
calling `dxb_storage_read_bytes()`. Page-addressed reads continue to use
`dxb_page_io_t` and `dxb_storage_read_io()`, which now adapts its checked page
request through the same byte-read submission helper. The old raw
`dxb_storage_read()` helper is gone from the C source, leaving storage reads
described by explicit byte or page request objects before raw pread is reached.
Verification passed `git diff --check`, source scans proving raw
`dxb_storage_read()` calls are gone, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `make -f GNUmakefile
mdbx_migration_smoke`, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.108` batch, `1.193` crud, `1.032` iterate, `1.025`
get, and `1.107` delete.

A later byte-write descriptor cleanup reused `dxb_byte_io_t` for
byte-addressed write requests. Commit metadata payload writes, metadata undo
rewrites, and steady-meta sign wiping now build a checked byte descriptor before
calling `dxb_storage_write_bytes()`, while page-addressed writes adapt their
checked `dxb_page_io_t` through the same byte-write submission helper. The old
raw `dxb_storage_write()` helper is gone from the C source, leaving storage
writes described by explicit byte or page request objects before raw pwrite is
reached. Verification passed `git diff --check`, source scans proving raw
`dxb_storage_write()` calls are gone, stale data-file mmap and removed
sync-adapter scans across the shipped core sources, `make -f GNUmakefile
mdbx_migration_smoke`, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.113` batch, `1.170` crud, `1.164` iterate, `0.927`
get, and `1.080` delete.

A later vector-write descriptor cleanup added `dxb_storage_writev_bytes()` for
scatter/gather writes with an explicit `dxb_byte_io_t` request. The page-vector
wrapper now computes the iovec byte span, builds a checked byte descriptor from
the page-derived start offset, and submits that descriptor before invalidating
the written page-cache range. The old raw `dxb_storage_writev()` helper is gone
from the C source, leaving vector writes described by the same byte request
shape as single-buffer byte writes before raw pwritev is reached. Verification
passed `git diff --check`, source scans proving raw `dxb_storage_writev()`
calls are gone, stale data-file mmap and removed sync-adapter scans across the
shipped core sources, `make -f GNUmakefile mdbx_migration_smoke`, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.117` batch, `1.155` crud, `0.743`
iterate, `0.814` get, and `1.071` delete.

A later dirty-write queue descriptor cleanup moved `dxb_byte_io_t` into the
internal header and changed `osal_ioring_add()` plus `osal_ioring_walk()` to
exchange byte request descriptors. `dxb_storage_add_queued_io()` now adapts its
checked `dxb_page_io_t` into a checked `dxb_byte_io_t` before queue insertion,
and dirty-page completion receives that descriptor for page-number checks and
shadow-buffer release accounting. The old raw
`dxb_storage_add_queued_write()` helper is gone, leaving dirty-write queue
add/walk boundaries described by the same byte request shape used by direct
byte reads, writes, and vector writes. Verification passed `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.094` batch, `1.167` crud, `1.171` iterate, `0.921`
get, and `1.071` delete.

A later queued-write item descriptor cleanup changed `ior_item_t` to store a
`dxb_byte_io_t` instead of a raw offset. Queue coalescing now extends the stored
request byte span, queue walking derives completion ranges from that request,
and POSIX queued write submission uses `item->io.offset` for `pwrite()` and
`pwritev()`. The separate `last_bytes` bookkeeping in `osal_ioring_t` is gone,
leaving queued write geometry attached to the queued request item itself.
Verification passed `git diff --check`, `make -f GNUmakefile
mdbx_migration_smoke`, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.083` batch, `1.162` crud, `0.847` iterate, `0.968`
get, and `1.081` delete.

A later queued-write descriptor validation pass made the stored
`dxb_byte_io_t` authoritative at the POSIX queued submission boundary. The
`pwritev()` path now computes the queued iovec payload span and rejects a
descriptor mismatch with `MDBX_EINVAL`; the single-buffer fallback performs the
same check before writing. Single-buffer queued writes submit `item->io.bytes`
to `pwrite()`, so queued request length now comes from the stored descriptor
rather than the payload container. Verification passed `git diff --check`,
`make -f GNUmakefile mdbx_migration_smoke`, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `0.733` batch, `1.132` crud, `1.003` iterate, `0.861`
get, and `0.632` delete.

A later raw-storage adapter descriptor handoff changed the storage-local
`dxb_storage_pread()`, `dxb_storage_pwrite()`, and `dxb_storage_pwritev()`
helpers to receive a `dxb_byte_io_t` instead of unpacked `bytes` and `offset`
arguments. The adjacent async-style partial-writev fault-injection helper now
receives the same descriptor and verifies that its real prefix write fits
inside the request before issuing the test-only corruption write. This keeps the
byte request authoritative down to the final OSAL syscall handoff without
changing the OS abstraction itself. Verification passed `git diff --check`,
source scans proving the storage adapters and partial-writev hook no longer
receive loose byte/offset arguments, `make -f GNUmakefile
mdbx_migration_smoke`, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.133` batch, `1.163` crud, `1.239` iterate, `0.972`
get, and `1.067` delete.

A later environment-copy fast-path descriptor cleanup made the accelerated
source-copy paths use the same byte request shape as the portable fallback.
`copy_asis()` now builds a checked `dxb_byte_io_t` before calling the
`sendfile()` and destination-file `copy_file_range()` classifiers, while those
helpers keep mutable syscall offsets local and return only the advanced byte
count to the copy loop. Same-file page copies now convert the checked source
and destination `dxb_page_io_t` descriptors into `dxb_byte_io_t` spans before
calling the storage `copy_file_range()` wrapper, and the helpers reject advances
that exceed the requested descriptor span. Verification passed `git diff
--check`, source scans proving the fast-copy helpers no longer receive raw
source offset/length arguments, `make -f GNUmakefile mdbx_migration_smoke`,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.172` batch, `1.212`
crud, `1.502` iterate, `0.921` get, and `0.848` delete.

A later advice/discard descriptor cleanup changed storage readahead and
discard helpers to receive `dxb_byte_io_t` ranges instead of loose
offset/length pairs. Page prefetch now adapts its `dxb_page_io_t` into a byte
request before `F_RDADVISE`/`posix_fadvise()`, and resize/open/shrink tail
discard callers build checked byte descriptors before `DONTNEED` advice and
cache invalidation. The POSIX advice wrappers validate descriptor bounds before
casting to platform offsets, keeping fd-backed advisory operations shaped like
future async range requests. Verification passed `git diff --check`, source
scans proving advice/discard helpers no longer receive loose range arguments,
`make -f GNUmakefile mdbx_migration_smoke`, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.118` batch, `1.186` crud, `1.048` iterate, `0.962`
get, and `1.073` delete.

A later cache-invalidation descriptor cleanup changed the storage-local current
file-view probe and page-cache invalidation helper to receive `dxb_byte_io_t`
ranges instead of loose offset/length pairs. The coherency root-txnid probe now
builds one checked byte request, tests that descriptor against the current
storage size, and reuses it for the explicit read. Data-write, discard-remove,
and truncate-driven stale-tail invalidation now pass descriptors into the cache
invalidation boundary, keeping cache eviction keyed by the same byte request
shape used by explicit reads, writes, advice, and copy helpers. Verification
passed `git diff --check`, source scans proving the current-view and
invalidation helpers no longer receive loose range arguments, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.116` batch,
`1.164` crud, `1.004` iterate, `1.005` get, and `1.064` delete.

A later data-sync descriptor cleanup removed the standalone `dxb_sync_range_t`
page-bound pair from the C source. `dxb_sync_locked()` and the pre-writer
`env_sync()` path now build checked `dxb_page_io_t` descriptors from the
committed data range before calling `dxb_storage_sync_range()`, so the sync
boundary receives the same page request shape used by read, prefetch, write,
and cache ownership paths. The current backend still performs the same
whole-file sync underneath, but the storage API now carries validated page
geometry that a future range-aware or async backend can consume. Verification
passed `git diff --check`, source scans proving `dxb_sync_range_t` and
`dxb_sync_range_all()` are gone from the C source, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.090` batch,
`1.165` crud, `0.817` iterate, `1.054` get, and `1.095` delete.

A later descriptor-parking cleanup changed the data-file, dsync, and Windows
overlapped parking helpers to receive a checked zero-length `dxb_byte_io_t`
position request instead of a loose raw offset. `env_open()` now builds the
safe parking descriptor once, uses it for all data-file storage handles, and
continues to leave lock-file parking as a direct lock-file seek. This keeps the
data-file handle-positioning boundary shaped like the rest of the explicit
storage requests while preserving the existing corruption guard against
accidental relative descriptor use. Verification passed `git diff --check`,
source scans proving the storage parking helpers no longer receive loose raw
offsets, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.127` batch, `1.166` crud, `0.959`
iterate, `0.966` get, and `1.058` delete.

A later data-file lock range cleanup introduced `dxb_lock_io_t` for POSIX DXB
file-lock ranges that may need `OFF_T_MAX` extents without fitting the normal
`dxb_byte_io_t` `size_t` length on every target. The data-file lock operation
and retry helpers now receive this descriptor, validate offset/length bounds
before casting to `off_t`, and `lck_seize()`, `lck_downgrade()`,
`lck_upgrade()`, and `lck_destroy()` build checked whole-file, pid-slot, and
split-around-pid lock ranges before entering storage. Lock-file mmap and
lock-file record locking remain unchanged. Verification passed `git diff
--check`, source scans proving the data-file storage lock helpers no longer
receive loose raw offset/length pairs, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.113` batch, `1.156`
crud, `1.008` iterate, `0.994` get, and `1.087` delete.

A later storage geometry descriptor cleanup introduced `dxb_size_io_t` for the
data-file current-size/limit pair used by open setup and resize. `dxb_setup()`
and `dxb_resize()` now build a checked target-size descriptor before entering
the storage helpers, and the storage setup/resize entry points consume that
descriptor instead of loose `size`/`limit` arguments. This leaves file-length
mutation and storage current/limit bookkeeping behind the same validated
request-object style used by explicit reads, writes, sync ranges, advice,
parking, and locks. Verification passed `git diff --check`, source scans
proving the old `dxb_storage_setup_bytes()` and `dxb_storage_resize_bytes()`
helpers are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.096` batch, `1.155`
crud, `1.044` iterate, `0.978` get, and `1.097` delete.

A later page-helper descriptor cleanup removed the remaining page-number
wrapper entry points for storage writes, vector writes, same-file page copies,
prefetch, and dirty-write queue add/prepare. Callers now build checked
`dxb_page_io_t` requests before entering `dxb_storage_write_io()`,
`dxb_storage_writev_io()`, `dxb_storage_copy_io()`,
`dxb_storage_prefetch_io()`, `dxb_storage_add_queued_io()`, and
`dxb_storage_prepare_write_queue_io()`. This keeps page-addressed storage work
described before it reaches the storage layer, and lets the storage layer adapt
only from page descriptors to byte descriptors or raw syscalls. The old
`dxb_storage_write_pages()`, `dxb_storage_writev_pages()`,
`dxb_storage_copy_pages()`, `dxb_storage_prefetch_pages()`,
`dxb_storage_add_queued_pages()`, and
`dxb_storage_prepare_write_queue_pages()` helpers are gone from `mdbx.c`.
Verification passed `git diff --check`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.112` batch, `1.161`
crud, `1.038` iterate, `1.106` get, and `1.057` delete.

A later filesize descriptor cleanup introduced `dxb_filesize_io_t` for
data-file length observations and mutations. The storage filesize read,
truncate/extend, fault-injected setsize path, checker bookkeeping, and
creation-time current-size update now pass file-length descriptors instead of
raw byte counts. The coherency path that refreshes the observed filesize now
passes the required data-file span as a `dxb_byte_io_t` range into
`dxb_storage_fetch_filesize_if_current_lacks()`, keeping the refresh trigger
keyed by the same range shape used by explicit reads and writes. Verification
passed `git diff --check`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.121` batch, `1.137` crud, `1.249`
iterate, `0.967` get, and `1.086` delete.

A later cache-invalidation page descriptor cleanup added
`dxb_storage_page_io_from_bytes()` and changed the storage page-cache invalidator
to consume `dxb_page_io_t` ranges instead of raw begin/end page numbers. Byte
range invalidation now converts its `dxb_byte_io_t` span into a checked page
descriptor before evicting overlapping cache entries, and write/copy paths pass
their existing page I/O descriptors directly into invalidation. This keeps
cache eviction keyed by the same explicit page range shape used by storage
reads, writes, copy, prefetch, sync, and queue preparation. Verification passed
`git diff --check`, source scans proving the old raw page invalidator is gone
from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.154` batch, `1.154` crud, `1.200`
iterate, `0.957` get, and `1.087` delete.

A later public-cache descriptor cleanup kept `MDBX_cache_entry_t` unchanged but
added internal conversion helpers for its public data-file offset/length pair.
Cache-hit materialization now adapts an entry into a checked `dxb_byte_io_t`
before validating the used range, deriving the containing page request, and
returning the pinned value bytes. Cache refresh now derives a `dxb_byte_io_t`
from the cursor-retained value reference, then stores it back to the public
entry only when the descriptor fits the public offset/length fields; otherwise
the already-found value is returned as `MDBX_CACHE_UNABLE` instead of recording
a lossy cache entry. The migration smoke source explicitly covers
`mdbx_cache_get()` refresh and `mdbx_cache_get_SingleThreaded()` hit paths.
Verification passed `git diff --check`, source scans proving the old
`cache_offset_from_ref()` and `cache_value_offset()` helpers are gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.120` batch, `1.148` crud, `1.242`
iterate, `1.406` get, and `1.073` delete.

A later meta-shadow descriptor cleanup added checked
`dxb_storage_meta_payload_io()` and `dxb_storage_meta_field_io()` helpers.
Commit-meta writes and steady-sign wipes now build byte descriptors for the
metadata payload/sign spans before writing, and the in-memory `meta_shadow`
mirror consumes those same `dxb_byte_io_t` requests instead of separate raw meta
numbers and field offsets. Whole-page meta overrides pass their existing
`dxb_page_io_t` descriptor into the shadow copy path as well. This keeps the
disk metadata write and shadow metadata update keyed by the same explicit
request descriptors. Verification passed `git diff --check`, source scans
proving `meta_shadow_copy_field()` is gone and raw meta payload/field write
descriptor construction is gone from call sites in `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.112` batch, `1.151`
crud, `0.841` iterate, `0.929` get, and `1.076` delete.

A later readahead-window descriptor cleanup added `dxb_readahead_io_t` and
`dxb_storage_readahead_io()` so `dxb_set_readahead()` describes each computed
window once. Advisory calls now use the descriptor's checked byte range, while
prefetch uses the descriptor's derived page range instead of rebuilding
begin/end page numbers locally. This keeps readahead advice and prefetch keyed
by one explicit request shape, matching the descriptor model used by resize,
discard, cache invalidation, sync, and page I/O paths. Verification passed
`git diff --check`, source scans proving the stale `begin_pgno`/`end_pgno` locals
and local prefetch descriptor are gone from `dxb_set_readahead()`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.119` batch, `1.152` crud, `0.959` iterate, `0.983`
get, and `1.073` delete.

A later page-to-byte adapter cleanup added `dxb_storage_byte_io_from_page()`
for storage paths that need to hand a checked `dxb_page_io_t` range to
byte-addressed backends. Page prefetch, explicit page reads and writes, queued
dirty-page insertion/writev submission, same-file page copy, and
whole-meta-page shadow updates now derive their `dxb_byte_io_t` through that
adapter instead of rebuilding byte descriptors from raw `offset`/`bytes` fields
locally. This keeps the page range descriptor authoritative until the exact
byte-oriented storage boundary.
Verification passed `git diff --check`, source scans proving the old direct
page-descriptor `offset`/`bytes` compound literals are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `0.997` batch, `0.991` crud, `0.852` iterate, `1.065`
get, and `1.003` delete.

A later sync-range descriptor cleanup added `dxb_sync_io_t` and
`dxb_storage_sync_io()` so data-page sync callers describe the range and sync
flags as one request. Commit-time data sync and writer-free pre-sync now build
checked sync descriptors with both page and byte spans before entering
`dxb_storage_sync_range()`, which validates the derived byte span before
preserving the current whole-file `fsync()` behavior. This keeps the durable
sync boundary ready for future range-aware or async completion backends without
changing current persistence semantics. Verification passed `git diff --check`,
source scans proving the old `dxb_page_io_t sync_range` locals and separate
`dxb_storage_sync_range(..., mode_bits)` calls are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.106` batch, `1.163` crud, `1.003` iterate, `0.991`
get, and `1.076` delete.

A later byte-span descriptor cleanup added `dxb_storage_byte_span_io()` for
checked `[begin, end)` byte ranges. Warmup forced reads, env-copy fast paths
and fallback, resize/open-tail discard, and commit shrink discard now build
checked span descriptors instead of local `end - begin` byte-length arithmetic.
The filesize stale-tail invalidation path keeps its capped range behavior.
Verification passed `git diff --check`, source scans proving the targeted local
span arithmetic was removed from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.119` batch, `1.178`
crud, `1.164` iterate, `0.937` get, and `1.089` delete.

A later committed-page read descriptor cleanup added `dxb_read_io_t` for page
reads that need both page and byte spans before reaching the storage backend.
Meta-shadow refresh, defrag fallback page reads, ordinary page-cache fills, and
large-page materialization now build checked read descriptors and
`dxb_storage_read_pages()` validates the byte span against the page descriptor
before issuing the synchronous `pread()`. Byte-only reads remain limited to
field probes, warmup, copy, and other non-page spans. Verification passed
`git diff --check`, source scans proving the old page-descriptor
`dxb_storage_read_io(..., dxb_page_io_t, ...)` executor shape is gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.103` batch, `1.158` crud, `0.832` iterate,
`1.047` get, and `1.074` delete.

A later page-write descriptor cleanup added `dxb_write_io_t` so page writes
carry the write channel together with checked page and byte spans. Defrag
writebacks, database creation meta triplets, whole-meta overrides, queued dirty
page preparation/submission, page-kill write/writev paths, and same-file copy
cache invalidation now build write descriptors before crossing the storage
write boundary. `dxb_storage_write_pages()`, `dxb_storage_writev_pages()`, and
the queued write helpers validate the descriptor's byte span against the page
span before calling the synchronous write backend. Byte-only metadata field
writes still used `dxb_storage_write_bytes()` at this checkpoint. Verification passed
`git diff --check`, source scans proving the old
`dxb_storage_write_io(..., dxb_page_io_t, ...)` and `dxb_storage_writev_io()`
executor shapes are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.134` batch, `1.141`
crud, `1.026` iterate, `1.004` get, and `1.082` delete.

A later same-file copy descriptor cleanup added `dxb_copy_io_t` so
`copy_file_range()` page moves carry source and destination page/byte spans as
one checked request. Defrag overflow-tail same-file copy now builds that copy
descriptor before crossing the storage boundary, and
`dxb_storage_copy_pages()` validates both page-derived byte spans before calling
the synchronous `copy_file_range()` backend. Destination cache invalidation
continues through the write descriptor path. Verification passed
`git diff --check`, source scans proving the old separate source/destination
`dxb_page_io_t` copy call shape is gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.118` batch, `1.177` crud, `1.260` iterate, `0.938`
get, and `1.076` delete.

A later metadata-write descriptor cleanup added `dxb_meta_io_t` so metadata
writes carry the logical meta slot, payload offset, payload length, and checked
byte span as one request. Full meta payload writes in `dxb_sync_locked()` and
steady-sign wipes in `meta_unsteady()` now submit through
`dxb_storage_write_meta()`, which rebuilds and validates the descriptor before
the synchronous byte write. Meta-shadow copy helpers now take the same typed
descriptor, so the in-memory shadow update is tied to the same slot/field span
as the disk write. Verification passed `git diff --check`, source scans proving
the old meta helper byte signatures and direct meta-byte write call sites are
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.119` batch, `1.167` crud, `0.817` iterate,
`1.048` get, and `1.077` delete.

A later readahead-advice descriptor cleanup kept `dxb_readahead_io_t` intact
through storage advice and prefetch submission. `dxb_set_readahead()` now
passes the checked window descriptor to `dxb_storage_advise_readahead_io()` for
normal/random advice and to `dxb_storage_prefetch_readahead_io()` for
`WILLNEED` prefetch; both helpers rebuild and validate the page span from the
byte window before touching the fd-backed advice path. This leaves the raw
`dxb_storage_advise_range()` helper below the storage request boundary instead
of exposing separate byte/page window members to callers. Verification passed
`git diff --check`, source scans proving the old `dxb_storage_prefetch_io()`
helper and direct `window.bytes`/`window.pages` advice calls are gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.125` batch, `1.153` crud, `0.743` iterate,
`0.876` get, and `1.076` delete.

A later discard descriptor cleanup added `dxb_discard_io_t` so data-file
DONTNEED/remove requests carry the checked byte span, derived page span, and
discard mode as one storage request. Resize shrink, open-tail discard, and
commit-time shrink discard now build that descriptor before crossing into
storage, and `dxb_storage_discard_range()` rebuilds and validates the
page-derived view before choosing clean/remove behavior and cache
invalidation. The raw byte-only `posix_fadvise(POSIX_FADV_DONTNEED)` helper now
stays below the storage request boundary. Verification passed
`git diff --check`, source scans proving the old
`dxb_storage_discard_range(..., mode)` call shape and local byte-only discard
requests are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.132` batch, `1.135`
crud, `0.975` iterate, `0.981` get, and `1.087` delete.

A later outbound-copy descriptor cleanup added `dxb_outbound_io_t` so
environment-copy fast paths carry the DXB source byte span, derived source page
span, destination fd, and optional destination offset as one storage request.
The `sendfile()` pipe path and destination-file `copy_file_range()` path in
`copy_asis()` now build that outbound descriptor before entering storage, and
the storage helpers rebuild and validate the source page span and destination
state before issuing the synchronous syscall. The portable fallback remains a
plain byte read because it stages source bytes through a caller-owned buffer.
Verification passed `git diff --check`, source scans proving the old
`dxb_storage_sendfile_to_fd(..., fd, dxb_byte_io_t, ...)`,
`dxb_storage_copy_to_fd(..., fd, dxb_byte_io_t, ...)`, and local
`out_offset` call shapes are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.134` batch, `1.141` crud, `1.014` iterate, `1.016`
get, and `1.061` delete.

A later current-coverage descriptor cleanup added `dxb_coverage_io_t` so
storage current-size checks carry the checked byte span and derived page span
together. Coherency root probes now build that descriptor before asking whether
the known current file view contains a root-txnid read, and
`coherency_fetch_head()` builds the same descriptor before asking storage to
refresh filesize when `current` does not cover `first_unallocated`.
`dxb_storage_fetch_filesize_if_current_lacks()` validates the descriptor before
consulting cached current-size state or fetching filesize. Verification passed
`git diff --check`, source scans proving raw current-coverage checks no longer
cross the storage request boundary, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.113` batch, `1.177`
crud, `0.876` iterate, `0.982` get, and `1.075` delete.

A later cache-invalidation descriptor cleanup removed the byte-to-page
invalidation helper from the storage cache boundary. Page writes now invalidate
through their already validated `dxb_write_io_t` page descriptor after the
single-buffer write succeeds, matching vector page writes. Destructive discard
paths invalidate through the validated `dxb_discard_io_t` page span, and
filesize shrink invalidation builds one page descriptor for the stale tail
before entering the page-cache invalidator. This keeps cache eviction keyed by
the same page request shapes used by reads, writes, copies, and discards,
instead of re-deriving page spans from byte requests at the final cache
boundary. Verification passed `git diff --check`, source scans proving
`dxb_storage_invalidate_cached_bytes()` is gone and page/discard/write
invalidation routes through `dxb_storage_invalidate_cached_io()`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.097` batch, `1.197` crud, `1.067` iterate, `1.009`
get, and `1.073` delete.

A later storage-size descriptor validation cleanup added
`dxb_storage_size_io_validate()` so setup and resize executors rebuild and
validate their `dxb_size_io_t` request before using its current/limit values to
set file length, refresh observed filesize, or mutate cached storage geometry.
This keeps the storage geometry boundary aligned with the descriptor-validation
pattern used by read, write, sync, discard, copy, outbound, and coverage
requests. Verification passed `git diff --check`, source scans proving
`dxb_storage_setup_size()` and `dxb_storage_resize_size()` validate
`dxb_size_io_t` before use, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.131` batch, `1.167` crud, `0.973` iterate,
`0.991` get, and `1.078` delete.

A later storage-size state mutation cleanup routed storage current/limit
updates through validated size descriptors too. `dxb_storage_set_size()` now
accepts a `dxb_size_io_t`, validates it at the mutation point, and returns an
error instead of installing loose current/limit values. The known-filesize,
limit-from-filesize, and observed-filesize update helpers now build or forward
that descriptor and propagate failures through filesize refresh, setup, resize,
and transaction-start filesize reconciliation paths. Verification passed
`git diff --check`, source scans proving storage size state setters no longer
take loose current/limit pairs and all callers handle their status, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.162` batch, `1.170` crud, `0.801` iterate, `1.093`
get, and `1.074` delete.

A later filesize descriptor validation cleanup added
`dxb_storage_filesize_io_validate()` so filesize observations, file-length
changes, and in-memory filesize/current updates rebuild and validate
`dxb_filesize_io_t` before crossing storage state or filesystem boundaries.
`dxb_storage_set_filesize()` and `dxb_storage_set_current()` now return status,
the current-size setter consumes the same filesize descriptor instead of a
loose `size_t`, and the combined size/filesize setter validates both
descriptors before mutating storage geometry state. Filesize reads now build
their descriptor through `dxb_storage_filesize_io()`, known-filesize geometry
helpers rebuild descriptors through the same constructor path, and checker plus
filesize refresh paths propagate descriptor failures. Verification passed
`git diff --check`, source scans proving loose current-size setters and raw
filesize reads are gone from the targeted storage boundary, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.116` batch, `1.156` crud, `0.873` iterate, `1.041`
get, and `1.079` delete.

A later byte request validation cleanup added
`dxb_storage_byte_io_validate()` so raw byte-span requests are rebuilt and
checked before they cross byte-to-page conversion, cache-coverage checks, queued
write insertion, advisory/discard calls, raw read/write/writev submission,
same-file copy, and the partial-write fault-injection hook. This keeps the
lowest storage syscall boundary aligned with the descriptor-validation pattern:
page, meta, copy, discard, sync, and outbound descriptors may still carry richer
shape, but their final `dxb_byte_io_t` span is now rejected if its offset/length
cannot be represented as a checked byte request. Verification passed
`git diff --check`, source scans proving targeted byte consumers call
`dxb_storage_byte_io_validate()`, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.114` batch, `1.168`
crud, `0.740` iterate, `1.069` get, and `1.070` delete.

A later page request validation cleanup added
`dxb_storage_page_io_validate()` so page-number requests are rebuilt and checked
against the storage page size before they enter page-cache lookup/fill,
metadata shadow copies, cache invalidation, sync, read, write, vector-write,
queued-write preparation/insertion, and same-file copy paths. The page-derived
read/write/copy descriptor helpers now take `dxb_storage_t`, validate their
source `dxb_page_io_t`, and then derive the byte request, so `pgno`, `end_pgno`,
`npages`, `offset`, and `bytes` remain one checked request shape across the
page-cache, dirty-write queue, and storage submission boundaries. Verification
passed `git diff --check`, source scans proving page descriptor construction
and page consumers call `dxb_storage_page_io_validate()`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.010` batch, `0.987` crud, `0.979` iterate, `0.976`
get, and `0.998` delete.

A later metadata request validation cleanup added
`dxb_storage_meta_io_validate()` so meta slot/payload descriptors are rebuilt
and checked before metadata writes and shadow updates consume them.
`dxb_storage_write_meta()`, `meta_shadow_copy_payload()`, and
`meta_shadow_copy_bytes()` now reject or skip malformed meta requests before
crossing the explicit meta I/O or in-memory shadow-copy boundary. Verification
passed `git diff --check`, source scans proving metadata consumers call
`dxb_storage_meta_io_validate()`, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.113` batch, `1.175`
crud, `1.018` iterate, `0.990` get, and `1.069` delete.

A later sync request validation cleanup added
`dxb_storage_sync_io_validate()` so range-sync descriptors are rebuilt from
their page range and mode before the storage sync boundary consumes them.
`dxb_storage_sync_range()` now rejects malformed sync descriptors through the
same storage-owned request validation pattern used by byte, page, metadata,
filesize, coverage, and readahead requests. Verification passed
`git diff --check`, source scans proving the sync boundary calls
`dxb_storage_sync_io_validate()`, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, `cmake
--build @cmake-ninja-build`, the six focused `migration_smoke` CTest entries,
the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.137` batch, `1.168`
crud, `0.930` iterate, `1.005` get, and `1.084` delete.

A later read/write request validation cleanup added
`dxb_storage_read_io_validate()` and `dxb_storage_write_io_validate()` so page
read/write descriptors are rebuilt from their page ranges before page reads,
page writes, vector page writes, write-queue preparation, and queued-write
insertion consume them. Write descriptor construction and byte write/writev
submission now also reject invalid storage channels before resolving a DXB file
descriptor. Verification passed `git diff --check`, source scans proving the
page read/write and queued-write boundaries call the new validators and no
longer carry the old inline page-to-byte validation blocks, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.109` batch, `1.149` crud, `1.020` iterate, `1.012`
get, and `1.065` delete.

A later file-range request validation cleanup added
`dxb_storage_copy_io_validate()`, `dxb_storage_outbound_io_validate()`, and
`dxb_storage_discard_io_validate()` so same-file page copies, outbound
copy/sendfile requests, and discard requests are rebuilt from their canonical
byte/page descriptors before the storage executor consumes them. The old
executor-local discard, copy, and outbound validation blocks are gone, leaving
those file-range operations aligned with the descriptor-validation path used by
read, write, sync, metadata, and readahead requests. Verification passed
`git diff --check`, source scans proving file-range executors call the new
validators and the stale inline validation patterns appear only inside those
validators, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.124` batch, `1.151` crud, `0.976` iterate,
`0.994` get, and `1.081` delete.

A later DXB lock request validation cleanup added
`dxb_storage_lock_io_validate()` so POSIX data-file lock ranges are rebuilt and
checked before the storage lock helpers submit them to `fcntl()`.
`dxb_storage_lock_op()` and `dxb_storage_setlk_with3retries()` now validate the
range descriptor before converting it to `off_t`, keeping DXB lock routing on
the same descriptor-validation pattern as the explicit read/write/copy/sync
paths. Verification passed `git diff --check`, source scans proving the DXB
lock executors call `dxb_storage_lock_io_validate()`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.132` batch, `1.163` crud, `0.976` iterate, `0.997`
get, and `1.084` delete.

A later descriptor boundary cleanup made three remaining byte/coverage request
edges rebuild through the storage-owned constructors before use.
`dxb_storage_contains_coverage()` now validates the complete
`dxb_coverage_io_t` byte/page pair before comparing it with the current storage
size, truncate-driven stale-tail cache invalidation builds its `dxb_byte_io_t`
through `dxb_storage_byte_io()`, and `osal_ioring_walk()` constructs each dirty
write completion callback span through the same byte-request constructor
instead of open-coded aggregate initializers. This leaves only intentional
zero-initialized output descriptors in the raw descriptor initializer scan and
keeps queued-write walking aligned with the explicit async submission shape.
Verification passed `git diff --check`, source scans proving raw
`dxb_byte_io_t` aggregate initializers are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.131` batch, `1.170` crud, `0.931` iterate, `0.889`
get, and `1.067` delete.

A later syscall-boundary validation cleanup made the final explicit storage
adapters and POSIX dirty-write queue executor revalidate byte request
descriptors immediately before raw file I/O. `dxb_storage_pread()`,
`dxb_storage_pwrite()`, and `dxb_storage_pwritev()` now validate the
`dxb_byte_io_t` they receive, and the write adapters also reject invalid
storage channels before selecting a DXB fd. The POSIX `osal_ioring_write_item()`
path now validates the queued item's stored byte descriptor and payload span
before submitting `pwrite()` or `pwritev()`, so queue insertion, queue walking,
fault-injection, and the final synchronous executor all enforce the same
request shape that a future async backend will submit. Verification passed
`git diff --check`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.101` batch, `1.144` crud, `0.996` iterate,
`0.997` get, and `1.074` delete.

A later queue-drain validation cleanup moved the queued-item byte descriptor
check into common OSAL code and applies it to the Windows `WriteFileGather()`,
`WriteFileEx()`, and `WriteFile()` submission branches after their actual
payload byte counts are derived. POSIX and Windows queue drains now enforce the
same stored `dxb_byte_io_t` request shape before issuing platform writes, so a
future async backend can rely on queue items being validated at insertion,
walking, fault-injection, and final submission boundaries. Verification passed
`git diff --check`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.143` batch, `1.170` crud, `1.121` iterate,
`0.963` get, and `1.085` delete.

A later POSIX copy fast-path validation cleanup made the raw
`copy_file_range()` and `sendfile()` storage wrappers revalidate the checked
byte/outbound descriptors immediately before syscall submission. The wrappers
now derive their own local `off_t` source offsets from the descriptor shape,
validate destination offsets for outbound `copy_file_range()`, and normalize
descriptor rejection through `errno` before the higher-level copy classifiers
decode syscall outcomes. This removes the last loose source-offset handoff from
the accelerated environment-copy and same-file copy path, keeping those raw
syscall boundaries aligned with the explicit request objects used by the
portable read/write/copy fallback. Verification passed `git diff --check`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.143` batch, `1.159` crud, `0.998` iterate, `1.002`
get, and `1.078` delete.

A later page-span byte request cleanup added
`dxb_storage_page_span_bytes_io()` and `dxb_storage_page_field_io()` so callers
that need a byte window inside a page or page span build it through checked
page geometry before producing the final `dxb_byte_io_t`. The fast public cache
value offset path now derives cache-entry byte windows from the retained page
ref's checked `(pgno, npages)` span, and the root-txnid coherency probe reads
`page_t.txnid` through the checked page-field helper instead of hand-building
`pgno2bytes() + offset`. The old raw `dxb_storage_page_field_offset()` helper
is gone. Verification passed `git diff --check`, source scans proving the old
raw page-field helper and targeted hand-built offsets are gone, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.007` batch, `0.983` crud, `1.031` iterate, `1.005`
get, and `0.990` delete.

A later cache materialization request cleanup added
`dxb_storage_byte_start_page_io()` so a checked byte request can produce the
first page-cache read descriptor plus the byte offset inside that page. Fast
public cache-hit materialization now validates its stored value byte range
against a checked transaction-used page span, reads the starting page through
that descriptor, and validates post-overflow materialization with
`dxb_storage_page_span_bytes_io()` instead of open-coding `bytes2pgno()`,
`pgno2bytes()`, and large-page span arithmetic. Verification passed
`git diff --check`, source scans proving the targeted cache-materialization
raw conversions are gone, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.124` batch, `1.166` crud, `1.041` iterate,
`0.977` get, and `1.067` delete.

A later non-compacting copy source-range cleanup added
`dxb_storage_outbound_io_from_page_span()` so accelerated outbound copies can
start from a validated page descriptor and derive the source byte request from
that descriptor before entering storage. `copy_asis()` now constructs a checked
used-page span for `0..first_unallocated` and builds the `sendfile()`,
`copy_file_range()`, and portable fallback DXB source ranges from that
descriptor instead of repeatedly passing loose `offset..used_size` byte spans
through copy planning. Verification passed `git diff --check`, source scans
proving the targeted loose copy-source spans are gone, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.106` batch, `1.159` crud, `1.003` iterate, `0.994`
get, and `1.070` delete.

A later transaction/setup page-prefix cleanup added
`dxb_storage_page_prefix_io()` so storage-bound uses of `0..end_pgno` ranges
validate the page descriptor before consuming the derived byte size.
Compaction-copy output extension, non-compacting copy used-page
setup, public-cache materialization bounds, coherency snapshot file-coverage
checks, open/setup allocated-range handling, read/write transaction coverage
assertions, Windows read-transaction shrink checks, and `txn_setup_primal()`
now use the checked prefix descriptor instead of open-coding
`pgno2bytes()`/round-trip validation for those page-prefix decisions.
Verification passed `git diff --check`, source scans proving the targeted raw
page-prefix conversions are gone, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.144` batch, `1.172`
crud, `1.158` iterate, `0.991` get, and `1.074` delete.

A later coverage-descriptor cleanup added
`dxb_storage_coverage_io_from_page_prefix()` so coherency snapshot coverage
checks carry the validated `0..first_unallocated` page-prefix descriptor into
the file-size refresh path instead of rebuilding the same range as loose
`0..required_bytes` byte endpoints. The now-unused loose outbound-copy
constructor and loose coverage constructor are gone, leaving accelerated
outbound copies page-span based and coverage requests either byte-descriptor or
page-prefix based. Verification passed `git diff --check`, source scans
proving the removed loose constructors are absent, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.128` batch, `1.155` crud, `1.009` iterate, `1.027`
get, and `1.068` delete.

A later warmup read-range cleanup added `dxb_storage_byte_subrange_io()` so a
validated byte-range descriptor can produce checked child read requests.
`mdbx_env_warmup()` now builds a single `warmup_range` descriptor for the
selected used range, and `warmup_force_read()` validates/clamps that descriptor
against the current storage size before deriving each forced-read chunk as a
checked subrange instead of rebuilding raw `offset..offset+bytes` spans inside
the loop. The migration smoke harness explicitly covers default warmup,
forced/OOM-safe warmup, and no-data-mmap lock-warmup behavior. Verification
passed `git diff --check`, source scans proving the targeted warmup
`offset + bytes` request is gone, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.114` batch, `1.167`
crud, `0.918` iterate, `1.016` get, and `1.088` delete.

A later discard-range cleanup removed the loose begin/end discard constructor
and its byte-span wrapper. Resize shrink advice, setup-time tail discard, and
sync-time shrink discard now build checked `dxb_byte_io_t` descriptors at the
call site before deriving the `dxb_discard_io_t`, so file-discard requests enter
the storage layer through descriptor validation instead of hidden endpoint
conversion. Verification passed `git diff --check`, source scans proving the
removed loose discard helpers are absent, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.119` batch, `1.162`
crud, `1.010` iterate, `0.951` get, and `1.080` delete.

A later readahead-window cleanup replaced the raw `offset,length` readahead
constructor with `dxb_storage_readahead_io_from_bytes()`. `dxb_set_readahead()`
now clamps the advice window against the storage limit, builds a checked
`dxb_byte_io_t`, and derives the page-aware readahead request from that
descriptor before issuing prefetch or advice calls. Verification passed
`git diff --check`, source scans proving the raw `dxb_storage_readahead_io()`
constructor is absent, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.147` batch, `1.154`
crud, `1.136` iterate, `0.959` get, and `1.078` delete.

A later file-size shrink cleanup added `dxb_storage_filesize_shrink_tail_io()`
so truncation-driven cache invalidation derives the stale tail from a validated
file-size target descriptor instead of rebuilding `target->bytes + stale_bytes`
inline in `dxb_storage_set_filesize_io()`. The shrink path now sets the file
size on disk, asks the descriptor helper for the stale byte range, converts that
range to pages, and invalidates cached pages from the checked descriptor.
Verification passed `git diff --check`, source scans proving the old
`stale_bytes64` inline request is gone, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.116` batch, `1.148`
crud, `1.281` iterate, `0.946` get, and `1.081` delete.

A later startup meta-probe cleanup added `dxb_storage_meta_probe_io()` for the
bootstrapping read path that must probe meta pages before the database page size
is known. `dxb_read_header()` now derives each minimum-page double-read request
from a checked `(probe_pagesize, meta_number)` descriptor and logs the descriptor
offset/length instead of open-coding `offset, MDBX_MIN_PAGESIZE` in the retry
loop. Verification passed `git diff --check`, source scans proving the old raw
startup meta-probe request and stale `%u,%u` meta-read log formats are gone, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.117` batch, `1.158` crud, `0.994` iterate, `0.994`
get, and `1.083` delete.

A later warmup scan-range cleanup changed `warmup_force_read()` to derive its
current-file clamped scan descriptor as a checked subrange of the already
validated warmup range. The forced-read loop still clamps the requested range to
the current storage size and reads it in aligned chunks, but the scan window now
flows through `dxb_storage_byte_subrange_io(range, 0, used_range, ...)` instead
of rebuilding `range->offset, used_range` as a loose byte request. Verification
passed `git diff --check`, source scans proving the targeted
`dxb_storage_byte_io(range->offset, ...)` request is absent, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.135` batch,
`1.161` crud, `1.157` iterate, `0.936` get, and `1.069` delete.

A later readahead edge-window cleanup added
`dxb_storage_readahead_window_bytes_io()` so `dxb_set_readahead()` derives the
clamped byte descriptor for `(prev_edge, edge, toggle)` through a
storage-boundary helper before converting it to the page-aware readahead
request. The old local `offset`/`length` construction and direct
loose byte-request path are gone from the readahead toggle logic. Verification
passed `git diff --check`, source scans proving the old loose readahead
byte-window construction is absent, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.131` batch, `1.142`
crud, `0.871` iterate, `0.997` get, and `1.068` delete.

A later byte-span call-site cleanup uses `dxb_storage_byte_span_io()` for
callers that already reason in begin/end byte bounds. Warmup range setup,
resize shrink discard, open-time tail discard, sync-time shrink discard, and
the data-fd parking-lot request now derive their checked `dxb_byte_io_t`
through that helper instead of rebuilding offset/length pairs locally.
Verification passed `git diff --check`, source scans proving the targeted raw
warmup/discard/parking-lot byte constructors are absent, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.121` batch,
`1.157` crud, `1.052` iterate, `0.943` get, and `1.069` delete.

A later byte-span helper reuse cleanup changed descriptor helpers that already
derive byte begin/end bounds to finish through `dxb_storage_byte_span_io()`.
Byte subranges, page-derived byte descriptors, page-span byte descriptors, and
readahead edge windows now share the same span validation path instead of
finishing with helper-local offset/length construction. Verification passed
`git diff --check`, source scans proving the targeted helper-local raw byte
constructors are absent, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.125` batch, `1.181` crud, `1.020` iterate,
`0.953` get, and `1.062` delete.

A later primitive byte-constructor cleanup routed the remaining metadata,
public-cache, filesize-tail, and dirty-write-walk byte request builders through
`dxb_storage_byte_span_io()`. The primitive `dxb_storage_byte_io()` constructor
is now confined to its own validator/span layer, while meta probes, meta
payload writes, public cache entries, capped stale-file tails, and ioring walk
callbacks use checked byte spans before storage submission. Verification passed
`git diff --check`, source scans proving `dxb_storage_byte_io()` is only used
inside the primitive layer, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.131` batch, `1.165` crud, `1.058` iterate,
`0.935` get, and `1.087` delete.

A later meta payload descriptor cleanup changed `dxb_storage_meta_io()` to
derive metadata payload byte requests through `dxb_storage_page_field_io()`
from `(meta slot, PAGEHDRSZ + payload_offset, bytes)`. The private meta
payload/field offset helpers are gone; `dxb_storage_meta_page_offset()` remains
for env-owned meta-shadow page addressing. Verification passed `git diff
--check`, source scans proving the removed helpers are absent and the retained
page-offset helper is limited to shadow-page layout, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.112` batch,
`1.163` crud, `1.197` iterate, `0.926` get, and `1.082` delete.

A later descriptor subrange cleanup added
`dxb_storage_page_subrange_bytes_io()` so callers that already hold a checked
`dxb_page_io_t` can derive byte subranges from that descriptor instead of
rebuilding the same page span from `(pgno, npages)`. Non-compacting environment
copy fallback reads and outbound copy/sendfile request construction now keep
the existing used-page descriptor authoritative through byte-range derivation.
Dirty-write queue walking now derives callback requests with
`dxb_storage_byte_subrange_io(&item->io, ...)`, so each callback range is
validated as a subrange of the queued item's stored byte descriptor instead of
reconstructing an absolute `offset..offset+bytes` span. Verification passed
`git diff --check`, source scans proving `osal_ioring_walk_bytes()` no longer
builds raw absolute byte spans and descriptor-owning copy paths use the
page-subrange helper, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.097` batch, `1.165` crud, `0.959` iterate,
`0.977` get, and `1.062` delete.

A later page-ref descriptor cleanup added `dxb_storage_page_ref_io()` and
`dxb_storage_page_ref_bytes_io()`. Public-cache value-range reconstruction now
derives byte requests from the page ref that backs the returned value; cache
refs validate and use their owning `page_cache_entry_t` descriptor, while
non-cache refs still rebuild through the ordinary page descriptor constructor.
Cached entry materialization now validates the materialized value span through
the same page-ref byte helper after any large-page expansion. This keeps
returned-value byte ranges tied to the pinned page/cache descriptor instead of
duplicating `(pgno, npages)` conversion in the public cache path. Verification
passed `git diff --check`, source scans proving the targeted public-cache
`dxb_storage_page_span_bytes_io()` callers are gone and the new page-ref helper
is used for value span reconstruction, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.131` batch, `1.149`
crud, `1.263` iterate, `0.940` get, and `1.081` delete.

A later large-page page-ref descriptor cleanup added
`dxb_storage_page_ref_span_io()`. Large overflow materialization now validates
the pinned cache ref against its owning cache-entry descriptor and derives the
expanded read request from that descriptor's start page, instead of rebuilding
from `pgr->ref.pgno` directly. The first attempt over-constrained cache refs by
requiring `ref->npages == cache->io.npages`; direct migration smoke caught this
in the defrag page walker, where overflow validation may set `ref->npages` from
the page header before cache materialization expands `cache->io`. The final
helper keeps the cache descriptor authoritative for storage identity while
allowing the caller-selected span to drive overflow expansion. Verification
passed `git diff --check`, source scans proving the raw `pgr->ref.pgno`
materialization call is gone and the over-strict comparison is absent, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs after the crash fix, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.079` batch, `1.152` crud, `1.187` iterate,
`1.014` get, and `1.071` delete.

A later transaction coverage descriptor cleanup removed
`dxb_storage_current_covers_page_prefix()` so transaction start paths no longer
hide page-prefix descriptor construction behind a boolean helper.
`basal_start_locked()` now builds the checked used-page prefix under
`CHECKS0_ENABLED()` before asserting current storage coverage, while
`txn_ro_start()` reuses the explicit prefix descriptor for the Windows shrink
decision and builds the same checked descriptor for non-Windows coverage
assertions. Verification passed `git diff --check`, source scans proving the
removed helper is absent, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.112` batch, `1.164` crud, `1.010` iterate, `1.004`
get, and `1.069` delete.

A later explicit-buffer pointer cleanup added `page_from_storage_buffer()` so
known explicit I/O buffers map pointers back to page numbers through validated
`dxb_page_io_t` spans. Page-cache lookups now use each cache entry's stored page
descriptor for range size and page-number derivation, while dirty-list lookups
build a checked descriptor from the dirty entry's `(pgno, npages)` before
checking whether a user pointer belongs to that buffer. This removes the raw
`pgno2bytes(scan->env, dpl_npages(...))` range calculation and direct
pagesize-shift page-number recovery from that explicit-buffer path. Verification
passed `git diff --check`, source scans proving the targeted dirty-list raw
range calculation and direct shift recovery are gone, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.139` batch,
`1.163` crud, `0.944` iterate, `1.067` get, and `1.092` delete.

A later queued-write release cleanup added
`dxb_storage_exact_page_io_from_bytes()` so queued byte descriptors can be
validated as exact page spans before being used as page ranges.
`iov_callback4dirtypages()` now derives the queued page descriptor once, checks
shadow-release bounds against that descriptor, and advances multi-page releases
with checked `dxb_page_io_t` chunks instead of converting queued offsets and
lengths back to page numbers manually. Verification passed `git diff --check`,
source scans proving the targeted queue-drain `dxb_storage_bytes2pgno()` and
`dxb_storage_npages2bytes()` conversions are gone, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.115` batch,
`1.171` crud, `1.120` iterate, `0.957` get, and `1.081` delete.

A later killed-page descriptor cleanup added `page_kill_writev()` so frozen
kill-page auxiliary writev batches build and advance from checked
`dxb_page_io_t` descriptors. Non-frozen `page_kill()` writes now use the checked
kill-span descriptor's byte length for payload poisoning before writing through
the same descriptor, while frozen writes use a one-page descriptor for auxiliary
buffer sizing and chunk descriptors for writev submission/next-page advancement.
Verification passed `git diff --check`, source scans proving the targeted
`page_kill()` raw `dxb_storage_npages2bytes(storage, npages)`,
`dxb_storage_pagesize(storage)` iov sizing, and `iov_pgno +=` chunk advancement
are gone, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, `cmake --build
@cmake-ninja-build`, the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.144` batch, `1.173` crud, `1.020` iterate, `1.009`
get, and `1.080` delete.

A later meta-triplet descriptor cleanup added `dxb_storage_meta_pages_io()` so
shadow metadata allocation/refresh, copy/export meta-buffer sizing,
new-database meta-page writes, and open-time minimum allocated-size checks all
derive the three-meta-page span from a checked `dxb_page_io_t` descriptor. This
removes the remaining direct `dxb_storage_npages2bytes(storage, NUM_METAS)`
callers and avoids using a raw page-count-to-byte conversion at those storage
boundaries. Verification passed `git diff --check`, source scans proving the
targeted `dxb_storage_npages2bytes(storage, NUM_METAS)` conversions and direct
`dxb_storage_page_prefix_io(storage, NUM_METAS, ...)` call sites outside the
helper are gone, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.132` batch, `1.167` crud, `0.995` iterate, `0.999`
get, and `1.075` delete.

A later coverage descriptor cleanup added `dxb_storage_coverage_io_from_page()`
so the coherency head-fetch path derives the required file coverage descriptor
from a checked page span at the storage-helper layer. `coherency_fetch_head()`
now asks storage to build the byte coverage for
`txn->geo.first_unallocated` instead of hand-copying the page descriptor and
calling `dxb_storage_byte_io_from_page()` locally before the current-size
refresh check. Verification passed `git diff --check`, a source scan proving
the targeted `required.pages = required_pages` and
`dxb_storage_byte_io_from_page(&required.pages, ...)` manual assembly is gone,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.118` batch, `1.160` crud, `0.774` iterate, `1.011`
get, and `1.085` delete.

A later page-prefix descriptor cleanup added
`dxb_storage_coverage_io_from_page_prefix()` and
`dxb_storage_sync_io_from_page_prefix()` so storage-bound coverage/sync callers
can request a checked `0..end_pgno` descriptor directly. `coherency_fetch_head()`,
`dxb_sync_locked()`, and the `env_sync()` pre-sync path no longer build a
temporary prefix page descriptor and then wrap it locally. Verification passed
`git diff --check`, a source scan proving the targeted prefix-plus-wrap sites
now use the prefix constructors, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.104` batch, `1.157`
crud, `0.811` iterate, `1.091` get, and `1.066` delete.

A later auxiliary meta-buffer descriptor cleanup moved `env_page_auxbuffer()`
onto checked storage page descriptors. New-database setup now initializes the
storage page size before allocating the auxiliary meta triplet, and
`env_page_auxbuffer()` derives its allocation size, first-two-page poison span,
and final zeroed page span from `dxb_page_io_t` descriptors instead of raw
`env->ps * NUM_METAS` arithmetic. Verification passed `git diff --check`,
source scans proving the targeted raw `env_page_auxbuffer()` sizing is gone,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.124` batch, `1.162` crud, `1.011` iterate, `1.015`
get, and `1.088` delete.

A later readahead descriptor cleanup added `dxb_storage_readahead_window_io()`
so `dxb_set_readahead()` asks storage for the complete page-aware readahead
request derived from `(prev_edge, edge, force_whole)`. The byte-window helper
still owns file-size clamping, but the readahead toggle path no longer keeps a
local byte descriptor and then derives the readahead descriptor itself.
Verification passed `git diff --check`, source scans proving the local
`window_bytes` construction is gone from `dxb_set_readahead()`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.114` batch,
`1.163` crud, `0.978` iterate, `0.990` get, and `1.077` delete.

A later cache-invalidation descriptor cleanup added
`dxb_storage_invalidate_cached_bytes_io()` for byte-range invalidation after
truncate/shrink and `dxb_storage_invalidate_copied_io()` for same-file copy
destinations. `dxb_storage_set_filesize_io()` now invalidates the stale tail
through the shrink byte descriptor, and `dxb_storage_copy_pages()` invalidates
from the copy descriptor instead of fabricating a `dxb_write_io_t` solely for
cache eviction. Verification passed `git diff --check`, source scans proving
the stale-tail local `dxb_storage_page_io_from_bytes()` conversion and the copy
path's fake write descriptor are gone, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.118` batch, `1.148`
crud, `0.861` iterate, `1.060` get, and `1.069` delete.

A later cached-read descriptor cleanup added `dxb_storage_read_page_span()` so
explicit page-cache miss fills and large-overflow materialization submit reads
through one storage helper that accepts the checked `dxb_page_io_t` span and
derives the async-facing read descriptor internally. `dxb_storage_read_cached_page()`
and `dxb_storage_materialize_cached_large_page()` no longer each keep a local
`dxb_read_io_t` conversion before calling the read submission helper.
Verification passed `git diff --check`, source scans proving the cache-read
paths now use `dxb_storage_read_page_span()`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.139` batch,
`1.155` crud, `0.858` iterate, `1.051` get, and `1.074` delete.

A later page-read submission cleanup reused `dxb_storage_read_page_span()` for
meta-shadow refresh and defrag fallback reads. These callers already build
checked `dxb_page_io_t` spans, and now hand those spans directly to the storage
read helper instead of each deriving a local `dxb_read_io_t` before submission.
This keeps the page-numbered read boundary consistent across cache fills,
overflow materialization, meta refresh, and defrag copy fallback reads.
Verification passed `git diff --check`, source scans proving there are no
remaining local `dxb_read_io_t request` page-read conversions, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.116` batch,
`1.183` crud, `0.974` iterate, `1.053` get, and `1.081` delete.

A later page-write submission cleanup added `dxb_storage_write_page_span()` for
callers that already hold a checked `dxb_page_io_t` and immediately submit a
write. Defrag page moves, initial meta-triplet creation, explicit meta-page
override, and non-frozen debug page-kill writes now hand their page descriptors
to storage directly instead of deriving throwaway local `dxb_write_io_t`
objects. Dirty-page queue setup/enqueue and writev page-kill paths still keep
their write descriptors because queue sizing, coalescing, and scatter/gather
submission consume them directly. Verification passed `git diff --check`,
source scans proving the simple immediate writes now use
`dxb_storage_write_page_span()` while queue/writev descriptor uses remain, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.144` batch,
`1.168` crud, `1.234` iterate, `0.973` get, and `1.082` delete.

A later direct page-span I/O cleanup collapsed the legacy read/write page
wrapper layer underneath those helpers. `dxb_storage_read_page_span()` now
validates its checked `dxb_page_io_t`, derives the byte descriptor itself, and
submits the read; the single-use `dxb_read_io_t`, read-page adapter, and
read-page validator are gone. Direct page writes and debug page-kill writev
now follow the same pattern through `dxb_storage_write_page_span()` and
`dxb_storage_writev_page_span()`, deriving byte descriptors locally and
invalidating cached data pages from the original page-span descriptor. The
remaining `dxb_write_io_t` users are intentionally limited to dirty-write queue
setup/enqueue and the queue validator, where the queued completion path still
needs both byte and page views. Verification passed `git diff --check`, source
scans proving the removed read/write wrappers are gone and the remaining
`dxb_write_io_t` uses are queue-local, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.113` batch, `1.158`
crud, `1.007` iterate, `0.943` get, and `1.063` delete.

A later queued-write page-span cleanup removed the last `dxb_write_io_t` layer
from the C source. Dirty-write queue sizing and enqueue now use
`dxb_storage_prepare_write_queue_page_span()` and
`dxb_storage_add_queued_page_span()`, which validate the caller's checked
`dxb_page_io_t`, derive a `dxb_byte_io_t`, and then hand that byte descriptor
to the existing OSAL queue. The queue storage and completion walk still use
byte descriptors, with completion converting back to exact page spans for
shadow-buffer release checks. Verification passed `git diff --check`, source
scans proving `dxb_write_io_t` and the old write-IO adapter/queue helpers are
gone, the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the
six focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, the ASAN build, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.144` batch, `1.156` crud, `1.189` iterate, `0.996`
get, and `1.075` delete.

A later same-file page-copy cleanup removed the `dxb_copy_io_t` wrapper from
the C source. Defrag overflow-tail copies now pass source and destination
`dxb_page_io_t` spans directly to `dxb_storage_copy_page_span()`, which
validates both page spans, derives byte descriptors at the storage boundary,
submits the existing `copy_file_range()` path, and invalidates cached pages
from the destination page span. Verification passed `git diff --check`, source
scans proving `dxb_copy_io_t`, the copy wrapper constructor/validator, and the
old `dxb_storage_copy_pages()` helper are gone, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest
suite, forced tiny-cache fault injection, `cmake --build @cmake-asan-build`,
the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.120` batch, `1.179` crud, `1.195` iterate, `0.967`
get, and `1.071` delete.

A later readahead cleanup removed the `dxb_readahead_io_t` wrapper from the C
source. `dxb_set_readahead()` now computes the storage advice window as a
checked `dxb_byte_io_t`, derives a page span only for logging, and submits
normal/random advice directly through `dxb_storage_advise_range()`. Toggle-time
prefetch now uses `dxb_storage_prefetch_readahead_bytes()`, which derives its
page span internally before issuing the existing will-need advice. Verification
passed `git diff --check`, source scans proving the old readahead wrapper,
constructor/validator, and advice/prefetch wrapper helpers are gone, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest
suite, forced tiny-cache fault injection, `cmake --build @cmake-asan-build`,
the six focused ASAN `migration_smoke` CTest entries, and a rerun of
`mdbx_migration_bench_lazy`. The first benchmark attempt missed the iterate
threshold by noise at `0.695` versus `0.700`; the rerun passed with
forced/default ratios of `0.959` batch, `0.992` crud, `0.979` iterate, `1.044`
get, and `1.002` delete.

A later discard cleanup removed the `dxb_discard_io_t` wrapper from the C
source. Resize/open/shrink advisory discard paths now submit their checked
`dxb_byte_io_t` ranges directly to `dxb_storage_discard_range()`, which validates
the discard mode, derives the page coverage internally, and invalidates cached
pages only for destructive remove-style discard modes. This keeps byte-range
advice at the call sites while preserving the storage-boundary page span needed
by the explicit page cache.

A later data-sync cleanup removed the `dxb_sync_io_t` wrapper from the C source.
Commit sync and pre-sync paths now build checked page-prefix descriptors and
submit them directly to `dxb_storage_sync_page_span()`, which validates the page
span and derives the corresponding byte range at the storage boundary before
issuing the existing fd sync. `dxb_note_fsync_pgop()` still observes the selected
sync mode after successful range construction and before sync submission.
Verification passed `git diff --check`, source scans proving the sync wrapper,
constructors, validator, and old range helper are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.106` batch, `1.166` crud, `1.027` iterate, `0.983`
get, and `1.075` delete.

A later coverage cleanup removed the `dxb_coverage_io_t` wrapper from the C
source. The coherency root probe now checks and reads its byte subrange directly,
while snapshot head refresh builds a checked page-prefix descriptor and passes
it to `dxb_storage_fetch_filesize_for_page_span_if_needed()`, which validates the
page span and derives the byte coverage internally before deciding whether a
fresh on-disk filesize read is needed. Verification passed `git diff --check`,
source scans proving the coverage wrapper, constructors, validator, coverage
contains helper, and old filesize-refresh helper are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.121` batch, `1.161` crud, `0.803` iterate, `1.049`
get, and `1.072` delete.

A later outbound-copy cleanup removed the `dxb_outbound_io_t` wrapper from the C
source. `copy_asis()` now submits page-span subranges directly to
`dxb_storage_sendfile_page_span_to_fd()` and
`dxb_storage_copy_page_span_to_fd()`, which derive the byte subrange internally
before calling the existing `sendfile()` or `copy_file_range()` paths. The fd
copy helpers now carry destination fd/offset arguments directly, with progress
still reported through the existing `advanced` output. Verification passed
`git diff --check`, source scans proving the outbound wrapper, constructors,
validator, and old fd-copy helpers are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.095` batch, `1.154` crud, `0.798` iterate, `0.966`
get, and `1.084` delete.

A later meta-subrange cleanup removed the `dxb_meta_io_t` wrapper from the C
source. Meta commit writes and steady-signature wipes now pass meta number,
payload offset, and payload length directly to `dxb_storage_write_meta()`, which
derives the exact byte range at the storage boundary. Shadow-buffer updates now
derive the same byte ranges internally through `meta_shadow_copy_payload()` and
`meta_shadow_copy_bytes()`, keeping explicit metadata I/O without carrying a
separate wrapper object. Verification passed `git diff --check`, source scans
proving the meta wrapper, constructor, validator, and old wrapped call sites are
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.111` batch, `1.168` crud,
`0.954` iterate, `1.000` get, and `1.083` delete.

A later filesize-state cleanup removed the one-field `dxb_filesize_io_t`
wrapper from the C source. Filesize reads, file-size changes, storage-current
updates, checker accounting, initial database creation, setup, and resize now
pass raw byte counts directly to the storage helpers, while shrink-tail cache
invalidation derives its byte range at the state-mutation boundary. This keeps
the fault-injected setsize/filesize operations explicit without carrying a
descriptor that held no range or backend-specific submission shape.
Verification passed `git diff --check`, source scans proving the filesize
wrapper, constructor, validator, and old filesize-wrapper setters are gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.091` batch, `1.176` crud,
`1.045` iterate, `0.960` get, and `1.067` delete.

A later data-write invalidation cleanup moved successful data-channel cache
invalidation from page write helpers to the byte write submission boundary.
`dxb_storage_write_bytes()` and `dxb_storage_writev_bytes()` now validate the
written byte span into page coverage before submitting the syscall, and
invalidate overlapping non-reusable cache entries only after the write and
completion fault hook succeed. Page-span write helpers now only derive their
byte request and submit it, so direct byte writes and page-derived writes share
the same cache-coherency boundary. Verification passed `git diff --check`,
source scans proving the old page-local `dxb_storage_invalidate_written_pages()`
hook is gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.118` batch, `1.157` crud,
`0.994` iterate, `1.019` get, and `1.080` delete.

A later dirty-write queue byte-request cleanup removed the remaining
page-span wrappers around queue sizing and enqueue. `iov_init()` now builds the
queue capacity span as a checked page range and then derives the byte request
before calling `dxb_storage_prepare_write_queue_bytes()`. `iov_page()` now does
the same for each dirty page span before calling `dxb_storage_add_queued_bytes()`,
so the OSAL write queue receives only byte descriptors from its callers. The
completion walk still converts queued byte descriptors back to exact page spans
for shadow-buffer release validation. Verification passed `git diff --check`,
source scans proving the old page-span queue helpers and
`dxb_storage_write_bytes_from_page_span()` are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.091` batch, `1.170` crud, `1.181` iterate, `0.970`
get, and `1.086` delete.

A later queued-write cache invalidation cleanup made successful dirty-write
queue completion invalidate explicit page-cache entries at the queued byte range
boundary. `iov_callback4dirtypages()` now converts each queued byte descriptor
back to an exact page span, invalidates overlapping non-reusable cache entries
only when the queued write completed successfully and the channel writes the
data file, then releases the shadow dirty pages. Failed queued writes still skip
cache invalidation while releasing shadow buffers. Verification passed `git
diff --check`, source scans proving queued dirty-write completion has the new
data-channel invalidation boundary and no longer treats storage as const in the
callback, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.105` batch, `1.159` crud,
`1.100` iterate, `0.915` get, and `1.071` delete.

A later outbound-copy fast-path cleanup removed the page-span wrappers around
environment-copy `sendfile()` and `copy_file_range()` submissions. `copy_asis()`
now derives the remaining source byte descriptor once at the page-cache/storage
boundary, passes that descriptor directly to `dxb_storage_sendfile_bytes_to_fd()`
or `dxb_storage_copy_bytes_to_fd()`, and derives the portable fallback read
subrange from the same byte descriptor. Verification passed `git diff --check`,
source scans proving the old `dxb_storage_sendfile_page_span_to_fd()` and
`dxb_storage_copy_page_span_to_fd()` helpers are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.120` batch,
`1.156` crud, `1.103` iterate, `1.149` get, and `1.066` delete.

A later defrag copy cleanup removed the same-file
`dxb_storage_copy_page_span()` wrapper from the C source. `defrag_move()` now
builds checked source and destination page spans, derives exact byte descriptors
for the remaining moved run at the defrag/storage boundary, calls
`dxb_storage_copy_bytes()` directly, and invalidates the destination cache span
after a successful copy. Verification passed `git diff --check`, source scans
proving `dxb_storage_copy_page_span()` is gone from `mdbx.c` and the defrag
copy path now submits byte descriptors directly, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.098` batch,
`1.158` crud, `0.983` iterate, `0.995` get, and `1.073` delete.

A later sync submission cleanup removed the `dxb_storage_sync_page_span()`
wrapper from the C source. Data-sync callers still build checked page-prefix
coverage from transaction geometry, but now derive `dxb_byte_io_t` explicitly at
the sync boundary and submit that descriptor through `dxb_storage_sync_bytes()`;
the helper validates the byte request before preserving the existing whole-file
fsync behavior. Verification passed `git diff --check`, source scans proving
the old page-span sync helper is gone from `mdbx.c` and sync coverage callers
derive byte descriptors explicitly, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.129` batch,
`1.160` crud, `0.943` iterate, `0.980` get, and `1.093` delete.

A later coherency filesize-coverage cleanup removed
`dxb_storage_fetch_filesize_for_page_span_if_needed()` from the C source.
`coherency_fetch_head()` now builds the checked `0..first_unallocated`
page-prefix span, derives the byte coverage explicitly, compares that byte span
with the cached current size, and calls
`dxb_storage_fetch_filesize_for_bytes_if_needed()` only when a filesize refresh
is still required. Verification passed `git diff --check`, source scans proving
the old page-span filesize helper is gone from `mdbx.c` and the coherency path
now submits byte coverage directly, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs,
`cmake --build @cmake-ninja-build`, the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.094` batch,
`1.168` crud, `1.148` iterate, `0.966` get, and `1.096` delete.

A later meta-byte descriptor cleanup added explicit full-meta byte request
helpers and moved meta-shadow refresh, copy buffer sizing, new-environment
meta-page writes, meta override writes, and meta payload offset derivation away
from synthetic page-span storage submissions. Page descriptors still remain
where page identity matters, such as page-cache reads, defrag movement, and
debug page-kill writes. Verification passed `git diff --check`, source scans
proving meta-shadow refresh no longer calls `dxb_storage_read_page_span()` and
full meta-page writes no longer call `dxb_storage_write_page_span()`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.126` batch, `1.162` crud, `1.064` iterate, `1.048`
get, and `1.088` delete.

A later data-write wrapper cleanup removed the
`dxb_storage_write_page_span()` and `dxb_storage_writev_page_span()` helpers
from the C source. Defrag single-page fallback reads and writes now derive
explicit byte descriptors at the defrag/storage boundary, and debug page-kill
writes submit byte descriptors directly to `dxb_storage_write_bytes()` or
`dxb_storage_writev_bytes()`. Verification passed `git diff --check`, source
scans proving the page-span write helpers and the unused
`dxb_storage_meta_pages_io()` helper are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.134` batch, `1.170` crud, `0.978` iterate, `0.987`
get, and `1.060` delete.

A later page-cache read wrapper cleanup removed
`dxb_storage_read_page_span()` from the C source. The page cache still keys
cached entries by checked `dxb_page_io_t` descriptors, but single-page cache
fills and large-overflow materialization now derive exact `dxb_byte_io_t`
coverage at the cache/storage boundary before calling
`dxb_storage_read_bytes()`. Verification passed `git diff --check`, source
scans proving the read/write page-span wrappers and the unused
`dxb_storage_meta_pages_io()` helper are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.100` batch, `1.183` crud, `0.915` iterate, `1.008`
get, and `1.075` delete.

A later page-subrange adapter cleanup removed
`dxb_storage_page_subrange_bytes_io()` from the C source. Page-ref value
materialization, non-compacting copy fallbacks, and coherency root-txnid probes
now derive checked page coverage first, convert that coverage to a
`dxb_byte_io_t`, and then derive local subranges with
`dxb_storage_byte_subrange_io()`. Verification passed `git diff --check`,
source scans proving the page-subrange adapter, read/write page-span wrappers,
and the unused `dxb_storage_meta_pages_io()` helper are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.124` batch, `1.142` crud, `1.175` iterate, `0.964`
get, and `1.062` delete.

A later page-ref byte adapter cleanup removed
`dxb_storage_page_ref_bytes_io()` from the C source. Public-cache value-range
reconstruction and cache-entry materialization now ask the page ref for a
checked `dxb_page_io_t`, convert that page descriptor to a `dxb_byte_io_t`, and
derive the value subrange with `dxb_storage_byte_subrange_io()` at the cache
boundary. Verification passed `git diff --check`, source scans proving the
page-ref byte adapter, page-subrange adapter, read/write page-span wrappers,
and the unused `dxb_storage_meta_pages_io()` helper are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.115` batch, `1.165` crud, `1.056` iterate, `1.020`
get, and `1.087` delete.

A later meta-triplet byte adapter cleanup removed
`dxb_storage_meta_pages_bytes_io()` from the C source. Meta-shadow allocation
and refresh, environment-copy buffer sizing, new-database meta initialization,
post-setup meta coverage checks, and aux-page buffer allocation now build
checked `dxb_page_io_t` coverage for the first `NUM_METAS` pages before
converting that coverage to a `dxb_byte_io_t`. Verification passed
`git diff --check`, source scans proving the meta-triplet byte adapter,
page-ref byte adapter, page-subrange adapter, read/write page-span wrappers,
and the unused `dxb_storage_meta_pages_io()` helper are gone from `mdbx.c`,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the
six focused `migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.135` batch, `1.172` crud, `0.865` iterate, `0.960`
get, and `1.077` delete.

A later single-meta-page byte adapter cleanup removed
`dxb_storage_meta_page_bytes_io()`,
`dxb_storage_meta_payload_bytes_io()`, and the raw
`dxb_storage_meta_page_offset()` helper from the C source. Meta-shadow page
lookup, shadow page/payload copies, explicit meta writes, aux-buffer zero-page
placement, and explicit meta override now derive a checked one-page
`dxb_page_io_t` first, convert it to `dxb_byte_io_t`, and take payload
subranges with `dxb_storage_byte_subrange_io()` where needed. Verification
passed `git diff --check`, source scans proving the single-meta-page,
meta-payload, meta-triplet, page-ref byte, page-subrange, and read/write
page-span adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.154` batch, `1.170` crud, `0.731` iterate, `0.841`
get, and `1.089` delete.

A later data-range adapter cleanup removed
`dxb_storage_filesize_shrink_tail_io()`,
`dxb_storage_readahead_window_bytes_io()`, and
`dxb_storage_invalidate_cached_bytes_io()` from the C source. File-size shrink
now derives the stale tail byte range and its cache-invalidation page coverage
inside `dxb_storage_set_filesize_bytes()`, while readahead toggling derives its
clamped byte window and page coverage directly in `dxb_set_readahead()`. This
keeps explicit range construction at the storage operation boundary instead of
hiding it behind one-call adapters. Verification passed `git diff --check`,
source scans proving the shrink-tail, readahead-window, byte-invalidation,
single-meta-page, meta-payload, meta-triplet, page-ref byte, page-subrange, and
read/write page-span adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.113` batch, `1.168` crud, `0.940` iterate, `0.971`
get, and `1.086` delete.

A later startup meta-probe adapter cleanup removed
`dxb_storage_meta_probe_io()` from the C source. Startup header reads still use
byte-addressed probes while the database page size is unknown, but
`dxb_read_header()` now derives the checked `probe_pagesize * meta_number`
byte offset and `MDBX_MIN_PAGESIZE` request locally before submitting
`dxb_storage_read_bytes()`. Verification passed `git diff --check`, source
scans proving the startup meta-probe, shrink-tail, readahead-window,
byte-invalidation, single-meta-page, meta-payload, meta-triplet, page-ref byte,
page-subrange, and read/write page-span adapters are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite, forced tiny-cache fault injection, the ASAN build
(`cmake --build @cmake-asan-build`), the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.111` batch, `1.161` crud, `1.079`
iterate, `0.953` get, and `1.059` delete.

A later byte-to-page adapter cleanup removed
`dxb_storage_byte_start_page_io()` and
`dxb_storage_exact_page_io_from_bytes()` from the C source. Public-cache value
materialization now derives the starting page number, one-page read request,
and byte offset inside `cache_materialize_entry()`, while write-queue
callbacks derive the queued page coverage locally and require the queued byte
range to match full page boundaries before cache invalidation and page-shadow
release. Verification passed `git diff --check`, source scans proving the
byte-start and exact-page byte adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with
forced/default ratios of `1.109` batch, `1.160` crud, `1.018` iterate, `0.989`
get, and `1.078` delete.

A later file-size coverage cleanup removed `dxb_storage_contains_range()` and
`dxb_storage_fetch_filesize_for_bytes_if_needed()` from the C source. Root
coherency probes now check the derived `page_t.txnid` byte request against the
current storage size at the probe site, and head refresh now checks the
required used-page byte range before fetching a fresh file size directly. This
keeps explicit byte coverage checks beside the read paths they protect.
Verification passed `git diff --check`, source scans proving the range
containment and fetch-if-needed helpers are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, `cmake --build @cmake-ninja-build`, the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`.
The paired benchmark gate passed with forced/default ratios of `1.085` batch,
`1.153` crud, `0.997` iterate, `1.027` get, and `1.079` delete.

A later cached-page pointer cleanup removed the unused
`dxb_storage_cached_page_contains()` helper from the C source. The active
public `mdbx_is_dirty()` pointer resolution still uses explicit page-cache and
dirty-list spans, but the dead helper that scanned cached page memory by raw
address is gone. Verification passed `git diff --check`, source scans proving
the cached-page containment helper and the earlier removed storage adapters are
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.137` batch, `1.175`
crud, `1.231` iterate, `0.945` get, and `1.087` delete.

A later sync descriptor cleanup restored an explicit `dxb_sync_io_t` request
for data sync. Commit-time data sync and writer-free pre-sync still derive
checked `dxb_page_io_t` prefixes from transaction geometry, but now build a
`dxb_sync_io_t` carrying the page coverage, derived byte coverage, and sync mode
before calling `dxb_storage_sync_io()`. The current backend still performs the
same whole-file `fsync()` underneath, but the sync boundary again carries a
validated descriptor that a future range-aware or async completion backend can
consume. Verification passed `git diff --check`, source scans proving
`dxb_storage_sync_bytes()` and the earlier removed storage adapters are gone
from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.130` batch, `1.154`
crud, `1.204` iterate, `0.959` get, and `1.079` delete.

A later data-write descriptor cleanup removed the ambiguous
`dxb_storage_page_io_from_bytes()` helper from the C source. Advisory,
discard, readahead, and stale-tail invalidation paths now call the explicitly
rounded `dxb_storage_page_coverage_io_from_bytes()`, while data-channel writes
and write-queue callbacks build a `dxb_data_write_io_t` that must round-trip
the submitted byte span to an exact full-page `dxb_page_io_t` before cache
invalidation. This leaves future async write submission with a checked
data-write descriptor instead of a generic byte-to-page adapter. Verification
passed `git diff --check`, source scans proving
`dxb_storage_page_io_from_bytes()` and the earlier removed storage adapters are
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.141` batch, `1.199`
crud, `1.044` iterate, `0.948` get, and `1.070` delete.

A later cached-read descriptor cleanup added `dxb_data_read_io_t` for
page-cache data-file reads. Single-page cache fills and large-overflow
materialization still keep their checked `dxb_page_io_t` cache keys, but now
build a `dxb_data_read_io_t` that carries both the page coverage and exact byte
request before calling `dxb_storage_read_bytes()`. General byte reads for meta
refresh, warmup, and copy fallback remain byte-oriented; the new descriptor is
limited to population of pinned data pages that future async read submission
will own. Verification passed `git diff --check`, source scans proving
`dxb_storage_read_page_span()`, `dxb_storage_page_io_from_bytes()`, and the
earlier removed storage adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.111` batch, `1.158` crud, `1.021` iterate,
`1.023` get, and `1.084` delete.

A later public-cache value descriptor cleanup added `cache_value_io_t` so
cache refresh and cache-hit materialization keep a returned `MDBX_val`'s
backing page span, byte span, and page offset together. Refresh still stores
only the public offset/length pair into `MDBX_cache_entry_t`, but it now derives
that pair from a descriptor tied to a retained cursor page reference. Cache-hit
materialization now validates the returned pointer through the same
page-ref-backed descriptor before retaining the page and returning the value to
the caller. Verification passed `git diff --check`, source scans proving
`dxb_storage_read_page_span()`, `dxb_storage_page_io_from_bytes()`, and the
earlier removed storage adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.098` batch, `1.163` crud, `1.163` iterate,
`0.948` get, and `1.068` delete.

A later page-coverage descriptor cleanup removed the loose
`dxb_storage_page_coverage_io_from_bytes()` helper from the C source.
Readahead prefetch, discard, shrink-tail invalidation, and readahead logging
now build a `dxb_page_coverage_io_t` carrying the original byte request,
rounded page coverage, and full page-byte coverage. Advisory and discard calls
keep using the original byte request, while cache invalidation and will-need
prefetch use the rounded page span/page bytes. Verification passed
`git diff --check`, source scans proving the old page-coverage helper and the
earlier removed storage adapters are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.131` batch, `1.150` crud, `0.975`
iterate, `1.004` get, and `1.071` delete.

A later dirty-write queue descriptor cleanup made queue preparation and enqueue
consume checked `dxb_data_write_io_t` descriptors instead of loose byte
requests. Page-derived dirty-write spans now build a data-write descriptor from
the checked `dxb_page_io_t`; the storage queue helpers validate that page/byte
pair before sizing the queue or inserting an item, and only the OSAL queue keeps
the internal byte descriptor it needs for coalesced subrange walking. This keeps
future async dirty-write submission tied to exact full-page data-write
descriptors at the storage boundary. Verification passed `git diff --check`,
source scans proving `dxb_storage_prepare_write_queue_bytes()` and
`dxb_storage_add_queued_bytes()` are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.121` batch, `1.151` crud, `1.021`
iterate, `0.990` get, and `1.066` delete.

A later direct data-write descriptor cleanup split single-buffer data-file
writes from metadata byte writes. Defrag page moves, initial meta-triplet
creation, and page-kill poison writes now build checked `dxb_data_write_io_t`
descriptors from their page spans before submitting to `dxb_storage_write_data()`
or `dxb_storage_writev_data()`. Metadata subrange writes keep the byte-oriented
`dxb_storage_write_meta_bytes()` path, and source scans now prove no direct
data-channel call remains through a generic byte writer. Verification passed
`git diff --check`, source scans proving `dxb_storage_writev_bytes()` and
data-channel `dxb_storage_write_bytes()` calls are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.153` batch, `1.163` crud, `0.800`
iterate, `0.992` get, and `1.079` delete.

A later direct data-read descriptor cleanup added `dxb_storage_read_data()` for
full-page data-file reads. Page-cache single-page fills, large-overflow
materialization, and defrag source-page reads now submit checked
`dxb_data_read_io_t` descriptors; startup metadata/header probes and portable
environment-copy chunk reads remain byte-oriented. Verification passed
`git diff --check`, source scans proving data-read descriptor submissions no
longer call `dxb_storage_read_bytes()` directly in `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.123` batch, `1.162` crud, `0.841`
iterate, `1.067` get, and `1.081` delete.

A later meta-shadow read descriptor cleanup moved full meta-shadow refresh onto
the data-read path. `meta_shadow_refresh()` still allocates the explicit meta
buffer from the first `NUM_METAS` pages, but now builds a checked
`dxb_data_read_io_t` from that page span and submits it through
`dxb_storage_read_data()`. Startup meta/header probes continue using byte
descriptors because they run before the stored page size is established, and
portable env-copy/probe reads remain byte-oriented. Verification passed
`git diff --check`, source scans proving the old meta-shadow byte read is gone
from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.095` batch, `1.168`
crud, `0.796` iterate, `1.114` get, and `1.075` delete.

A later coherency-root read descriptor cleanup moved the remaining root-page
`mod_txnid` probe off field-sized byte reads. `coherency_probe_root_txnid()`
now builds a checked one-page `dxb_data_read_io_t`, reads the full root page
through `dxb_storage_read_data()` into a temporary aligned page buffer, and then
extracts the page `txnid` for validation. Startup meta/header probes and
portable env-copy/probe reads remain byte-oriented. Verification passed
`git diff --check`, source scans proving the old coherency root byte read is
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.126` batch, `1.168`
crud, `1.231` iterate, `1.028` get, and `1.073` delete.

A later warmup read descriptor cleanup moved forced warmup scans onto the
data-read path. `mdbx_env_warmup()` now derives the selected warmup window as a
rounded page-prefix `dxb_page_io_t`, and `warmup_force_read()` validates and
clamps that page span against the current storage size before submitting
full-page chunks through checked `dxb_data_read_io_t` descriptors. Startup
meta/header probes and portable env-copy/probe reads remain byte-oriented.
Verification passed `git diff --check`, source scans proving the old
byte-oriented warmup range and scan requests are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.131` batch, `1.164` crud, `0.830` iterate,
`0.974` get, and `1.068` delete.

A later environment-copy fallback descriptor cleanup moved the portable
non-compacting copy read path onto full-page data-read descriptors. `copy_asis()`
still derives the authoritative remaining source range as a checked byte
descriptor for `sendfile()` and `copy_file_range()`, but the portable fallback
now derives page coverage from each output chunk, submits that coverage through
`dxb_storage_read_data()`, and writes only the requested payload subrange from
the aligned buffer. Startup meta/header probes remain byte-oriented because
they run before the stored page size is established. Verification passed
`git diff --check`, source scans proving the old portable env-copy byte read is
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.146` batch, `1.150`
crud, `1.251` iterate, `0.972` get, and `1.067` delete.

A later metadata write descriptor cleanup added `dxb_meta_write_io_t` for meta
page writes. Meta commit, undo, steady-wipe, and full-page override paths now
build checked metadata write descriptors before calling
`dxb_storage_write_meta()`, so the storage boundary receives the meta page span
and exact byte request together instead of loose `(number, payload offset,
bytes)` arguments. Full meta-page overrides use a full-page descriptor, while
normal commit and steady-wipe updates use payload subrange descriptors.
Verification passed `git diff --check`, source scans proving the old loose
`dxb_storage_write_meta()` call shape is gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.089` batch, `1.158` crud, `1.008` iterate,
`1.000` get, and `1.069` delete.

A later startup metadata read descriptor cleanup added `dxb_meta_read_io_t` for
the initial header probe loop. `dxb_read_header()` now builds checked meta-read
descriptors from the meta number and probed page size before calling
`dxb_storage_read_meta()`, preserving the double-read/retry behavior while
removing the last direct startup `dxb_storage_read_bytes()` submissions from
the header path. These descriptors intentionally record the guessed page size
instead of using the normal page-span helper, because the stored page size has
not been validated yet during this phase. Verification passed
`git diff --check`, source scans proving the startup meta probe no longer calls
`dxb_storage_read_bytes()` directly, the GNUmake `mdbx_migration_smoke` target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite, forced tiny-cache
fault injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.148` batch, `1.155`
crud, `0.888` iterate, `1.013` get, and `1.065` delete.

A later metadata shadow descriptor cleanup replaced the loose shadow-copy
helpers with `meta_shadow_copy_write()`, which validates and consumes the same
`dxb_meta_write_io_t` used for the corresponding disk write. Meta commit,
steady-wipe, and full-page override paths now update `env->meta_shadow` from
the already-built write descriptor instead of reconstructing meta number,
payload offset, and byte count locally. This keeps the explicit in-memory meta
buffer synchronized through the same checked request shape as metadata I/O.
Verification passed `git diff --check`, source scans proving the old
`meta_shadow_copy_page()`, `meta_shadow_copy_payload()`, and
`meta_shadow_copy_bytes()` helpers are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.100` batch, `1.167` crud, `0.819` iterate,
`1.041` get, and `1.068` delete.

A later typed write submission cleanup removed the private
`dxb_storage_write_bytes_to_channel()` and `dxb_storage_write_meta_bytes()`
shims. `dxb_storage_write_data()` and `dxb_storage_write_meta()` now validate
their `dxb_data_write_io_t`/`dxb_meta_write_io_t` descriptors, preserve the
same `write` and `write-complete` fault-injection hooks, and submit directly
to `dxb_storage_pwrite()` on the data or metadata channel. Data writes still
invalidate the page cache from the validated page span, while metadata writes
remain descriptor-only at the storage boundary. Verification passed
`git diff --check`, source scans proving the removed byte-channel and meta-byte
write helpers are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.163` batch, `1.164` crud, `1.021` iterate, `0.998`
get, and `1.078` delete.

A later typed read submission cleanup removed the private
`dxb_storage_read_bytes()` shim. `dxb_storage_read_data()` and
`dxb_storage_read_meta()` now validate their `dxb_data_read_io_t` and
`dxb_meta_read_io_t` descriptors, preserve the same `read` and `read-complete`
fault-injection hooks, and submit directly to `dxb_storage_pread()`. This keeps
data and startup metadata reads descriptor-typed all the way to the primitive
storage operation instead of dropping through an intermediate untyped byte
request wrapper. Verification passed `git diff --check`, source scans proving
the removed byte-read helper is gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke` CTest
entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate passed
with forced/default ratios of `1.115` batch, `1.165` crud, `1.239` iterate,
`0.973` get, and `1.075` delete.

A later metadata sync descriptor cleanup added
`dxb_storage_make_meta_sync_io()` for sync requests over the three metadata
pages. Metadata commit sync, steady-meta wipe sync, `meta_sync()`, and
full-page `meta_override()` now build checked `dxb_sync_io_t` descriptors
before calling `dxb_storage_sync_io()`, leaving the raw
`dxb_storage_sync()` primitive behind that descriptor boundary. Verification
passed `git diff --check`, source scans proving direct metadata
`dxb_storage_sync()` submissions are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.110` batch, `1.161` crud, `0.836`
iterate, `1.077` get, and `1.059` delete.

A later sync submission cleanup removed the raw mode-only
`dxb_storage_sync()` helper from the C source. `dxb_storage_sync_io()` now
validates the caller's checked `dxb_sync_io_t` descriptor itself, preserves the
same sync fault-injection hooks, and then submits the existing whole-file
`fsync()`/`fdatasync()` backend using the descriptor's mode bits. This leaves
data and metadata sync callers behind one descriptor-validated storage boundary
instead of dropping through a private untyped sync primitive. Verification
passed `git diff --check`, source scans proving `dxb_storage_sync()` is gone
from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.132` batch, `1.155`
crud, `1.249` iterate, `0.974` get, and `1.067` delete.

A later same-file copy descriptor cleanup added `dxb_data_copy_io_t` for
defrag overflow-tail copies. The defrag copy path now builds checked source and
destination page spans, constructs one data-copy descriptor carrying both
page and byte coverage, and submits it through `dxb_storage_copy_data()`.
Successful `copy_file_range()` completion now invalidates the destination cache
span inside the typed storage operation instead of at the call site, and the
old raw `dxb_storage_copy_bytes()` wrapper is gone. Verification passed `git
diff --check`, source scans proving `dxb_storage_copy_bytes()` is gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.136` batch, `1.179`
crud, `1.181` iterate, `0.904` get, and `1.069` delete.

A later outbound-copy submission cleanup removed the private raw
`dxb_storage_copy_file_range_to_fd()` and
`dxb_storage_sendfile_to_fd_raw()` wrappers from the C source. The existing
environment-copy helpers still accept the checked `dxb_byte_io_t` source range
from `copy_asis()`, but now validate descriptor coverage and submit
`copy_file_range()` or `sendfile()` directly at that storage boundary. This
keeps the current byte-range export shape while removing another untyped
intermediate syscall wrapper. Verification passed `git diff --check`, source
scans proving the removed outbound wrappers are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.152` batch, `1.158` crud, `0.979`
iterate, `1.023` get, and `1.083` delete.

A later readahead prefetch coverage cleanup removed the private
`dxb_storage_prefetch_readahead_bytes()` wrapper from the C source.
`dxb_set_readahead()` already builds checked page coverage for the clamped
toggle-time window, so the `WILLNEED` advice path now reuses
`window_coverage.page_bytes` directly instead of rebuilding the same coverage
inside another helper. This keeps the toggle-time prefetch advice behind the
same validated window as the surrounding readahead logging and leaves no
references to `dxb_storage_prefetch_readahead_bytes()` in `mdbx.c`.
Verification passed `git diff --check`, source scans proving the removed
prefetch helper is gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.134` batch, `1.154` crud, `0.996` iterate, `1.038`
get, and `1.074` delete.

A later typed submission cleanup removed the private
`dxb_storage_pread()`, `dxb_storage_pwrite()`, and `dxb_storage_pwritev()`
wrappers from the C source. `dxb_storage_read_data()`,
`dxb_storage_read_meta()`, `dxb_storage_write_data()`,
`dxb_storage_write_meta()`, and `dxb_storage_writev_data()` still validate
their checked read/write descriptors and preserve the same fault-injection
hooks, but now submit directly to the corresponding `osal_pread()`,
`osal_pwrite()`, or `osal_pwritev()` call at the typed storage boundary. This
removes another redundant byte-level validation hop after descriptor validation
and leaves no references to the removed primitive wrappers in `mdbx.c`.
Verification passed `git diff --check`, source scans proving the removed
primitive wrappers are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.104` batch, `1.176` crud, `0.968` iterate, `0.988`
get, and `1.072` delete.

A later sync/size/filesize submission cleanup removed the private
`dxb_storage_fsync()`, `dxb_storage_fsetsize()`, and
`dxb_storage_read_filesize_from_disk()` wrappers from the C source.
`dxb_storage_sync_io()` still validates the checked sync descriptor and
preserves sync fault injection before submitting directly to `osal_fsync()`.
The filesize grow/shrink and refresh paths likewise keep their existing
fault-injection and storage-state updates, but submit directly to
`osal_fsetsize()` and `osal_filesize()` at the storage boundary. This removes
the last redundant single-call primitive wrappers from this part of the
data-file storage path and leaves no references to those helper names in
`mdbx.c`. Verification passed `git diff --check`, source scans proving the
removed wrappers are gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite,
forced tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six
focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.121` batch, `1.164` crud, `0.901` iterate, `1.068`
get, and `1.076` delete.

A later outbound export descriptor cleanup added `dxb_data_export_io_t` for
environment-copy fast paths. `copy_asis()` now builds a checked export
descriptor from the remaining source byte range before attempting outbound
`sendfile()` or `copy_file_range()` acceleration. The descriptor carries both
the exact source byte request and its page coverage, plus the destination
offset used by same-filesystem regular-file copies. The storage helpers are
now `dxb_storage_sendfile_data_to_fd()` and
`dxb_storage_copy_data_to_fd()`, both of which validate the checked export
descriptor before submitting the syscall, and the old
`dxb_storage_sendfile_bytes_to_fd()` and `dxb_storage_copy_bytes_to_fd()`
helpers are gone from `mdbx.c`. Verification passed `git diff --check`, source
scans proving the old byte-export helper names are gone from `mdbx.c`, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.133` batch, `1.159`
crud, `0.848` iterate, `1.062` get, and `1.069` delete.

A later discard descriptor cleanup added `dxb_discard_io_t` for tail-discard
requests used by resize, open-time stale-tail cleanup, and commit-time shrink
cleanup. Those paths still derive byte spans from storage geometry, but now
construct a checked discard descriptor before submitting `POSIX_FADV_DONTNEED`
or future remove-style discard work. The descriptor carries the exact requested
byte range, page coverage for cache invalidation, and the discard mode. The old
loose `dxb_storage_discard_range()`, `dxb_storage_discard_clean_range()`,
`dxb_storage_discard_remove_range()`, and unused `dxb_discard_mode_valid()`
helpers are gone from `mdbx.c`; `dxb_storage_discard_io()` now validates the
descriptor at the storage boundary before applying the same advice and cache
invalidation behavior. Verification passed `git diff --check`, source scans
proving the old discard helper names are gone from `mdbx.c`, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.122` batch, `1.144` crud, `1.333`
iterate, `1.127` get, and `1.073` delete.

A later readahead advice descriptor cleanup added `dxb_advice_io_t`.
`dxb_set_readahead()` now builds checked advice descriptors for normal, random,
and willneed advice before submitting through `dxb_storage_advise_io()`. The
descriptor carries the original byte request, its page coverage, and the advice
mode, so the storage boundary validates the full range and hint instead of
accepting a loose byte span plus enum. The old `dxb_storage_advise_range()`
submitter is gone from `mdbx.c`, and source scans prove the removed helper name
has no remaining references. Verification passed `git diff --check`, source
scans covering the advice descriptor helpers and stale storage helper names,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.130` batch, `1.176`
crud, `1.294` iterate, `1.172` get, and `1.076` delete.

A later same-file copy submission cleanup removed the private
`dxb_storage_copy_file_range()` and `dxb_storage_copy_file_range_error()`
wrappers from the C source. `dxb_storage_copy_data()` already receives a
validated `dxb_data_copy_io_t`, so it now checks the syscall size and offset
limits from that descriptor and submits `copy_file_range()` directly at the
typed storage boundary. This removes the last loose byte-span helper from the
defrag overflow-tail copy path while preserving the same copy fault-injection
and destination cache invalidation behavior. Verification passed `git diff
--check`, source scans proving the removed same-file copy helper names are gone
from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.110` batch, `1.178`
crud, `0.968` iterate, `1.169` get, and `1.066` delete.

A later write-queue walk cleanup added a typed
`dxb_storage_queued_write_callback_t` boundary. The low-level `osal_ioring`
still stores byte spans, but `dxb_storage_walk_write_queue()` now converts each
walked byte segment back into a checked `dxb_data_write_io_t` before invoking
dirty-page completion. As a result, `iov_callback4dirtypages()` receives the
same typed data-write descriptor used by queue submission instead of accepting
raw `dxb_byte_io_t` ranges and reconstructing them itself. This keeps queued
write completion behind the storage descriptor layer, which is the boundary
that future async write completion should preserve. Verification passed `git
diff --check`, source scans proving the old dirty-page byte-callback shape and
callback-local `dxb_storage_make_data_write_io(storage, io, ...)` rebuild are
gone from `mdbx.c`, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.146` batch, `1.152`
crud, `0.997` iterate, `0.994` get, and `1.070` delete.

A later queued-write retention cleanup moved `dxb_page_io_t` and
`dxb_data_write_io_t` into the internal header and changed `ior_item_t` to
retain the checked data-write descriptor instead of only its byte span.
`dxb_storage_add_queued_write()` now hands the validated descriptor directly to
`osal_ioring_add()`, and ring coalescing extends both the byte range and page
coverage when adjacent writes merge. The low-level OSAL submission still writes
through the descriptor's byte member, but the queued state now preserves page
coverage needed by the storage layer and by future async write completion.
Verification passed `git diff --check`, source scans proving the old
`osal_ioring_add(..., const dxb_byte_io_t *)` shape and plain `item->io.bytes`
counter are gone, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.104` batch, `1.166`
crud, `0.857` iterate, `1.027` get, and `1.077` delete.

A later queued-write walk cleanup made `osal_ioring_walk()` descriptor-native.
Queued ring items already retain `dxb_data_write_io_t`, so the walk callback
now receives checked data-write descriptor subranges instead of byte ranges.
`dxb_data_write_subrange_io()` derives each callback request from the retained
queue item descriptor, validating the byte subrange and matching page coverage
before dirty-page completion sees it. The old byte-only
`osal_ioring_walk_bytes()` helper, `walk_write_callback` trampoline state, and
private `dxb_storage_queued_write_callback_t` boundary are gone; source scans
prove those old callback/trampoline names have no remaining references.
Verification passed `git diff --check`, source scans covering the
descriptor-native queued-write walk helpers and stale callback names, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.100` batch, `1.162`
crud, `1.176` iterate, `1.042` get, and `1.068` delete.

A later queued-write execution validation cleanup added a storage-independent
`dxb_data_write_io_validate_queued()` helper. Queue execution now validates the
retained data-write descriptor's byte span, page coverage, page count, page
size, and end page before writing or deriving walk subranges. The test-only
partial-write fault injector now receives the full `dxb_data_write_io_t`
instead of a bare `dxb_byte_io_t`, so injected partial `writev` failures are
also tied to the same queued descriptor coverage. Source scans prove the old
partial-write byte-descriptor call shape and queue-item byte-only validation
are gone. Verification passed `git diff --check`, source scans covering
queued-write descriptor validation and stale byte-only fault-injection shapes,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.131` batch, `1.157`
crud, `0.842` iterate, `1.045` get, and `1.076` delete.

A later queued-write submission cleanup added `dxb_queued_write_io_t` for the
write-queue flush target. `dxb_storage_write_queued()` now constructs and
validates that descriptor from the storage channel before calling
`osal_ioring_write()`, and OSAL queue execution consumes the descriptor instead
of accepting a bare file handle. This keeps fd resolution at the storage
boundary while preserving the existing synchronous POSIX and Windows queue
submission behavior; future async submit/completion code can hang the backend
state from this descriptor boundary. Source scans prove the old
`osal_ioring_write(..., fd)` and POSIX `osal_ioring_write_item(..., fd)` call
shapes are gone. Verification passed `git diff --check`, source scans covering
queued-write submission descriptors and stale raw-fd queue submission shapes,
the GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.099` batch, `1.158`
crud, `1.168` iterate, `0.973` get, and `1.086` delete.

A later explicit page-cache invalidation cleanup added
`dxb_cache_invalidate_io_t`. Data writes, `writev` paths, same-file copies,
file shrink cleanup, remove-style discard branches, and queued dirty-page
completion now construct checked cache-invalidation descriptors before touching
the explicit page cache. `dxb_storage_invalidate_cached_io()` validates the
descriptor at the page-cache boundary instead of accepting a loose
`dxb_page_io_t` plus policy flag, preserving the existing reusable-snapshot
policy while making future async completion invalidation descriptor-shaped.
Source scans prove the old `dxb_storage_invalidate_cached_io(..., pages, bool)`
call shape has no remaining references. Verification passed `git diff
--check`, source scans covering cache-invalidation descriptors and stale loose
invalidation calls, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.135` batch, `1.160`
crud, `0.832` iterate, `0.955` get, and `1.094` delete.

A later page-cache read cleanup added `dxb_cache_read_io_t`. `page_cache_read()`
now constructs one checked descriptor carrying the data-read request, snapshot
txnid, reusable-page policy, and cache-tracking policy before lookup or
miss-fill. `dxb_storage_lookup_cached_page()` and
`dxb_storage_read_cached_page()` validate that descriptor at the explicit
page-cache boundary, and the stale loose `(page request, snapshot,
reusable/tracked)` call shape is gone. This keeps pinned explicit page reads
descriptor-shaped for future async read completion while preserving current
synchronous public API behavior and page-cache ownership rules. Verification
passed `git diff --check`, source scans covering cache-read descriptors and
stale loose cache-read calls, the GNUmake `mdbx_migration_smoke` target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.146` batch, `1.150`
crud, `1.198` iterate, `0.949` get, and `1.084` delete.

A later overflow materialization cleanup added `dxb_cache_materialize_io_t`.
When a cached single-page overflow header must be expanded into its full
large-page span, `dxb_storage_materialize_cached_large_page()` now constructs a
checked cache materialization descriptor from the pinned page ref and validates
that descriptor before the detached-ref path consumes it. The allocation,
explicit data read, cache accounting, reusable entry update, and detached
private entry now all use the descriptor's data-read span instead of a loose
`dxb_page_io_t` rebuilt inside the materialization helper. This keeps the
remaining pinned overflow read path descriptor-shaped for future async read
completion. Verification passed `git diff --check`, source scans covering the
cache materialization descriptor and stale loose materialization calls, the
GNUmake `mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default
and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.121` batch, `1.157`
crud, `0.913` iterate, `0.957` get, and `1.072` delete.

A later public cache materialization cleanup added
`dxb_cache_entry_read_io_t` for `MDBX_cache_entry_t` hit reconstruction. The
cached-entry path now derives and validates one descriptor carrying the public
value byte range, its page-cache read descriptor, and the in-page value offset
before pinning returned `MDBX_val` data. A new `page_cache_read_io()` helper
submits prebuilt `dxb_cache_read_io_t` descriptors, while the existing
`page_cache_read()` wrapper now just builds that descriptor for ordinary page
requests and delegates. This keeps the cache-hit value materialization path
descriptor-shaped from public offset lookup through page-cache read submission,
which is the boundary future async read completion must preserve. Verification
passed `git diff --check`, source scans covering cached-entry read descriptors
and stale loose materialization calls, the GNUmake `mdbx_migration_smoke`
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest
suite, forced tiny-cache fault injection, `cmake --build @cmake-asan-build`,
the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.130` batch, `1.170` crud, `0.869` iterate,
`1.050` get, and `1.069` delete.

A later portable export fallback cleanup added `dxb_data_export_read_io_t` for
environment-copy fallback reads. The accelerated outbound copy paths already
use `dxb_data_export_io_t`; the portable fallback now derives one checked
descriptor carrying the source export byte range, the page-aligned data-read
span, and the payload offset inside the read buffer. `copy_asis()` now consumes
that descriptor instead of rebuilding a byte subrange, page coverage, read
span, and payload offset inline before `osal_write()`. This keeps the last
environment-copy read fallback behind a descriptor boundary that can later be
submitted or completed asynchronously without changing the public synchronous
copy API. Verification passed `git diff --check`, source scans covering export
read descriptors and stale loose fallback range construction, the GNUmake
`mdbx_migration_smoke` target, direct `mdbx_migration_smoke` default and forced
tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`), the six
focused `migration_smoke` CTest entries, the full 15-test public migration
CTest suite including tool roundtrips, forced tiny-cache fault injection,
`cmake --build @cmake-asan-build`, the six focused ASAN `migration_smoke`
CTest entries, and `mdbx_migration_bench_lazy`. The paired benchmark gate
passed with forced/default ratios of `1.008` batch, `0.995` crud, `1.112`
iterate, `0.989` get, and `0.998` delete.

A later queued-write merge cleanup replaced piecemeal write-ring descriptor
extension with `ior_item_make_merged_io()`. Adjacent OSAL write-ring entries
now build and validate a merged `dxb_data_write_io_t` before updating the queue
item, checking byte contiguity, page-span contiguity, page-size agreement, and
overflow bounds in one request shape. This keeps dirty-write coalescing aligned
with the descriptor that later write completion and future async submission
will observe, instead of mutating the byte and page spans field-by-field.
Verification passed `git diff --check`, source scans proving the old
`ior_item_append_io()` and `ior_item_io_contiguous()` helpers are gone from
`mdbx.c`, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.135` batch, `1.145` crud, `1.165` iterate,
`1.045` get, and `1.082` delete.

A later queued-write insertion validation cleanup made `osal_ioring_add()`
validate the full `dxb_data_write_io_t` before deriving offsets or touching ring
state. Queue insertion now rejects null descriptors/data, invalid queued write
spans, oversized writes beyond `MAX_WRITE`, and byte ranges that overflow the
data-file limit before calculating the item slot. This keeps the queue-insert
boundary on the same `dxb_data_write_io_validate_queued()` contract already
used by queued-write merging, walking, and execution. Verification passed
`git diff --check`, source scans covering queue insertion validation and stale
loose insertion checks, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.109` batch, `1.153` crud, `1.004` iterate,
`1.054` get, and `1.073` delete.

A later storage queue-insertion validation cleanup added
`dxb_storage_queued_data_write_io_validate()`. The storage-facing queued write
insert path now validates both the storage-derived data-write descriptor and
the queued-write descriptor contract before forwarding to `osal_ioring_add()`,
while `dxb_data_write_io_validate_queued()` itself now rejects null descriptors
before inspecting byte/page spans. Queue capacity preparation intentionally
stays on the broader data-write descriptor validator because spill/recovery
paths may prepare an empty write budget after earlier spilled writes. This keeps
actual queued items descriptor-shaped without making capacity reservation
stricter than execution. Verification passed `git diff --check`, source scans
covering storage queued-write validation and stale loose insertion checks, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.107` batch, `1.154` crud, `0.949` iterate,
`0.993` get, and `1.077` delete.

A later queued-write submission validation cleanup added
`dxb_queued_write_io_validate()`. Storage-side submission validation now
rejects null submit descriptors before comparing the resolved channel fd, and
`osal_ioring_write()` validates the queued-submit descriptor before any write
execution reads its fd. This keeps the final queued-write submission boundary
descriptor-checked even when future async backends submit the batch from the
OSAL layer. Verification passed `git diff --check`, source scans covering
queued-submit descriptor validation and stale direct fd checks, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.122` batch, `1.154` crud, `0.779` iterate,
`0.968` get, and `1.081` delete.

A later queued-write submit-channel cleanup moved `dxb_io_channel` into the
internal header and added it to `dxb_queued_write_io_t`. The storage submit
builder now carries both logical channel intent and the resolved fd into the
OSAL queue execution descriptor, and validation checks that both fields still
match the storage channel before submission. `osal_ioring_write()` also rejects
descriptors with invalid channels before any backend writes execute. This keeps
future async submission from seeing an fd-only request that has lost the data
vs. dsync/meta channel decision. Verification passed `git diff --check`,
source scans covering the header-level channel enum, queued submit channel
population/comparison, and removed local enum duplicate, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.144` batch, `1.137` crud, `1.197` iterate,
`0.947` get, and `1.087` delete.

A later queued-write data-channel validation cleanup made queued submit
descriptors data-channel only. `dxb_io_channel_is_data()` now lives beside the
general channel validator so `dxb_queued_write_io_validate()` and
`dxb_storage_make_queued_write_io()` can reject `dxb_io_meta` before resolving
or executing a queued write. General storage I/O still accepts the metadata
channel where appropriate, but the OSAL write queue now represents only dirty
data-file batches. This keeps future async queue submission from accepting
metadata writes through the data-write ring. Verification passed `git diff
--check`, source scans covering the single data-channel predicate and queued
submit data-channel rejection, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.131` batch, `1.168` crud, `1.014` iterate,
`0.949` get, and `1.064` delete.

A later dirty-write context cleanup made `iov_ctx` data-channel only.
`iov_init()` now accepts only `dxb_io_data` or `dxb_io_data_dsync`, and
`iov_page()` rechecks the same invariant before adding dirty pages to the write
queue. Dirty-page completion records `MDBX_EINVAL` if a corrupted context
somehow reaches shadow-page release with a non-data channel, but still
continues cleanup. This leaves metadata page writes on the explicit
`dxb_storage_write_meta()` path and keeps the OSAL write queue scoped to dirty
data-file batches before future async submission has to model queue contexts.
Verification passed `git diff --check`, source scans covering `iov_init()`,
`iov_page()`, queued-write submission, and the absence of stale
`dxb_storage_io_channel_valid(ctx->channel)` checks, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.135` batch, `1.161` crud, `0.984` iterate,
`0.904` get, and `1.075` delete.

A later dirty-write queue-preparation cleanup added
`dxb_dirty_write_queue_io_t`. `iov_init()` now builds one descriptor carrying
the data channel, requested queue capacity, and the page-span write budget, and
`dxb_storage_prepare_write_queue()` validates that descriptor before calling
`osal_ioring_prepare()`. The builder keeps the previous capacity-reservation
semantics, including empty write budgets used by spill/recovery preparation,
but queue preparation is now explicit about the channel readiness and data-file
span it is sizing for. This gives future async write submission a validated
queue-context shape instead of separate loose `items`, `npages`, and channel
arguments. Verification passed `git diff --check`, source scans covering
`dxb_dirty_write_queue_io_t`, its builder/validator, the new prepare signature,
and the remaining direct `osal_ioring_prepare()` boundary, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.105` batch, `1.158` crud, `1.225` iterate,
`0.968` get, and `1.061` delete.

A later dirty-write queue-walk cleanup added `dxb_dirty_write_walk_io_t`.
`iov_complete()` now builds one descriptor carrying the queued-write context,
its current channel, and the dirty-page release callback before walking the
OSAL write queue. `dxb_storage_walk_write_queue()` validates that descriptor
before calling `osal_ioring_walk()`, while channel-invariant errors remain in
the completion callback so shadow pages are still released after prior write
failures or corrupted context state. This keeps future async completion
plumbing explicit about the callback context it is replaying without changing
the dirty-page cleanup contract. Verification passed `git diff --check`,
source scans covering `dxb_dirty_write_walk_io_t`, its builder/validator, the
new walk signature, and the remaining direct `osal_ioring_walk()` boundary, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.116` batch, `1.152` crud, `1.266` iterate,
`0.954` get, and `1.086` delete.

A later dirty queued-write insertion cleanup added
`dxb_dirty_queued_write_io_t`. `iov_page()` now builds one descriptor carrying
the dirty data-file write span and the page-buffer pointer before asking
storage to enqueue it, and `dxb_storage_add_queued_write()` validates that
descriptor before forwarding the low-level `dxb_data_write_io_t` plus buffer to
`osal_ioring_add()`. This keeps the storage-facing enqueue boundary explicit
about both the file range and the memory backing the future async write
submission, while leaving the OSAL ring primitive as the final platform-level
descriptor split. Verification passed `git diff --check`, source scans covering
`dxb_dirty_queued_write_io_t`, its builder/validator, the new enqueue
signature, and the remaining direct `osal_ioring_add()` boundary, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.130` batch, `1.161`
crud, `0.794` iterate, `1.051` get, and `1.065` delete.

A later OSAL enqueue cleanup moved `dxb_dirty_queued_write_io_t` into the
internal header and changed `osal_ioring_add()` to accept that descriptor
directly. Storage still validates the dirty queued-write descriptor against its
page geometry before enqueueing, and OSAL now receives the same object carrying
both the data-file span and backing buffer instead of separate
`dxb_data_write_io_t` and `void *` arguments. This makes the final enqueue
boundary descriptor-shaped before future async implementations turn queued
items into platform submissions. Verification passed `git diff --check`,
source scans covering the header-level descriptor, the new `osal_ioring_add()`
signature, removal of the stale split-signature call, and the remaining dirty
queued-write users, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.109` batch, `1.158` crud, `0.997` iterate,
`0.979` get, and `1.071` delete.

A later OSAL queue-walk cleanup moved `dxb_dirty_write_walk_io_t` into the
internal header and changed `osal_ioring_walk()` to accept that descriptor
directly. Storage still validates the dirty walk descriptor against the active
queue context before completion, and OSAL now replays queued write items with
the same descriptor carrying the callback context and release callback instead
of receiving loose `iov_ctx_t *` and callback arguments. Channel-invariant
errors remain in the dirty-page callback so shadow pages are still released
after earlier write failures or corrupted context state. This makes the final
completion-walk boundary descriptor-shaped for future async completion replay.
Verification passed `git diff --check`, source scans covering the header-level
walk descriptor, the new `osal_ioring_walk()` signature, removal of the stale
split-signature call, and the remaining dirty walk users, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.073` batch, `1.166`
crud, `1.033` iterate, `0.998` get, and `1.087` delete.

A later OSAL queue-preparation cleanup moved `dxb_dirty_write_queue_io_t` into
the internal header, added the rounded `reserve_bytes` budget to the
descriptor, and changed `osal_ioring_prepare()` to accept that descriptor
directly. Storage still validates the data-channel queue descriptor and derives
the reserve budget with the previous `ceil_powerof2(..., globals.sys_pagesize)`
semantics before OSAL sizing. OSAL now receives one object carrying channel
intent, item capacity, byte reserve, and data-file span instead of separate
`items` and byte-budget arguments. This makes the final queue-preparation
boundary descriptor-shaped before future async backends size submission rings
or direct-I/O segment arrays. Verification passed `git diff --check`, source
scans covering the header-level queue descriptor, `reserve_bytes`, the new
`osal_ioring_prepare()` signature, removal of the stale split-signature call,
and the remaining queue-preparation users, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.110` batch, `1.152`
crud, `1.375` iterate, `1.005` get, and `1.067` delete.

A later queued-write submission cleanup added `used_slots` to
`dxb_queued_write_io_t`. Storage now captures the current OSAL write-queue slot
usage when it builds the submit descriptor, rejects empty submit descriptors,
and revalidates that the descriptor still matches the queue before submission.
`osal_ioring_write()` also rejects descriptors whose captured slot count no
longer matches the ring. This gives future async submission a descriptor that
describes not only the target data channel and fd but also the exact queued
batch shape being submitted. Verification passed `git diff --check`, source
scans covering `used_slots`, the queue-used helpers, queued-write submit
validation, and the OSAL write boundary, the GNUmake `mdbx_migration_smoke`
build target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
the Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.098` batch, `1.175` crud, `1.171` iterate,
`0.966` get, and `1.071` delete.

A later POSIX write-item cleanup added `osal_ioring_write_item_io_t`, so the
item executor now receives one descriptor carrying the result accumulator, the
queued ring item, and the checked queued-write submission context. The POSIX
fault-order loops now construct that descriptor for forward, reverse, and
outside-in writes instead of passing loose `result`, `item`, and submit
arguments. The executor validates the submit descriptor before using its fd and
preserves the existing scatter/gather advancement behavior on write-vector
validation errors. This gives future async submission a per-item execution
object that can be queued or completed without reconstructing state from
separate loop locals. Verification passed `git diff --check`, source scans
covering the new item descriptor and removal of the stale split-signature call,
the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.116` batch, `1.158` crud, `1.000` iterate,
`0.991` get, and `1.070` delete.

A later queued-write submission cleanup added `payload_bytes` to
`dxb_queued_write_io_t`. Storage now walks the prepared OSAL ring when building
the submit descriptor, captures both the used slot count and the total queued
payload bytes, and rejects empty or overflowed submit batches. Storage
revalidation and `osal_ioring_write()` now compare the descriptor's byte total
against the live ring before issuing writes. This makes the submitted batch
shape explicit enough for future async backends to size and validate completion
accounting without rediscovering byte totals from loop locals. Verification
passed `git diff --check`, source scans covering `payload_bytes`, the new ring
payload-byte helper, queued-write submit validation, and the OSAL write
boundary, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.160` batch, `1.133` crud, `1.048` iterate,
`1.038` get, and `1.075` delete.

A later queued-write result cleanup added `payload_bytes` to
`osal_ioring_write_result_t`. `osal_ioring_write()` now reports the submitted
payload byte total only after a queued batch completes successfully, and the
storage wrapper rejects a successful result whose byte count does not match the
checked submit descriptor. `iov_write()` also treats a zero-byte successful
write result as invalid for non-empty dirty queues. This carries byte-level
batch completion accounting across the OSAL boundary so future async backends
can validate successful completions against the descriptor they accepted.
Verification passed `git diff --check`, source scans covering the result
initializer updates, `payload_bytes`, the storage result check, and the OSAL
write boundary, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.098` batch, `1.134` crud, `1.203` iterate,
`0.974` get, and `1.085` delete.

A later queued-write result cleanup added `used_slots` to
`osal_ioring_write_result_t`. `osal_ioring_write()` now reports both submitted
slot count and payload byte count only after a queued batch completes
successfully, and the storage wrapper rejects a successful result whose slot or
byte accounting does not match the checked submit descriptor. `iov_write()`
also rejects zero-slot or zero-byte successful results for non-empty dirty
queues. This carries the complete submitted batch shape across the OSAL
completion boundary so future async backends can report and validate
completion state without relying on caller-side ring walks after submission.
Verification passed `git diff --check`, source scans covering result
initializer updates, `used_slots`, `payload_bytes`, the storage result checks,
and the OSAL write boundary, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.112` batch, `1.161` crud, `1.015` iterate,
`1.009` get, and `1.079` delete.

A later queued-write submission cleanup added `write_items` to
`dxb_queued_write_io_t`. Storage now captures the logical OSAL ring item count
beside the used slot count and queued payload bytes, and both storage
revalidation and `osal_ioring_write()` reject a descriptor whose item count no
longer matches the live ring. Successful queued writes must now report `wops`
matching that descriptor item count, and `iov_write()` treats a zero-operation
successful write result as invalid for non-empty dirty queues. The OSAL ring
now uses one item-stride helper for item counting and payload-byte accounting,
which keeps future async backends from depending on duplicated traversal logic
when validating submitted batches and completions. Verification passed
`git diff --check`, source scans covering `write_items`, the OSAL item-count
helper, result `wops` checks, and queued-write descriptors, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.128` batch, `1.173`
crud, `1.112` iterate, `0.964` get, and `1.080` delete.

A later queued-write result cleanup added `write_items` to
`osal_ioring_write_result_t`. `osal_ioring_write()` now reports the submitted
logical item count only after a queued batch completes successfully, and the
storage wrapper validates that item completion count against the checked submit
descriptor. `iov_write()` rejects zero-item successful results separately from
zero `wops`, so logical queue completion accounting is no longer overloaded on
the physical write-operation statistic. This lets future async backends report
completed queue items independently from the number of write operations used to
submit or complete them. Verification passed `git diff --check`, source scans
covering the result initializer updates, `write_items`, the storage result
check, and the OSAL write boundary, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.117` batch, `1.158` crud, `0.968` iterate,
`1.005` get, and `1.081` delete.

A later direct-write cleanup added `dxb_write_result_t` for storage writes that
do not go through the dirty-page queue. `dxb_storage_write_data()`,
`dxb_storage_write_meta()`, and `dxb_storage_writev_data()` now return an
explicit result containing the error code, completed write-operation count, and
payload byte count instead of returning only `int`. Existing callers still
thread the same `.err` value through their control flow, and best-effort page
kill paths explicitly discard the result, so this keeps current success/error
semantics while making direct write completions look more like queued write
completions. This gives future async storage backends a common place to report
direct data/meta write completion details without rediscovering them at call
sites. Verification passed `git diff --check`, source scans covering
`dxb_write_result_t`, direct storage write wrappers, and every
`dxb_storage_write_data()`, `dxb_storage_write_meta()`, and
`dxb_storage_writev_data()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.142` batch, `1.165` crud, `1.211` iterate,
`0.957` get, and `1.069` delete.

A later direct-read cleanup added `dxb_read_result_t` for storage reads that
pull data or metadata directly from the data file. `dxb_storage_read_data()` and
`dxb_storage_read_meta()` now return an explicit result containing the error
code and completed payload byte count instead of returning only `int`. Existing
callers still unwrap the same `.err` value, preserving the current synchronous
control flow, while successful direct reads now have a completion payload field
that future async read backends can report without rediscovering byte counts at
each call site. Verification passed `git diff --check`, source scans covering
`dxb_read_result_t`, direct storage read wrappers, and every
`dxb_storage_read_data()` and `dxb_storage_read_meta()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.112` batch, `1.154`
crud, `1.421` iterate, `1.155` get, and `1.080` delete.

A later direct-sync cleanup added `dxb_sync_result_t` for storage syncs.
`dxb_storage_sync_io()` now returns an explicit result containing the error code
and synchronized payload byte count instead of returning only `int`. Existing
callers still unwrap the same `.err` value, preserving current durability
control flow, while successful syncs now report the checked descriptor byte span
after the existing `sync-complete` fault-injection point. No-op sync modes
report zero payload bytes on success. This gives future async sync backends a
completion-shaped boundary beside direct reads, direct writes, and queued
writes. Verification passed `git diff --check`, source scans covering
`dxb_sync_result_t`, the direct storage sync wrapper, and every
`dxb_storage_sync_io()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.125` batch, `1.140` crud, `0.960` iterate,
`1.019` get, and `1.075` delete.

A later file-size I/O cleanup added `dxb_filesize_result_t` for storage
file-size operations. `dxb_storage_fetch_filesize()`,
`dxb_storage_set_filesize_on_disk()`, `dxb_storage_set_filesize_bytes()`, and
`dxb_storage_set_filesize_as_current()` now return an explicit result containing
the error code and completed file size instead of returning only `int`.
Existing callers still unwrap the same `.err` value, preserving setup, header
read, validation, and resize control flow, while successful filesize and
set-length operations now have a completion field that future async file-size
backends can report without reading storage state back from side effects.
Verification passed `git diff --check`, source scans covering
`dxb_filesize_result_t`, file-size helper wrappers, and every
`dxb_storage_fetch_filesize()`, `dxb_storage_set_filesize_bytes()`, and
`dxb_storage_set_filesize_as_current()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.134` batch, `1.155`
crud, `0.845` iterate, `1.001` get, and `1.076` delete.

A later range-advisory cleanup added `dxb_range_result_t` for descriptor-shaped
range operations. `dxb_storage_advise_io()` and `dxb_storage_discard_io()` now
return an explicit result containing the error code and completed range byte
count instead of returning only `int`. Existing callers still unwrap the same
`.err` value and continue treating `MDBX_RESULT_TRUE` as a non-error advisory
fallback, while successful `posix_fadvise()`/`F_RDADVISE` and discard requests
now report the checked request byte span. This puts readahead/random/DONTNEED
range operations on the same completion-shaped path as direct reads, writes,
syncs, and file-size operations for future async-capable backends. Verification
passed `git diff --check`, source scans covering `dxb_range_result_t`, range
result helpers, and every `dxb_storage_advise_io()` and
`dxb_storage_discard_io()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.138` batch, `1.159` crud, `1.032` iterate,
`0.998` get, and `1.077` delete.

A later copy/export cleanup added `dxb_copy_result_t` for data-file copy
operations. `dxb_storage_copy_data()`, `dxb_storage_copy_data_to_fd()`, and
`dxb_storage_sendfile_data_to_fd()` now return an explicit result containing
the error code, completed payload byte count, copy-completion state, and the
existing fallback flags for unavailable kernel helpers and cross-device copies.
Existing callers still consume the same `.err` value and set the same
`copy_file_range()`/`sendfile()` fallback booleans, preserving current copy and
export behavior while removing completion side channels from out parameters.
This gives future async copy backends a single return object for completion,
fallback, and byte-count reporting. Verification passed `git diff --check`,
source scans covering `dxb_copy_result_t`, copy result helpers, and every
`dxb_storage_copy_data()`, `dxb_storage_copy_data_to_fd()`, and
`dxb_storage_sendfile_data_to_fd()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.124` batch, `1.144`
crud, `1.280` iterate, `0.924` get, and `1.073` delete.

A later resize/setup cleanup added `dxb_resize_result_t` for storage geometry
operations. `dxb_storage_setup_size()` and `dxb_storage_resize_size()` now
return an explicit result containing the error code, current byte size, limit
byte size, and observed file size instead of returning only `int`. Existing
callers still unwrap the same `.err` value in `dxb_setup()` and `dxb_resize()`,
preserving setup, grow, shrink, and read-only resize behavior while making the
post-operation storage geometry available as completion data. This gives future
async resize backends a single return object for state reporting after file
size changes and geometry refreshes. Verification passed `git diff --check`,
source scans covering `dxb_resize_result_t`, resize result helpers, and every
`dxb_storage_setup_size()` and `dxb_storage_resize_size()` call site, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the
full 15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.101` batch,
`1.128` crud, `0.903` iterate, `1.047` get, and `1.068` delete.

A later page-cache cleanup added `dxb_cache_result_t` for cache
materialization. `dxb_storage_materialize_cached_large_page()` and
`dxb_storage_detach_materialized_large_page()` now return an explicit result
containing the error code, materialized payload byte count, materialized page
count, and whether a shared pinned cache entry had to be detached. The existing
`page_cache_read_large()` caller still unwraps the same `.err` value, preserving
overflow-page read behavior and cursor-visible page lifetimes while exposing
materialization completion data. This gives future async page-cache read
backends a place to report extent completion and detach decisions without
threading state through side effects only. Verification passed
`git diff --check`, source scans covering `dxb_cache_result_t`, cache result
helpers, and every `dxb_storage_materialize_cached_large_page()` and
`dxb_storage_detach_materialized_large_page()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.140` batch, `1.153`
crud, `0.867` iterate, `1.074` get, and `1.070` delete.

A later cache-invalidation cleanup extended `dxb_cache_result_t` with a cache
entry count and converted `dxb_storage_invalidate_cached_io()` from a `void`
side-effect helper into an explicit result. Cache invalidation now returns the
validated byte range, page count, and number of cache entries marked stale or
released, while existing write, copy, discard, truncate, and queued-write
callers continue to ignore the result and therefore preserve their previous
success/error behavior. This gives future async-capable page-cache backends a
completion boundary for cache eviction decisions after data-file writes and
file-size changes. Verification passed `git diff --check`, source scans
covering `dxb_cache_result_t`, cache invalidation result helpers, and every
`dxb_storage_invalidate_cached_io()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.122` batch, `1.153`
crud, `0.970` iterate, `0.962` get, and `1.071` delete.

A later readahead-toggle cleanup added `dxb_readahead_result_t` for the direct
data-file `F_RDAHEAD` switch. `dxb_storage_set_readahead()` now returns an
explicit result containing the error code, requested enabled state, and whether
the platform had a concrete toggle instead of returning only `int`. The public
`dxb_set_readahead()` path still unwraps the same `.err` value, preserving
resize/open readahead behavior while letting future async-capable advisory
backends report no-op versus real descriptor-toggle completion. Verification
passed `git diff --check`, source scans covering `dxb_readahead_result_t`,
readahead result helpers, and every `dxb_storage_set_readahead()` call site,
the GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the
full 15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.077` batch,
`1.145` crud, `0.991` iterate, `0.961` get, and `1.068` delete.

A later descriptor-sysinfo cleanup added `dxb_sysinfo_result_t` for public
environment information queries that read data-file descriptor metadata.
`dxb_storage_fetch_sysinfo()` now returns an explicit result containing the
error code, observed data-file size, filesystem allocation, and filesystem I/O
block size instead of writing directly into `MDBX_envinfo`. The public
`env_info_sys()` wrapper still initializes and fills the same ABI fields after
successful completion, preserving existing `mdbx_env_info_ex()` behavior while
giving future async-capable metadata backends a single completion boundary for
descriptor stat information. Verification passed `git diff --check`, source
scans covering `dxb_sysinfo_result_t`, sysinfo result helpers, and every
`dxb_storage_fetch_sysinfo()` call site, the GNUmake `mdbx_migration_smoke`
build target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
the Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.140` batch, `1.174` crud, `0.984` iterate,
`1.036` get, and `1.075` delete.

A later in-core detection cleanup added `dxb_incore_result_t` for the
data-file descriptor check that detects fully in-core database files.
`dxb_storage_check_incore()` now returns an explicit result containing the
normalized error code and detected in-core state instead of returning `int`
through a boolean out parameter. The `env_open()` caller still assigns
`env->incore`, emits the same notice for in-core databases, and fails on the
same `osal_check_fs_incore()` errors, preserving open behavior while making the
descriptor classification result explicit for future async-capable storage
setup. Verification passed `git diff --check`, source scans covering
`dxb_incore_result_t`, incore result helpers, and every
`dxb_storage_check_incore()` call site, the GNUmake `mdbx_migration_smoke`
build target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
the Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.109` batch, `1.149` crud, `0.772` iterate,
`1.074` get, and `1.073` delete.

A later write-queue lifecycle cleanup added `dxb_queue_result_t` for the dirty
page write queue used by the explicit data-file write path.
`dxb_storage_create_write_queue()` and `dxb_storage_destroy_write_queue()` now
return an explicit result containing the error code, readonly/no-op state, and
queue slot counts instead of returning only `int` or `void`. The `env_open()`
caller still unwraps the same create error, while `env_close()` still treats
destroy as cleanup, preserving open/close behavior while making queued-write
backend setup and teardown completion data available for future async-capable
storage. Verification passed `git diff --check`, source scans covering
`dxb_queue_result_t`, queue result helpers, and every
`dxb_storage_create_write_queue()` and `dxb_storage_destroy_write_queue()` call
site, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.107` batch, `1.149` crud, `0.966` iterate,
`1.016` get, and `1.070` delete.

A later data-file open cleanup added `dxb_open_result_t` for storage descriptor
open operations. `dxb_storage_open_data()`, `dxb_storage_open_dsync()`, and the
Windows `dxb_storage_open_overlapped()` path now return an explicit result with
the error code and data/meta/dsync/overlapped descriptor state instead of
returning only `int`. Existing public open and pre-open info paths still unwrap
the same `.err` value, preserving setup control flow while making descriptor
open completion data available for future async-capable storage backends.
Verification passed `git diff --check`, source scans covering
`dxb_open_result_t`, open result helpers, and every
`dxb_storage_open_data()`, `dxb_storage_open_dsync()`, and
`dxb_storage_open_overlapped()` call site, the GNUmake `mdbx_migration_smoke`
build target, direct `mdbx_migration_smoke` default and forced tiny-cache runs,
the Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.096` batch, `1.152` crud, `0.969` iterate,
`0.985` get, and `1.078` delete.

A later data-file close cleanup added `dxb_close_result_t` for storage
descriptor teardown. `dxb_storage_close_handles()` and `dxb_storage_close()`
now return an explicit result with the error code, whether data/dsync handles
were present, which handles closed successfully, and whether the public close
wrapper reset storage state. The normal `env_close()` path still ignores close
errors as before, while `lck_destroy()` now uses `close_result.had_data` in
place of the previous boolean out parameter when deciding whether to restore
POSIX data-file locks. This preserves lock restoration and teardown control
flow while making descriptor close completion data available for future
async-capable storage backends. Verification passed `git diff --check`, source
scans covering `dxb_close_result_t`, close result helpers, and every
`dxb_storage_close_handles()` and `dxb_storage_close()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.127` batch, `1.154`
crud, `0.781` iterate, `1.038` get, and `1.079` delete.

A later storage lifecycle cleanup added `dxb_deinit_result_t` for storage
teardown after environment close. `dxb_storage_deinit()` now returns an
explicit result with the error code, whether storage state was reset, whether
the page-cache mutex had been initialized, and whether that mutex was destroyed
successfully. The environment creation bailout still ignores storage deinit
errors as before, while final environment destruction asserts the same `.err`
value it previously compared directly. This keeps public teardown behavior
unchanged while preserving cache/queue lifecycle completion metadata for future
async-capable storage backends. Verification passed `git diff --check`, source
scans covering `dxb_deinit_result_t`, deinit result helpers, and every
`dxb_storage_deinit()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.121` batch, `1.160` crud, `1.053` iterate,
`0.937` get, and `1.088` delete.

A later storage initialization cleanup added `dxb_init_result_t` for the
storage lifecycle entry point. `dxb_storage_init()` now returns an explicit
result with the error code, configured explicit page-cache limit, whether reset
state was established, and whether the page-cache mutex was initialized
successfully. `mdbx_env_create()` still gates creation on the same error value,
but storage-owned initialization completion is now available to future
async-capable cache and queue setup. Verification passed `git diff --check`,
source scans covering `dxb_init_result_t`, init result helpers, and every
`dxb_storage_init()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.129` batch, `1.156` crud, `1.003` iterate,
`0.992` get, and `1.077` delete.

A later POSIX descriptor stat cleanup added `dxb_stat_result_t` for
`dxb_storage_stat()`. The storage stat helper now returns an explicit result
with the error code and `struct stat` payload instead of using an out parameter.
Environment close, POSIX lock-file mode inheritance, storage sysinfo fetch,
DXB/LCK validation, and SysV IPC permission setup still use the same metadata
and error values, but stat completion is now represented like the other
storage-owned probes for future async-capable backends. Verification passed
`git diff --check`, source scans covering `dxb_stat_result_t`, stat result
helpers, and every `dxb_storage_stat()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.137` batch, `1.159`
crud, `0.998` iterate, `0.980` get, and `1.077` delete.

A later read-only filesystem probe cleanup added `dxb_readonly_result_t` for
`dxb_storage_check_readonly()`. The storage helper now returns the original
open error, the read-only probe error, whether the filesystem was confirmed
read-only, and whether the probe was supported instead of returning only an
`int`. `lck_setup()` still preserves the same without-lock fallback behavior
for read-only/exclusive opens, but the storage-owned probe now exposes its
completion state for future async-capable backends. Verification passed
`git diff --check`, source scans covering `dxb_readonly_result_t`, read-only
result helpers, and every `dxb_storage_check_readonly()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.122` batch, `1.162`
crud, `0.968` iterate, `1.019` get, and `1.071` delete.

A later descriptor parking cleanup added `dxb_park_result_t` for the storage
helpers that move data, dsync, and Windows overlapped descriptors to the safe
parking offset after open. `dxb_storage_park_data()`,
`dxb_storage_park_dsync()`, and `dxb_storage_park_overlapped()` now return the
error code, I/O channel, requested offset, whether a descriptor was open, and
whether the seek completed. `env_open()` still explicitly ignores those results
as before, preserving the existing best-effort parking behavior while exposing
completion state for future async-capable storage backends. Verification passed
`git diff --check`, source scans covering `dxb_park_result_t`, park result
helpers, and every `dxb_storage_park_*()` call site, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.113` batch, `1.149`
crud, `0.982` iterate, `0.954` get, and `1.071` delete.

A later data-file lock completion cleanup added `dxb_lock_result_t` for the
POSIX DXB `fcntl()` lock wrappers. `dxb_storage_lock_op()` and
`dxb_storage_setlk_with3retries()` now return the error code, command, lock
type, byte range, attempted/completed state, and whether the retry wrapper was
used. Lock seize, downgrade, upgrade, destroy, and neighbor lock restore still
unwrap `.err`, preserving the existing ordering and retry behavior while
exposing data-file lock completion state for future async-capable storage
backends. Verification passed `git diff --check`, source scans covering
`dxb_lock_result_t`, the lock result helper, and every
`dxb_storage_lock_op()`/`dxb_storage_setlk_with3retries()` call site, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.115` batch,
`1.156` crud, `0.828` iterate, `1.037` get, and `1.068` delete.

A later storage-state cleanup added `dxb_state_result_t` for reset and size
bookkeeping helpers. `dxb_storage_reset()`,
`dxb_storage_set_filesize()`, `dxb_storage_set_current()`,
`dxb_storage_set_size()`, `dxb_storage_set_size_with_known_filesize()`,
`dxb_storage_set_limit_from_filesize()`, and
`dxb_storage_note_filesize()` now report the error code plus the resulting
current size, limit, recorded file size, and whether a reset occurred. Resize,
open, checker, transaction setup, close, deinit, and POSIX lock teardown paths
still unwrap `.err`, preserving the existing state transitions while making
storage state mutation completions visible to future async-capable backends.
Verification passed `git diff --check`, source scans covering
`dxb_state_result_t`, the state result helper, reset and size helper returns,
and every converted caller, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.104` batch, `1.161` crud, `1.233` iterate, `1.186`
get, and `1.087` delete.

A later dirty write-queue operation cleanup added `dxb_queue_op_result_t` for
queue prepare, enqueue, walk, and reset helpers. `dxb_storage_prepare_write_queue()`,
`dxb_storage_add_queued_write()`, `dxb_storage_walk_write_queue()`, and
`dxb_storage_reset_write_queue()` now report the error code, allocated/used ring
slots, queued write items, queued payload bytes, and which operation completed.
`iov_init()`, `iov_complete()`, and `iov_page()` still unwrap `.err`, preserving
the existing queue-full retry and dirty-page cleanup behavior while exposing
batch-queue operation completions for future async-capable write submission.
Verification passed `git diff --check`, source scans covering
`dxb_queue_op_result_t`, the queue operation result helper, every converted
queue operation wrapper, and each `.err`-unwrapping caller, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.109` batch, `1.142` crud,
`0.961` iterate, `0.999` get, and `1.071` delete.

A later OSAL write-ring operation cleanup moved `dxb_queue_op_result_t` down to
`osal_ioring_add()`, `osal_ioring_walk()`, and `osal_ioring_reset()`. The
storage write-queue helpers still validate their descriptors first, then return
the OSAL operation result directly so enqueue coalescing, walk callback errors,
and reset completion expose the same allocated/used slot, write item, and queued
payload counters at the lower I/O boundary. `osal_ioring_prepare()` remains a
small header inline around reservation resize, with storage still wrapping its
`int` result until that resize path is split into an out-of-line queue operation.
Verification passed `git diff --check`, source scans covering
`dxb_queue_op_result_t`, the OSAL write-ring operation signatures, the storage
forwarders, and each `.err`-unwrapping caller, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.078` batch, `1.163`
crud, `1.006` iterate, `0.977` get, and `1.084` delete.

A later OSAL write-ring prepare cleanup moved `osal_ioring_prepare()` out of
the internal header and changed it to return `dxb_queue_op_result_t` directly.
The full OSAL write-ring operation set (`prepare`, `add`, `walk`, and `reset`)
now reports queue operation completion with the same allocated/used slot, write
item, queued payload, and operation-state fields, while
`dxb_storage_prepare_write_queue()` still performs descriptor validation before
forwarding the OSAL result. This removes the last header-inline write-ring
operation that returned a bare `int`, leaving queue capacity reservation ready
to become a submit/completion-backed backend step. Verification passed `git diff
--check`, source scans covering `dxb_queue_op_result_t`,
`osal_ioring_prepare()`, storage prepare forwarding, and each
`.err`-unwrapping caller, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.090` batch, `1.162` crud, `1.028` iterate, `1.011`
get, and `1.066` delete.

A later queued-write storage result cleanup added `dxb_queue_write_result_t`.
`dxb_storage_write_queued()` now translates `osal_ioring_write_result_t` into a
storage-owned result containing the dirty-write channel, physical write
operations, queued slot/item/payload accounting, and explicit submitted and
completed state. `iov_write()` now consumes that storage result and no longer
depends on the OSAL result type, while preserving the same error propagation,
write-operation statistics, and completion cleanup behavior. This keeps queued
dirty-page submission behind the storage abstraction so a later async backend
can report completion state without leaking ring-specific result types into
transaction code. Verification passed `git diff --check`, source scans covering
`dxb_queue_write_result_t`, storage queued-write result translation,
`osal_ioring_write_result_t` confinement, and the `iov_write()` caller, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.079` batch,
`1.174` crud, `1.007` iterate, `0.997` get, and `1.083` delete.

A later page-cache read result cleanup added an internal
`dxb_cache_page_result_t` around cached page references. `dxb_storage_lookup_cached_page()`
and `dxb_storage_read_cached_page()` now report cache hit, miss-fill, and
tracked-entry state explicitly instead of using only `pgr_t.err` to distinguish
lookup hits from `MDBX_RESULT_TRUE` misses. `page_cache_read_io()` still unwraps
the same `pgr_t` for callers, preserving page reference lifetime and error
behavior, but the storage-cache boundary now exposes whether a page came from a
reusable cache hit or from an explicit storage read. This gives a later
async-capable read backend a place to report cache-hit versus submitted-read
completion without changing cursor/page APIs. Verification passed `git diff
--check`, source scans covering `dxb_cache_page_result_t`, cache hit/miss/fill
helpers, `dxb_storage_lookup_cached_page()`, `dxb_storage_read_cached_page()`,
and `page_cache_read_io()`, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.096` batch, `1.151` crud, `1.019` iterate, `1.004`
get, and `1.075` delete.

A later overflow read result cleanup changed `page_cache_read_large()` from an
`int` helper into a `dxb_cache_result_t` helper. Callers still unwrap the same
`.err` value into their existing `pgr_t` or local error state, preserving
overflow-page read behavior and cursor-visible page lifetimes, but the
transaction-level large-page helper now keeps materialized byte count, page
count, entry count, and detach-state metadata available after storage
materialization. This narrows another async-facing boundary: a future read
backend can report whether an overflow extent completed as a no-op, in-place
cache expansion, or detached pinned buffer without adding side-channel state to
cursor code. Verification passed `git diff --check`, source scans covering
`page_cache_read_large()`, `dxb_cache_result_t`, `dxb_cache_success()`, and all
large-page read call sites, the GNUmake `mdbx_migration_smoke` build target,
direct `mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja
build (`cmake --build @cmake-ninja-build`), the six focused `migration_smoke`
CTest entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.091` batch, `1.186` crud, `0.951` iterate, `0.979`
get, and `1.077` delete.

A later committed-read result cleanup propagated `dxb_cache_page_result_t`
through `page_cache_read_io()`, `page_cache_read()`, and
`page_get_committed()`. The broader cursor and public cache paths still unwrap
the same `pgr_t`, preserving page lifetime and error behavior, but cache-hit,
miss-fill, and tracked-entry metadata now survives up to the committed-page
lookup boundary instead of being collapsed inside the cache helper. This gives a
future async read backend a cleaner completion shape for distinguishing reusable
hits from submitted storage reads without changing cursor-visible page APIs.
Verification passed `git diff --check`, source scans covering
`dxb_cache_page_result_t`, `page_cache_read_io()`, `page_cache_read()`,
`page_get_committed()`, and the public cache materialization caller, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the full
15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.108` batch,
`1.158` crud, `1.211` iterate, `0.949` get, and `1.074` delete.

A later cache-read completion cleanup extended `dxb_cache_page_result_t` with a
`payload_bytes` field. Cache hits now report the cached page span size, and
miss fills preserve the `dxb_storage_read_data()` completion byte count instead
of collapsing the storage read result to an error code. Existing callers still
unwrap the same `pgr_t`, but the async-facing cache read result now carries the
completed payload size needed by a future submitted-read completion path.
Verification passed `git diff --check`, source scans covering
`dxb_cache_page_result_t`, `dxb_cache_page_result()`, `payload_bytes`, and the
cache hit/fill paths, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.111` batch, `1.148` crud, `1.171` iterate, `0.931`
get, and `1.073` delete.

A later overflow materialization completion cleanup made
`dxb_storage_materialize_cached_large_page()` preserve the
`dxb_storage_read_data()` completion payload. `dxb_cache_materialized()` now
receives the actual read `payload_bytes`, and
`dxb_storage_detach_materialized_large_page()` threads that value through when a
shared pinned overflow header must detach into a private buffer. Existing
callers still unwrap only `.err`, but the async-facing materialization result
now reports the completed read size consistently for both in-place cache
expansion and detached-buffer completion. Verification passed `git diff
--check`, source scans covering `dxb_cache_materialized()`,
`dxb_storage_materialize_cached_large_page()`,
`dxb_storage_detach_materialized_large_page()`, and `read_result.payload_bytes`,
the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.120` batch, `1.162` crud, `1.016` iterate, `1.024`
get, and `1.074` delete.

A later direct-read result cleanup extended `dxb_read_result_t` with
`submitted` and `completed` flags, matching the async-facing completion shape
already used by queued dirty writes. Synchronous `dxb_storage_read_data()` and
`dxb_storage_read_meta()` now return completed read results on success and
unsubmitted results on error. Page-cache miss fills and overflow materialization
now validate that successful storage reads were submitted, completed, and
reported the expected payload size before exposing their buffers as page-cache
entries. This gives a later async read backend explicit submission/completion
state at the same boundary where page buffers become cursor-visible.
Verification passed `git diff --check`, source scans covering
`dxb_read_result_t`, `dxb_read_result()`, `dxb_read_error()`,
`dxb_read_completed()`, and the cache read-result checks, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.129` batch, `1.149`
crud, `0.864` iterate, `1.045` get, and `1.074` delete.

A later direct-write result cleanup extended `dxb_write_result_t` with
`submitted` and `completed` flags, matching the state already carried by queued
dirty-write completions. Direct data, metadata, and writev writes now
distinguish validation/pre-submit errors from submitted-but-not-completed
write failures, while post-write cache-invalidation descriptor failures
preserve completed write state and the written payload byte count. This keeps
the direct synchronous write boundary shaped for a later async backend without
changing existing callers that still unwrap only `.err`. Verification passed
`git diff --check`, source scans covering `dxb_write_result_t`,
`dxb_write_result()`, `dxb_write_error()`, `dxb_write_submitted_error()`,
`dxb_write_completed_error()`, `dxb_write_completed()`, and the direct write
paths, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.112` batch, `1.142` crud, `1.003` iterate, `0.914`
get, and `1.079` delete.

A later sync result cleanup extended `dxb_sync_result_t` with `submitted` and
`completed` flags, matching the direct read/write and queued dirty-write
completion shape. `dxb_storage_sync_io()` now distinguishes validation and
pre-submit sync faults from submitted-but-not-completed fsync/fdatasync
failures, while `MDBX_SYNC_NONE`/`MDBX_SYNC_KICK` no-op syncs can report
completed without a submitted kernel operation. This keeps data and metadata
sync boundaries explicit for future async completion handling without changing
current callers that still unwrap only `.err`. Verification passed `git diff
--check`, source scans covering `dxb_sync_result_t`, `dxb_sync_result()`,
`dxb_sync_error()`, `dxb_sync_submitted_error()`,
`dxb_sync_noop_completed()`, `dxb_sync_completed()`, and the storage sync path,
the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.113` batch, `1.155` crud, `0.977` iterate, `1.080`
get, and `1.080` delete.

A later file-size result cleanup extended `dxb_filesize_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by
direct read/write and sync results. Data-file setsize and filesize probes now
distinguish validation/pre-submit faults from submitted-but-not-completed
syscall or completion-hook failures, while post-operation storage-state update
failures preserve completed file-size state and the observed target size. This
keeps grow/shrink/probe boundaries explicit for future async resize handling
without changing current callers that still unwrap only `.err`. Verification
passed `git diff --check`, source scans covering `dxb_filesize_result_t`,
`dxb_filesize_result()`, `dxb_filesize_error()`,
`dxb_filesize_submitted_error()`, `dxb_filesize_completed_error()`,
`dxb_filesize_completed()`, and the storage setsize/probe paths, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.101` batch, `1.141`
crud, `0.928` iterate, `1.016` get, and `1.084` delete.

A later range-operation result cleanup extended `dxb_range_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by the
other explicit storage operations. Data-file advice and discard paths now
distinguish validation and zero-length no-ops from submitted
`posix_fadvise()`/`F_RDADVISE` operations, and unsupported discard fallbacks
report a completed unsubmitted `MDBX_RESULT_TRUE` result instead of looking
like an attempted range operation. This keeps readahead, DONTNEED, and
discard/resize-advice boundaries explicit for future async range handling
without changing current callers that still unwrap only `.err`. Verification
passed `git diff --check`, source scans covering `dxb_range_result_t`,
`dxb_range_result()`, `dxb_range_error()`, `dxb_range_submitted_error()`,
`dxb_range_noop_completed()`, `dxb_range_unavailable()`,
`dxb_range_completed()`, and the storage advice/discard paths, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.105` batch, `1.154`
crud, `0.881` iterate, `1.030` get, and `1.079` delete.

A later copy result cleanup extended `dxb_copy_result_t` with `submitted` and
`completed` flags, matching the async-facing state used by the other explicit
storage operations. Export `copy_file_range()`/`sendfile()` helpers and the
internal same-file copy path now distinguish validation/pre-submit failures from
submitted-but-incomplete syscall failures, unavailable/cross-device fallback
results, partial copy completions, and post-copy cache-invalidation descriptor
failures that occur after the kernel copy completed. `copied` remains true only
on successful copies so existing fallback control flow is unchanged.
Verification passed `git diff --check`, source scans covering
`dxb_copy_result_t`, `dxb_copy_result()`, `dxb_copy_error()`,
`dxb_copy_submitted_error()`, `dxb_copy_incomplete_error()`,
`dxb_copy_completed_error()`, `dxb_copy_completed()`,
`dxb_copy_unavailable()`, `dxb_copy_cross_device()`, and the copy result
callers, the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.106` batch, `1.163` crud, `1.183` iterate, `0.953`
get, and `1.080` delete.

A later resize result cleanup extended `dxb_resize_result_t` with `submitted`
and `completed` flags, matching the async-facing state used by the lower-level
filesize operations that perform `ftruncate()`/filesize probes. Storage setup
and resize helpers now distinguish validation/pre-submit failures from
submitted filesystem size operations, preserve the submitted/completed state
when post-operation storage-state updates fail, and report successful setup or
resize after the underlying filesize operation completes. Current callers still
unwrap only `.err`, so the public behavior is unchanged while future async
resize completion handling gets an explicit state boundary. Verification passed
`git diff --check`, source scans covering `dxb_resize_result_t`,
`dxb_resize_result()`, `dxb_resize_state()`, `dxb_resize_error()`,
`dxb_resize_after_filesize()`, and the setup/resize callers, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.107` batch, `1.160`
crud, `1.298` iterate, `0.955` get, and `1.067` delete.

A later readahead result cleanup extended `dxb_readahead_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by
range advice and other explicit storage operations. The direct
`F_RDAHEAD` toggle now reports submitted-but-not-completed syscall failures,
submitted/completed successful toggles, and unsupported no-op completion when
the platform has no direct toggle. `enabled` and `supported` remain available
for policy decisions, and current callers still unwrap only `.err`.
Verification passed `git diff --check`, source scans covering
`dxb_readahead_result_t`, `dxb_readahead_result()`,
`dxb_readahead_submitted_error()`, `dxb_readahead_noop_completed()`,
`dxb_readahead_completed()`, and the `dxb_storage_set_readahead()` call site,
the GNUmake `mdbx_migration_smoke` build target, direct
`mdbx_migration_smoke` default and forced tiny-cache runs, the Ninja build
(`cmake --build @cmake-ninja-build`), the six focused `migration_smoke` CTest
entries, the full 15-test public migration CTest suite including tool
roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.104` batch, `1.150` crud, `0.813` iterate, `1.049`
get, and `1.078` delete.

A later stat/sysinfo result cleanup extended `dxb_stat_result_t` and
`dxb_sysinfo_result_t` with `submitted` and `completed` flags, matching the
async-facing state used by the other explicit storage probes. POSIX `fstat()`
results now distinguish submitted metadata-probe failures from successful
submitted/completed probes, while sysinfo treats an invalid data descriptor as
an unsubmitted completed no-op and propagates POSIX stat submission state into
derived sysinfo errors. Windows file-info probes report submitted/completed
success or submitted/not-completed failure. Current callers still unwrap only
`.err` and the value fields. Verification passed `git diff --check`, source
scans covering `dxb_stat_result_t`, `dxb_stat_result()`,
`dxb_stat_submitted_error()`, `dxb_stat_completed()`,
`dxb_stat_zero_submitted_error()`, `dxb_sysinfo_result_t`,
`dxb_sysinfo_result()`, `dxb_sysinfo_error()`,
`dxb_sysinfo_from_stat_error()`, `dxb_sysinfo_noop_completed()`,
`dxb_sysinfo_completed()`, and the stat/sysinfo callers, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.131` batch, `1.157`
crud, `1.071` iterate, `1.060` get, and `1.077` delete.

A later filesystem probe cleanup extended `dxb_incore_result_t` and
`dxb_readonly_result_t` with `submitted` and `completed` flags, matching the
async-facing state used by the other explicit storage probes. In-core checks
now report submitted/completed successful probes, submitted/not-completed probe
errors, and unsubmitted completed unsupported probes. Read-only filesystem
checks now preserve the original source error while distinguishing successful
submitted probes, unsupported unsubmitted fallbacks, and submitted probe
failures. Current callers still unwrap only `.err`, `.incore`, `.readonly`, and
`.supported`. Verification passed `git diff --check`, source scans covering
`dxb_incore_result_t`, `dxb_incore_result()`,
`dxb_incore_submitted_error()`, `dxb_incore_unavailable()`,
`dxb_incore_completed()`, `dxb_readonly_result_t`,
`dxb_readonly_result()`, `dxb_readonly_completed()`,
`dxb_readonly_unavailable()`, `dxb_readonly_submitted_error()`,
`dxb_readonly_from_probe()`, and the incore/readonly callers, the GNUmake
`mdbx_migration_smoke` build target, direct `mdbx_migration_smoke` default and
forced tiny-cache runs, the Ninja build (`cmake --build @cmake-ninja-build`),
the six focused `migration_smoke` CTest entries, the full 15-test public
migration CTest suite including tool roundtrips, forced tiny-cache fault
injection, `cmake --build @cmake-asan-build`, the six focused ASAN
`migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The paired
benchmark gate passed with forced/default ratios of `1.121` batch, `1.176`
crud, `0.992` iterate, `0.970` get, and `1.106` delete.

A later descriptor parking result cleanup extended `dxb_park_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by the
storage I/O and probe result types. Descriptor parking now reports validation
failures as unsubmitted/uncompleted, missing-descriptor parking as an
unsubmitted completed no-op, and actual `osal_fseek()` parking requests as
submitted with terminal completion. `env_open()` still ignores the parking
results, preserving the existing best-effort behavior while giving a future
async-capable storage backend a concrete lifecycle state for the parking
boundary. Verification passed `git diff --check`, source scans covering
`dxb_park_result_t`, `dxb_park_result()`, `dxb_park_error()`,
`dxb_park_noop()`, `dxb_park_completed()`, and every
`dxb_storage_park_*()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.117` batch, `1.165` crud, `0.969` iterate, `0.971`
get, and `1.090` delete.

A later data-file open result cleanup extended `dxb_open_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by the
other explicit storage result types. Data, dsync, and Windows overlapped
storage opens now report submitted/completed successful descriptor opens and
submitted/not-completed open failures while keeping descriptor-state fields for
data/meta/dsync/overlapped handles. Existing callers still unwrap only `.err`,
preserving public open/setup behavior while giving a future async-capable
backend a concrete lifecycle state for descriptor-open completion. Verification
passed `git diff --check`, source scans covering `dxb_open_result_t`,
`dxb_open_result()`, `dxb_open_completed()`,
`dxb_open_submitted_error()`, `dxb_open_from_rc()`, and every
`dxb_storage_open_*()` call site, the GNUmake `mdbx_migration_smoke` build
target, direct `mdbx_migration_smoke` default and forced tiny-cache runs, the
Ninja build (`cmake --build @cmake-ninja-build`), the six focused
`migration_smoke` CTest entries, the full 15-test public migration CTest suite
including tool roundtrips, forced tiny-cache fault injection, `cmake --build
@cmake-asan-build`, the six focused ASAN `migration_smoke` CTest entries, and
`mdbx_migration_bench_lazy`. The paired benchmark gate passed with
forced/default ratios of `1.091` batch, `1.149` crud, `0.872` iterate, `1.035`
get, and `1.074` delete.

A later descriptor close result cleanup extended `dxb_close_result_t` with
`submitted` and `completed` flags, matching the async-facing state used by the
other explicit storage result types. Descriptor teardown now reports
missing-handle closes as unsubmitted completed no-ops, reports actual
`osal_closefile()` attempts as submitted, and marks completion only when every
present data/dsync handle closed successfully. Existing callers still unwrap
only `.err`, `.had_data`, and the existing descriptor-state fields, preserving
environment close and POSIX lock-restore behavior while giving a future
async-capable backend a concrete lifecycle state for descriptor-close
completion. Verification passed `git diff --check`, source scans covering
`dxb_close_result_t`, `dxb_close_result()`, `dxb_close_with_reset()`,
`dxb_storage_close_handles()`, and `dxb_storage_close()` call sites, the
GNUmake `mdbx_migration_smoke` build target, direct `mdbx_migration_smoke`
default and forced tiny-cache runs, the Ninja build (`cmake --build
@cmake-ninja-build`), the six focused `migration_smoke` CTest entries, the
full 15-test public migration CTest suite including tool roundtrips, forced
tiny-cache fault injection, `cmake --build @cmake-asan-build`, the six focused
ASAN `migration_smoke` CTest entries, and `mdbx_migration_bench_lazy`. The
paired benchmark gate passed with forced/default ratios of `1.086` batch,
`1.155` crud, `1.262` iterate, `0.954` get, and `1.053` delete.

Use larger runs for final decisions; this reduced run is only a quick regression
smoke.

## Acceptance Criteria

The migration is not complete until all of the following are true:

- No data-file open/setup/resize/sync path requires `osal_mmap_t`, and
  `MDBX_env` has no data-file mmap field.
- Generic `pgno2page()` and mapped data-page helper fallbacks are removed, and
  normal page reads go through pinned `page_get_*()`/page-cache results.
- Meta page reads and writes use explicit buffers and explicit I/O.
- `MDBX_WRITEMAP` is rejected by a documented policy.
- Lock-file mmap remains working and unchanged for phase 1.
- Public API value lifetime behavior matches the existing API.
- Public gates pass, the full MDBX suite passes when available, and performance
  regressions are measured against the stored baseline.
