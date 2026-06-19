/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small public-API benchmark for async executor read throughput.
 *
 * This is intentionally not a pass/fail CTest. It gives a repeatable local
 * comparison between blocking serial reads, blocking pthread-parallel reads,
 * and reads submitted through multiple async executors. It covers both random
 * point gets and cursor-batch iteration.
 */

#include "mdbx.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ITEMS 20000u
#define DEFAULT_OPS 200000u
#define DEFAULT_WORKERS 4u
#define DEFAULT_WINDOW 64u
#define DEFAULT_CURSOR_BATCH_PAIRS 2048u

struct blocking_worker {
  MDBX_env *env;
  MDBX_dbi dbi;
  size_t items;
  size_t ops;
  size_t offset;
  int rc;
};

struct cursor_batch_worker {
  MDBX_env *env;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  size_t batch_pairs;
  int rc;
};

struct async_worker {
  MDBX_async *async;
  MDBX_txn *txn;
  MDBX_cursor *cursor;
  MDBX_async_op **ops;
  MDBX_val *data;
  MDBX_val *key_vals;
  MDBX_val *pairs;
  uint64_t *keys;
  size_t count;
  int *results;
  size_t pending;
  bool cursor_started;
};

static int fail_rc(const char *expr, int rc, const char *file, int line) {
  fprintf(stderr, "%s:%d: %s failed: (%d) %s\n", file, line, expr, rc, mdbx_strerror(rc));
  return rc ? rc : MDBX_PROBLEM;
}

static int fail_msg(const char *msg, const char *file, int line) {
  fprintf(stderr, "%s:%d: %s\n", file, line, msg);
  return MDBX_PROBLEM;
}

#define CHECK(expr)                                                                                                    \
  do {                                                                                                                 \
    rc = (expr);                                                                                                       \
    if (rc != MDBX_SUCCESS) {                                                                                          \
      rc = fail_rc(#expr, rc, __FILE__, __LINE__);                                                                     \
      goto bailout;                                                                                                    \
    }                                                                                                                  \
  } while (0)

static MDBX_val val(void *base, size_t len) {
  MDBX_val result;
  result.iov_base = base;
  result.iov_len = len;
  return result;
}

static uint64_t expected_value(uint64_t key) { return key * UINT64_C(17) + UINT64_C(11); }

static uint64_t key_for(size_t index, size_t items) {
  return (uint64_t)((index * UINT64_C(11400714819323198485) + UINT64_C(0x9E3779B9)) % items);
}

static uint64_t monotime_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static size_t env_size(const char *name, size_t fallback) {
  const char *const value = getenv(name);
  if (!value || !*value)
    return fallback;
  char *end = NULL;
  errno = 0;
  const unsigned long long parsed = strtoull(value, &end, 0);
  if (errno || end == value || *end || parsed == 0)
    return fallback;
  return (size_t)parsed;
}

static int expect_value(const MDBX_val *data, uint64_t key, const char *file, int line) {
  if (data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected value size", file, line);
  uint64_t actual = 0;
  memcpy(&actual, data->iov_base, sizeof(actual));
  if (actual != expected_value(key))
    return fail_msg("unexpected value payload", file, line);
  return MDBX_SUCCESS;
}

static int expect_cursor_batch(const MDBX_val *pairs, size_t count, size_t items, const char *file, int line) {
  if (count == 0 || (count & 1))
    return fail_msg("unexpected cursor batch count", file, line);
  for (size_t i = 0; i < count; i += 2) {
    if (pairs[i].iov_len != sizeof(uint64_t))
      return fail_msg("unexpected cursor batch key size", file, line);
    uint64_t actual_key = 0;
    memcpy(&actual_key, pairs[i].iov_base, sizeof(actual_key));
    if (actual_key >= items)
      return fail_msg("unexpected cursor batch key", file, line);
    int rc = expect_value(&pairs[i + 1], actual_key, file, line);
    if (rc != MDBX_SUCCESS)
      return rc;
  }
  return MDBX_SUCCESS;
}

static int wait_success(MDBX_async_op **op, int *operation_result, const char *file, int line) {
  if (!op || !*op)
    return fail_msg("missing async operation handle", file, line);
  int result = MDBX_SUCCESS;
  int rc = mdbx_async_wait(*op, &result);
  if (rc != MDBX_SUCCESS)
    return fail_rc("mdbx_async_wait", rc, file, line);
  rc = mdbx_async_op_release(*op);
  if (rc != MDBX_SUCCESS)
    return fail_rc("mdbx_async_op_release", rc, file, line);
  *op = NULL;
  if (operation_result) {
    *operation_result = result;
  } else if (result != MDBX_SUCCESS) {
    return fail_rc("async operation", result, file, line);
  }
  return MDBX_SUCCESS;
}

static int seed_database(MDBX_env *env, MDBX_dbi *dbi, size_t items) {
  MDBX_txn *txn = NULL;
  int rc;
  CHECK(mdbx_txn_begin(env, NULL, 0, &txn));
  CHECK(mdbx_dbi_open(txn, NULL, MDBX_DB_DEFAULTS, dbi));
  for (size_t i = 0; i < items; ++i) {
    uint64_t key_data = (uint64_t)i;
    uint64_t value_data = expected_value(key_data);
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(&value_data, sizeof(value_data));
    CHECK(mdbx_put(txn, *dbi, &key, &value, 0));
  }
  CHECK(mdbx_txn_commit(txn));
  return MDBX_SUCCESS;

bailout:
  if (txn)
    (void)mdbx_txn_abort(txn);
  return rc ? rc : MDBX_PROBLEM;
}

static int blocking_get_loop(MDBX_txn *txn, MDBX_dbi dbi, size_t items, size_t ops, size_t offset) {
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = key_for(offset + i, items);
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val data = val(NULL, 0);
    int rc = mdbx_get(txn, dbi, &key, &data);
    if (rc != MDBX_SUCCESS)
      return fail_rc("mdbx_get", rc, __FILE__, __LINE__);
    rc = expect_value(&data, key_data, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      return rc;
  }
  return MDBX_SUCCESS;
}

static double blocking_serial_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  const uint64_t start = monotime_ns();
  rc = blocking_get_loop(txn, dbi, items, ops, 0);
  const uint64_t finish = monotime_ns();
  const int abort_rc = mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || abort_rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static void *blocking_worker_main(void *arg) {
  struct blocking_worker *const worker = (struct blocking_worker *)arg;
  MDBX_txn *txn = NULL;
  worker->rc = mdbx_txn_begin(worker->env, NULL, MDBX_TXN_RDONLY, &txn);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = blocking_get_loop(txn, worker->dbi, worker->items, worker->ops, worker->offset);
  if (txn) {
    const int rc = mdbx_txn_abort(txn);
    if (worker->rc == MDBX_SUCCESS)
      worker->rc = rc;
  }
  return NULL;
}

static double blocking_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct blocking_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  int failed = 0;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].env = env;
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].ops = base_ops + (i < extra_ops);
    workers[i].offset = offset;
    offset += workers[i].ops;
    const int rc = pthread_create(&threads[i], NULL, blocking_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      workers_count = i;
      break;
    }
  }

  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
  }
  const uint64_t finish = monotime_ns();
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static int blocking_cursor_batch_loop(MDBX_cursor *cursor, size_t items, size_t target_pairs, size_t batch_pairs,
                                      size_t *completed_pairs) {
  MDBX_val *pairs = calloc(batch_pairs * 2, sizeof(*pairs));
  if (!pairs)
    return MDBX_ENOMEM;

  int rc = MDBX_SUCCESS;
  MDBX_cursor_op op = MDBX_FIRST;
  *completed_pairs = 0;
  while (*completed_pairs < target_pairs) {
    size_t count = 0;
    rc = mdbx_cursor_get_batch(cursor, &count, pairs, batch_pairs * 2, op);
    if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
      rc = fail_rc("mdbx_cursor_get_batch", rc, __FILE__, __LINE__);
      goto bailout;
    }
    const int operation_rc = rc;
    rc = expect_cursor_batch(pairs, count, items, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    *completed_pairs += count / 2;
    op = (operation_rc == MDBX_SUCCESS) ? MDBX_NEXT : MDBX_FIRST;
  }

bailout:
  free(pairs);
  return rc;
}

static double blocking_serial_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                           size_t batch_pairs) {
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  int rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  rc = mdbx_cursor_open(txn, dbi, &cursor);
  if (rc != MDBX_SUCCESS) {
    (void)mdbx_txn_abort(txn);
    return -1.0;
  }

  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  rc = blocking_cursor_batch_loop(cursor, items, target_pairs, batch_pairs, &completed_pairs);
  const uint64_t finish = monotime_ns();
  const int close_rc = mdbx_cursor_close2(cursor);
  const int abort_rc = mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || close_rc != MDBX_SUCCESS || abort_rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static void *cursor_batch_worker_main(void *arg) {
  struct cursor_batch_worker *const worker = (struct cursor_batch_worker *)arg;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  worker->completed_pairs = 0;
  worker->rc = mdbx_txn_begin(worker->env, NULL, MDBX_TXN_RDONLY, &txn);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = mdbx_cursor_open(txn, worker->dbi, &cursor);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = blocking_cursor_batch_loop(cursor, worker->items, worker->target_pairs, worker->batch_pairs,
                                            &worker->completed_pairs);
  if (cursor) {
    const int rc = mdbx_cursor_close2(cursor);
    if (worker->rc == MDBX_SUCCESS)
      worker->rc = rc;
  }
  if (txn) {
    const int rc = mdbx_txn_abort(txn);
    if (worker->rc == MDBX_SUCCESS)
      worker->rc = rc;
  }
  return NULL;
}

static double blocking_parallel_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                             size_t workers_count, size_t batch_pairs) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct cursor_batch_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  int failed = 0;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].env = env;
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].target_pairs = base_pairs + (i < extra_pairs);
    workers[i].batch_pairs = batch_pairs;
    const int rc = pthread_create(&threads[i], NULL, cursor_batch_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      workers_count = i;
      break;
    }
  }

  size_t completed_pairs = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    completed_pairs += workers[i].completed_pairs;
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
  }
  const uint64_t finish = monotime_ns();
  free(threads);
  free(workers);
  if (failed || completed_pairs == 0 || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static int async_worker_init(MDBX_env *env, struct async_worker *worker, MDBX_dbi dbi, size_t window) {
  int rc;
  MDBX_async_op *op = NULL;
  worker->ops = calloc(window, sizeof(*worker->ops));
  worker->data = calloc(window, sizeof(*worker->data));
  worker->key_vals = calloc(window, sizeof(*worker->key_vals));
  worker->keys = calloc(window, sizeof(*worker->keys));
  worker->results = calloc(window, sizeof(*worker->results));
  if (!worker->ops || !worker->data || !worker->key_vals || !worker->keys || !worker->results)
    return MDBX_ENOMEM;

  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &worker->async));
  CHECK(mdbx_async_txn_begin(worker->async, NULL, MDBX_TXN_RDONLY, &worker->txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_reset(worker->async, worker->txn, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_renew(worker->async, worker->txn, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  (void)dbi;
  return MDBX_SUCCESS;

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  return rc ? rc : MDBX_PROBLEM;
}

static void async_worker_destroy(struct async_worker *worker) {
  MDBX_async_op *op = NULL;
  if (worker->async && worker->cursor) {
    if (mdbx_async_cursor_close(worker->async, worker->cursor, &op) == MDBX_SUCCESS)
      (void)wait_success(&op, NULL, __FILE__, __LINE__);
    worker->cursor = NULL;
  }
  if (worker->async && worker->txn) {
    if (mdbx_async_txn_abort(worker->async, worker->txn, NULL, &op) == MDBX_SUCCESS)
      (void)wait_success(&op, NULL, __FILE__, __LINE__);
    worker->txn = NULL;
  }
  if (worker->async) {
    (void)mdbx_async_destroy(worker->async, true);
    worker->async = NULL;
  }
  free(worker->ops);
  free(worker->data);
  free(worker->key_vals);
  free(worker->pairs);
  free(worker->keys);
  free(worker->results);
}

static double async_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                 size_t window) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!workers)
    return -1.0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, window) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  size_t issued = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  while (issued < ops) {
    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      worker->pending = 0;
      while (worker->pending < window && issued < ops) {
        const size_t slot = worker->pending++;
        worker->keys[slot] = key_for(issued, items);
        worker->data[slot] = val(NULL, 0);
        MDBX_val key = val(&worker->keys[slot], sizeof(worker->keys[slot]));
        rc = mdbx_async_get(worker->async, worker->txn, dbi, &key, &worker->data[slot], &worker->ops[slot]);
        if (rc != MDBX_SUCCESS)
          goto bailout;
        issued += 1;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      rc = mdbx_async_wait_all(worker->ops, worker->pending, worker->results);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      rc = mdbx_async_op_release_all(worker->ops, worker->pending);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS) {
          rc = fail_rc("mdbx_async_get", operation_rc, __FILE__, __LINE__);
          goto bailout;
        }
        rc = expect_value(&worker->data[slot], worker->keys[slot], __FILE__, __LINE__);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
      worker->pending = 0;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(workers);
  if (finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    for (size_t slot = 0; slot < workers[i].pending; ++slot) {
      if (workers[i].ops[slot])
        (void)wait_success(&workers[i].ops[slot], NULL, __FILE__, __LINE__);
    }
    async_worker_destroy(&workers[i]);
  }
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                       size_t window) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!workers)
    return -1.0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, window) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  size_t issued = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  while (issued < ops) {
    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      worker->pending = 0;
      while (worker->pending < window && issued < ops) {
        const size_t slot = worker->pending++;
        worker->keys[slot] = key_for(issued, items);
        worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
        worker->data[slot] = val(NULL, 0);
        issued += 1;
      }
      if (worker->pending) {
        rc = mdbx_async_get_batch(worker->async, worker->txn, dbi, worker->key_vals, worker->data, worker->results,
                                  worker->pending, &worker->ops[0]);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      if (!worker->pending)
        continue;
      int batch_rc = MDBX_SUCCESS;
      rc = wait_success(&worker->ops[0], &batch_rc, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (batch_rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_get_batch", batch_rc, __FILE__, __LINE__);
        goto bailout;
      }
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS) {
          rc = fail_rc("mdbx_async_get_batch item", operation_rc, __FILE__, __LINE__);
          goto bailout;
        }
        rc = expect_value(&worker->data[slot], worker->keys[slot], __FILE__, __LINE__);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
      worker->pending = 0;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(workers);
  if (finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(workers);
  (void)rc;
  return -1.0;
}

static int async_cursor_batch_worker_init(MDBX_env *env, struct async_worker *worker, MDBX_dbi dbi,
                                          size_t batch_pairs) {
  int rc;
  MDBX_async_op *op = NULL;
  CHECK(async_worker_init(env, worker, dbi, 1));
  worker->pairs = calloc(batch_pairs * 2, sizeof(*worker->pairs));
  if (!worker->pairs) {
    rc = MDBX_ENOMEM;
    goto bailout;
  }
  CHECK(mdbx_async_cursor_open(worker->async, worker->txn, dbi, &worker->cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  worker->cursor_started = false;
  return MDBX_SUCCESS;

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  return rc ? rc : MDBX_PROBLEM;
}

static double async_parallel_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                          size_t workers_count, size_t batch_pairs) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!workers)
    return -1.0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_batch_worker_init(env, &workers[i], dbi, batch_pairs) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  size_t completed_pairs = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count && completed_pairs < target_pairs; ++i) {
      struct async_worker *const worker = &workers[i];
      worker->count = 0;
      rc = mdbx_async_cursor_get_batch(worker->async, worker->cursor, &worker->count, worker->pairs, batch_pairs * 2,
                                       worker->cursor_started ? MDBX_NEXT : MDBX_FIRST, &worker->ops[0]);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      worker->pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      if (!worker->pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&worker->ops[0], &operation_rc, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_SUCCESS && operation_rc != MDBX_RESULT_TRUE) {
        rc = fail_rc("mdbx_async_cursor_get_batch", operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_cursor_batch(worker->pairs, worker->count, items, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      completed_pairs += worker->count / 2;
      worker->cursor_started = operation_rc == MDBX_SUCCESS;
      worker->pending = 0;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(workers);
  if (completed_pairs == 0 || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(workers);
  (void)rc;
  return -1.0;
}

static void print_rate(const char *label, double rate) {
  if (rate <= 0.0)
    printf("%-28s failed\n", label);
  else if (rate >= 1000000.0)
    printf("%-28s %8.3f Mops/s\n", label, rate / 1000000.0);
  else if (rate >= 1000.0)
    printf("%-28s %8.3f Kops/s\n", label, rate / 1000.0);
  else
    printf("%-28s %8.3f ops/s\n", label, rate);
}

int main(void) {
  char path[128];
  MDBX_env *env = NULL;
  MDBX_dbi dbi = 0;
  int rc = MDBX_SUCCESS;
  const size_t items = env_size("MDBX_ASYNC_BENCH_ITEMS", DEFAULT_ITEMS);
  const size_t ops = env_size("MDBX_ASYNC_BENCH_OPS", DEFAULT_OPS);
  const size_t workers = env_size("MDBX_ASYNC_BENCH_WORKERS", DEFAULT_WORKERS);
  const size_t window = env_size("MDBX_ASYNC_BENCH_WINDOW", DEFAULT_WINDOW);
  size_t cursor_batch_pairs = env_size("MDBX_ASYNC_BENCH_CURSOR_BATCH_PAIRS", DEFAULT_CURSOR_BATCH_PAIRS);
  if (cursor_batch_pairs < 2)
    cursor_batch_pairs = 2;
  const size_t cursor_pairs = ops < cursor_batch_pairs ? cursor_batch_pairs : ops;

  snprintf(path, sizeof(path), "./async-api-bench-%ld", (long)getpid());
  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
    rc = fail_rc("mdbx_env_delete", rc, __FILE__, __LINE__);
    goto bailout;
  }

  CHECK(mdbx_env_create(&env));
  CHECK(mdbx_env_set_geometry(env, -1, -1, (intptr_t)(1024 * 1024 * 1024), -1, -1, -1));
  CHECK(mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM | MDBX_SAFE_NOSYNC | MDBX_NOMETASYNC, 0664));
  CHECK(seed_database(env, &dbi, items));

  printf("async-api-bench items=%zu ops=%zu workers=%zu window=%zu cursor-batch-pairs=%zu\n", items, ops, workers,
         window, cursor_batch_pairs);
  const double blocking_serial = blocking_serial_get(env, dbi, items, ops);
  const double blocking_parallel = blocking_parallel_get(env, dbi, items, ops, workers);
  const double async_parallel = async_parallel_get(env, dbi, items, ops, workers, window);
  const double async_batch_parallel = async_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double blocking_cursor_serial = blocking_serial_cursor_batch(env, dbi, items, cursor_pairs, cursor_batch_pairs);
  const double blocking_cursor_parallel =
      blocking_parallel_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_cursor_parallel =
      async_parallel_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  print_rate("blocking serial get", blocking_serial);
  print_rate("blocking parallel get", blocking_parallel);
  print_rate("async parallel get", async_parallel);
  print_rate("async batch parallel get", async_batch_parallel);
  print_rate("blocking cursor batch", blocking_cursor_serial);
  print_rate("parallel cursor batch", blocking_cursor_parallel);
  print_rate("async cursor batch", async_cursor_parallel);
  if (blocking_parallel > 0.0 && async_parallel > 0.0)
    printf("%-28s %8.3f\n", "async/blocking parallel", async_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch/blocking parallel", async_batch_parallel / blocking_parallel);
  if (blocking_serial > 0.0 && async_parallel > 0.0)
    printf("%-28s %8.3f\n", "async/blocking serial", async_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch/blocking serial", async_batch_parallel / blocking_serial);
  if (blocking_cursor_parallel > 0.0 && async_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor/blocking par", async_cursor_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor/blocking ser", async_cursor_parallel / blocking_cursor_serial);

  CHECK(mdbx_env_close(env));
  env = NULL;
  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc == MDBX_RESULT_TRUE)
    rc = MDBX_SUCCESS;
  if (rc != MDBX_SUCCESS)
    rc = fail_rc("mdbx_env_delete cleanup", rc, __FILE__, __LINE__);
  return rc;

bailout:
  if (env)
    (void)mdbx_env_close(env);
  (void)mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  return rc ? rc : MDBX_PROBLEM;
}
