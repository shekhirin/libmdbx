/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small public-API benchmark for async executor throughput.
 *
 * This is intentionally not a pass/fail CTest. It gives a repeatable local
 * comparison between blocking serial reads, blocking pthread-parallel reads,
 * and reads submitted through multiple async executors. It covers random point
 * gets, cursor-batch iteration, and bounded write-transaction put workloads.
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
#define DEFAULT_WRITE_OPS 20000u
#define DEFAULT_WRITE_BATCH 1024u
#define DEFAULT_WORKERS 4u
#define DEFAULT_WINDOW 64u
#define DEFAULT_CURSOR_BATCH_PAIRS 2048u
#define DEFAULT_LARGE_ITEMS 256u
#define DEFAULT_LARGE_OPS 30000u
#define DEFAULT_LARGE_VALUE_BYTES 10000u

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
  size_t value_bytes;
  bool large_values;
  int rc;
};

struct cursor_get_worker {
  MDBX_env *env;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  int rc;
};

struct cursor_scan_worker {
  MDBX_env *env;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  bool scan_from;
  int rc;
};

struct cursor_scan_check {
  size_t items;
  size_t target_pairs;
  size_t checked_pairs;
};

struct async_cursor_batch_check {
  size_t items;
  size_t value_bytes;
  bool large_values;
};

struct async_cursor_get_loop_check {
  size_t items;
  size_t checked_pairs;
};

struct async_get_batch_check {
  size_t checked;
};

struct async_get_loop_check {
  size_t items;
  size_t offset;
  size_t checked;
  uint64_t key;
};

struct async_put_loop_check {
  size_t items;
  size_t checked;
  uint64_t key;
  uint64_t value;
};

struct async_del_loop_check {
  size_t checked;
  uint64_t key;
};

struct async_replace_loop_check {
  size_t checked;
  uint64_t key;
  uint64_t value;
  uint64_t old_value;
};

struct async_replace_delete_loop_check {
  size_t checked;
  uint64_t key;
  uint64_t old_value;
};

struct async_worker {
  MDBX_async *async;
  MDBX_txn *txn;
  MDBX_cursor *cursor;
  MDBX_async_op **ops;
  MDBX_val *data;
  MDBX_val *key_vals;
  MDBX_val *pairs;
  MDBX_cache_entry_t *cache_entries;
  MDBX_cache_result_t *cache_results;
  uint64_t *keys;
  size_t *value_counts;
  size_t count;
  int *results;
  size_t pending;
  bool cursor_started;
};

struct async_thread_worker {
  struct async_worker worker;
  MDBX_dbi dbi;
  size_t items;
  size_t ops;
  size_t offset;
  size_t window;
  int rc;
  bool batch;
  bool many;
};

enum async_thread_loop_kind {
  async_thread_loop_get,
  async_thread_loop_get_ex,
  async_thread_loop_lowerbound
};

struct async_thread_loop_worker {
  struct async_worker worker;
  struct async_get_loop_check check;
  MDBX_dbi dbi;
  size_t items;
  size_t ops;
  size_t offset;
  size_t completed;
  int rc;
  enum async_thread_loop_kind kind;
};

struct async_thread_cache_batch_worker {
  struct async_worker worker;
  MDBX_dbi dbi;
  size_t items;
  size_t ops;
  size_t offset;
  size_t window;
  int rc;
  bool singlethreaded;
};

struct async_thread_cache_loop_worker {
  struct async_worker worker;
  struct async_get_loop_check check;
  MDBX_dbi dbi;
  size_t ops;
  size_t completed;
  int rc;
  bool singlethreaded;
};

struct async_thread_cursor_get_loop_worker {
  struct async_worker worker;
  struct async_cursor_get_loop_check check;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  int rc;
  bool loop_from;
};

struct async_thread_cursor_batch_loop_worker {
  struct async_worker worker;
  struct async_cursor_batch_check check;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  size_t batch_pairs;
  int rc;
  bool loop_from;
};

struct async_thread_cursor_batch_worker {
  struct async_worker worker;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  size_t batch_pairs;
  size_t value_bytes;
  bool large_values;
  int rc;
};

struct async_thread_cursor_scan_worker {
  struct async_worker worker;
  struct cursor_scan_check check;
  MDBX_dbi dbi;
  size_t items;
  size_t target_pairs;
  size_t completed_pairs;
  bool scan_from;
  int rc;
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

static void fill_large_value(uint8_t *bytes, size_t len, uint64_t key) {
  for (size_t i = 0; i < len; ++i)
    bytes[i] = (uint8_t)(key + i * 31u + i / 7u);
}

static uint64_t write_value(uint64_t key, size_t index, uint64_t salt) {
  return key * UINT64_C(31) + (uint64_t)index * UINT64_C(7) + salt;
}

static int preserve_value_copy(void *context, MDBX_val *target, const void *src, size_t bytes) {
  (void)context;
  if (!target || !src)
    return MDBX_EINVAL;
  if (target->iov_len < bytes) {
    target->iov_base = NULL;
    target->iov_len = bytes;
    return MDBX_RESULT_TRUE;
  }
  memcpy(target->iov_base, src, bytes);
  target->iov_len = bytes;
  return MDBX_SUCCESS;
}

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

static int expect_large_value(const MDBX_val *data, uint64_t key, size_t value_bytes,
                              const char *file, int line) {
  if (!data || !data->iov_base || data->iov_len != value_bytes)
    return fail_msg("unexpected large value size", file, line);
  const uint8_t *const bytes = (const uint8_t *)data->iov_base;
  for (size_t i = 0; i < data->iov_len; ++i) {
    const uint8_t expected = (uint8_t)(key + i * 31u + i / 7u);
    if (bytes[i] != expected)
      return fail_msg("unexpected large value payload", file, line);
  }
  return MDBX_SUCCESS;
}

static int async_get_batch_check_func(void *context, const MDBX_val keys[], MDBX_val data[], const int results[],
                                      size_t count) {
  struct async_get_batch_check *const check = (struct async_get_batch_check *)context;
  if (!check || !keys || !data || !results)
    return fail_msg("missing async get batch callback state", __FILE__, __LINE__);
  for (size_t i = 0; i < count; ++i) {
    if (results[i] != MDBX_SUCCESS)
      return fail_rc("mdbx_async_get_batch_cb item", results[i], __FILE__, __LINE__);
    if (keys[i].iov_len != sizeof(uint64_t))
      return fail_msg("unexpected async get batch callback key size", __FILE__, __LINE__);
    uint64_t key = 0;
    memcpy(&key, keys[i].iov_base, sizeof(key));
    const int rc = expect_value(&data[i], key, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      return rc;
    check->checked += 1;
  }
  return MDBX_SUCCESS;
}

static int async_get_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct async_get_loop_check *const check = (struct async_get_loop_check *)context;
  if (!check || !key)
    return fail_msg("missing async get loop key state", __FILE__, __LINE__);
  check->key = key_for(check->offset + index, check->items);
  *key = val(&check->key, sizeof(check->key));
  return MDBX_SUCCESS;
}

static int async_get_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                      int result) {
  struct async_get_loop_check *const check = (struct async_get_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async get loop result state", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_get_loop item", result, __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected async get loop key size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  const uint64_t expected_key = key_for(check->offset + index, check->items);
  if (actual_key != expected_key)
    return fail_msg("unexpected async get loop key payload", __FILE__, __LINE__);
  const int rc = expect_value(data, expected_key, __FILE__, __LINE__);
  if (rc != MDBX_SUCCESS)
    return rc;
  check->checked += 1;
  return MDBX_SUCCESS;
}

static int async_get_ex_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                         size_t values_count, int result) {
  struct async_get_loop_check *const check = (struct async_get_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async get_ex loop result state", __FILE__, __LINE__);
  if (values_count != 1)
    return fail_msg("unexpected async get_ex loop value count", __FILE__, __LINE__);
  return async_get_loop_result_func(context, index, key, data, result);
}

static int async_lowerbound_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                            const MDBX_val *data, int result) {
  if (result != MDBX_SUCCESS && result != MDBX_RESULT_TRUE)
    return fail_rc("mdbx_async_get_equal_or_great_loop item", result, __FILE__, __LINE__);
  return async_get_loop_result_func(context, index, key, data, MDBX_SUCCESS);
}

static int async_cache_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                        MDBX_cache_result_t result) {
  if (result.errcode != MDBX_SUCCESS)
    return fail_rc("mdbx_async_cache_get_loop item", result.errcode, __FILE__, __LINE__);
  return async_get_loop_result_func(context, index, key, data, MDBX_SUCCESS);
}

static int async_cache_batch_check_func(void *context, const MDBX_val keys[], MDBX_val data[],
                                        const MDBX_cache_result_t results[], size_t count) {
  struct async_get_batch_check *const check = (struct async_get_batch_check *)context;
  if (!check || !keys || !data || !results)
    return fail_msg("missing async cache batch callback state", __FILE__, __LINE__);
  for (size_t i = 0; i < count; ++i) {
    if (results[i].errcode != MDBX_SUCCESS)
      return fail_rc("mdbx_async_cache_get_batch_cb item", results[i].errcode, __FILE__, __LINE__);
    if (keys[i].iov_len != sizeof(uint64_t))
      return fail_msg("unexpected async cache batch callback key size", __FILE__, __LINE__);
    uint64_t key = 0;
    memcpy(&key, keys[i].iov_base, sizeof(key));
    const int rc = expect_value(&data[i], key, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      return rc;
    check->checked += 1;
  }
  return MDBX_SUCCESS;
}

static int async_put_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *data) {
  struct async_put_loop_check *const check = (struct async_put_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async put loop item state", __FILE__, __LINE__);
  check->key = key_for(index, check->items);
  check->value = write_value(check->key, index, UINT64_C(1001));
  *key = val(&check->key, sizeof(check->key));
  *data = val(&check->value, sizeof(check->value));
  return MDBX_SUCCESS;
}

static int async_put_loop_result_func(void *context, size_t index, const MDBX_val *key, MDBX_val *data,
                                      int result) {
  struct async_put_loop_check *const check = (struct async_put_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async put loop result state", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_put_loop item", result, __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected async put loop value size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  const uint64_t expected_key = key_for(index, check->items);
  if (actual_key != expected_key || actual_value != write_value(expected_key, index, UINT64_C(1001)))
    return fail_msg("unexpected async put loop payload", __FILE__, __LINE__);
  check->checked += 1;
  return MDBX_SUCCESS;
}

static int async_del_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct async_del_loop_check *const check = (struct async_del_loop_check *)context;
  if (!check || !key)
    return fail_msg("missing async delete loop key state", __FILE__, __LINE__);
  check->key = (uint64_t)index;
  *key = val(&check->key, sizeof(check->key));
  return MDBX_SUCCESS;
}

static int async_del_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                      bool has_data, int result) {
  struct async_del_loop_check *const check = (struct async_del_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async delete loop result state", __FILE__, __LINE__);
  if (has_data)
    return fail_msg("unexpected async delete loop data flag", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_del_loop item", result, __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t) || data->iov_base || data->iov_len)
    return fail_msg("unexpected async delete loop value shape", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index)
    return fail_msg("unexpected async delete loop key payload", __FILE__, __LINE__);
  check->checked += 1;
  return MDBX_SUCCESS;
}

static int async_replace_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *new_data,
                                        MDBX_val *old_data) {
  struct async_replace_loop_check *const check = (struct async_replace_loop_check *)context;
  if (!check || !key || !new_data || !old_data)
    return fail_msg("missing async replace loop item state", __FILE__, __LINE__);
  check->key = (uint64_t)index;
  check->value = write_value(check->key, index, UINT64_C(1101));
  check->old_value = 0;
  *key = val(&check->key, sizeof(check->key));
  *new_data = val(&check->value, sizeof(check->value));
  *old_data = val(&check->old_value, sizeof(check->old_value));
  return MDBX_SUCCESS;
}

static int async_replace_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                          const MDBX_val *new_data, MDBX_val *old_data, int result) {
  struct async_replace_loop_check *const check = (struct async_replace_loop_check *)context;
  if (!check || !key || !new_data || !old_data)
    return fail_msg("missing async replace loop result state", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_replace_loop item", result, __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t) || new_data->iov_len != sizeof(uint64_t) ||
      old_data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected async replace loop value size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  uint64_t actual_new = 0;
  uint64_t actual_old = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_new, new_data->iov_base, sizeof(actual_new));
  memcpy(&actual_old, old_data->iov_base, sizeof(actual_old));
  if (actual_key != index || actual_new != write_value(actual_key, index, UINT64_C(1101)) ||
      actual_old != expected_value(actual_key))
    return fail_msg("unexpected async replace loop payload", __FILE__, __LINE__);
  check->checked += 1;
  return MDBX_SUCCESS;
}

static int async_replace_delete_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *old_data) {
  struct async_replace_delete_loop_check *const check = (struct async_replace_delete_loop_check *)context;
  if (!check || !key || !old_data)
    return fail_msg("missing async replace delete loop item state", __FILE__, __LINE__);
  check->key = (uint64_t)index;
  check->old_value = 0;
  *key = val(&check->key, sizeof(check->key));
  *old_data = val(&check->old_value, sizeof(check->old_value));
  return MDBX_SUCCESS;
}

static int async_replace_delete_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                                 MDBX_val *old_data, int result) {
  struct async_replace_delete_loop_check *const check = (struct async_replace_delete_loop_check *)context;
  if (!check || !key || !old_data)
    return fail_msg("missing async replace delete loop result state", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_replace_delete_loop item", result, __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t) || old_data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected async replace delete loop value size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  uint64_t actual_old = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_old, old_data->iov_base, sizeof(actual_old));
  if (actual_key != index || actual_old != expected_value(actual_key))
    return fail_msg("unexpected async replace delete loop payload", __FILE__, __LINE__);
  check->checked += 1;
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

static int expect_cursor_large_batch(const MDBX_val *pairs, size_t count, size_t items,
                                     size_t value_bytes, const char *file, int line) {
  if (count == 0 || (count & 1))
    return fail_msg("unexpected large cursor batch count", file, line);
  for (size_t i = 0; i < count; i += 2) {
    if (pairs[i].iov_len != sizeof(uint64_t))
      return fail_msg("unexpected large cursor batch key size", file, line);
    uint64_t actual_key = 0;
    memcpy(&actual_key, pairs[i].iov_base, sizeof(actual_key));
    if (actual_key >= items)
      return fail_msg("unexpected large cursor batch key", file, line);
    int rc = expect_large_value(&pairs[i + 1], actual_key, value_bytes, file, line);
    if (rc != MDBX_SUCCESS)
      return rc;
  }
  return MDBX_SUCCESS;
}

static int cursor_scan_check_func(void *context, MDBX_val *key, MDBX_val *value, void *arg) {
  (void)arg;
  struct cursor_scan_check *const check = (struct cursor_scan_check *)context;
  if (!check || !key || !value)
    return fail_msg("missing cursor scan predicate state", __FILE__, __LINE__);
  if (check->checked_pairs >= check->target_pairs)
    return MDBX_RESULT_TRUE;
  if (key->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected cursor scan key size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key >= check->items)
    return fail_msg("unexpected cursor scan key", __FILE__, __LINE__);
  int rc = expect_value(value, actual_key, __FILE__, __LINE__);
  if (rc != MDBX_SUCCESS)
    return rc;
  check->checked_pairs += 1;
  return check->checked_pairs >= check->target_pairs ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

static int async_cursor_batch_check_func(void *context, const MDBX_val *pairs, size_t count) {
  const struct async_cursor_batch_check *const check = (const struct async_cursor_batch_check *)context;
  if (check->large_values)
    return expect_cursor_large_batch(pairs, count, check->items, check->value_bytes,
                                     __FILE__, __LINE__);
  return expect_cursor_batch(pairs, count, check->items, __FILE__, __LINE__);
}

static int async_cursor_get_loop_check_func(void *context, size_t index, const MDBX_val *key,
                                            const MDBX_val *data) {
  (void)index;
  struct async_cursor_get_loop_check *const check = (struct async_cursor_get_loop_check *)context;
  if (!check || !key || !data)
    return fail_msg("missing async cursor get loop state", __FILE__, __LINE__);
  if (key->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected async cursor get loop key size", __FILE__, __LINE__);
  uint64_t actual_key = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key >= check->items)
    return fail_msg("unexpected async cursor get loop key", __FILE__, __LINE__);
  int rc = expect_value(data, actual_key, __FILE__, __LINE__);
  if (rc != MDBX_SUCCESS)
    return rc;
  check->checked_pairs += 1;
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

static int seed_large_database(MDBX_env *env, MDBX_dbi *dbi, size_t items, size_t value_bytes) {
  MDBX_txn *txn = NULL;
  uint8_t *value_data = NULL;
  int rc;
  if (!items || !value_bytes)
    return MDBX_EINVAL;
  value_data = malloc(value_bytes);
  if (!value_data)
    return MDBX_ENOMEM;
  CHECK(mdbx_txn_begin(env, NULL, 0, &txn));
  CHECK(mdbx_dbi_open(txn, "async-large-cache-bench", MDBX_CREATE, dbi));
  CHECK(mdbx_drop(txn, *dbi, false));
  for (size_t i = 0; i < items; ++i) {
    uint64_t key_data = (uint64_t)i;
    fill_large_value(value_data, value_bytes, key_data);
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(value_data, value_bytes);
    CHECK(mdbx_put(txn, *dbi, &key, &value, 0));
  }
  CHECK(mdbx_txn_commit(txn));
  free(value_data);
  return MDBX_SUCCESS;

bailout:
  if (txn)
    (void)mdbx_txn_abort(txn);
  free(value_data);
  return rc ? rc : MDBX_PROBLEM;
}

static int drop_database(MDBX_env *env, MDBX_dbi dbi) {
  MDBX_txn *txn = NULL;
  int rc;
  CHECK(mdbx_txn_begin(env, NULL, 0, &txn));
  CHECK(mdbx_drop(txn, dbi, true));
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

static double blocking_write_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = key_for(i, items);
    uint64_t value_data = write_value(key_data, i, UINT64_C(101));
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(&value_data, sizeof(value_data));
    rc = mdbx_put(txn, dbi, &key, &value, 0);
    if (rc != MDBX_SUCCESS)
      break;
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  values = calloc(window, sizeof(*values));
  key_data = calloc(window, sizeof(*key_data));
  value_data = calloc(window, sizeof(*value_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !values || !key_data || !value_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = key_for(offset + i, items);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(202));
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      rc = mdbx_async_put(async, txn, dbi, &keys[i], &values[i], 0, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_put", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_put item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += submitted;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(value_data);
  free(key_data);
  free(values);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_batch_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  values = calloc(batch, sizeof(*values));
  key_data = calloc(batch, sizeof(*key_data));
  value_data = calloc(batch, sizeof(*value_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !values || !key_data || !value_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = key_for(offset + i, items);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(303));
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_put_batch(async, txn, dbi, keys, values, results, chunk, 0, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_put_batch item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += chunk;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(value_data);
  free(key_data);
  free(values);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_loop_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  struct async_put_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  check.items = items;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_put_loop(async, txn, dbi, ops, async_put_loop_item_func, async_put_loop_result_func,
                            &check, &completed, 0, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_put_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async put loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_cursor_write_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  rc = mdbx_cursor_open(txn, dbi, &cursor);
  if (rc != MDBX_SUCCESS) {
    (void)mdbx_txn_abort(txn);
    return -1.0;
  }

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = key_for(i, items);
    uint64_t value_data = write_value(key_data, i, UINT64_C(707));
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(&value_data, sizeof(value_data));
    rc = mdbx_cursor_put(cursor, &key, &value, 0);
    if (rc != MDBX_SUCCESS)
      break;
  }
  mdbx_cursor_close(cursor);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_cursor_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  values = calloc(window, sizeof(*values));
  key_data = calloc(window, sizeof(*key_data));
  value_data = calloc(window, sizeof(*value_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !values || !key_data || !value_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = key_for(offset + i, items);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(737));
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      rc = mdbx_async_cursor_put(async, cursor, &keys[i], &values[i], 0, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_put", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_put item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += submitted;
  }

  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(value_data);
  free(key_data);
  free(values);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_batch_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  values = calloc(batch, sizeof(*values));
  key_data = calloc(batch, sizeof(*key_data));
  value_data = calloc(batch, sizeof(*value_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !values || !key_data || !value_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = key_for(offset + i, items);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(808));
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_cursor_put_batch(async, cursor, keys, values, results, chunk, 0, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_cursor_put_batch", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_put_batch item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += chunk;
  }

  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(value_data);
  free(key_data);
  free(values);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_loop_put(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  struct async_put_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  check.items = items;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_cursor_put_loop(async, cursor, ops, async_put_loop_item_func, async_put_loop_result_func,
                                   &check, &completed, 0, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_cursor_put_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async cursor put loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = (uint64_t)i;
    MDBX_val key = val(&key_data, sizeof(key_data));
    rc = mdbx_del(txn, dbi, &key, NULL);
    if (rc != MDBX_SUCCESS)
      break;
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  uint64_t *key_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  key_data = calloc(window, sizeof(*key_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !key_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      rc = mdbx_async_del(async, txn, dbi, &keys[i], NULL, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_del", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_del item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += submitted;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(key_data);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_batch_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  uint64_t *key_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  key_data = calloc(batch, sizeof(*key_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !key_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_del_batch(async, txn, dbi, keys, NULL, results, chunk, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_del_batch", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_del_batch item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += chunk;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(key_data);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_loop_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  struct async_del_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_del_loop(async, txn, dbi, ops, async_del_loop_key_func, NULL, async_del_loop_result_func,
                            &check, &completed, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_del_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async delete loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_cursor_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  CHECK(mdbx_cursor_open(txn, dbi, &cursor));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST));

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    CHECK(mdbx_cursor_del(cursor, MDBX_CURRENT));
    if (i + 1 < ops) {
      key = val(NULL, 0);
      data = val(NULL, 0);
      CHECK(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_CURRENT));
    }
  }
  const uint64_t finish = monotime_ns();
  mdbx_cursor_close(cursor);
  cursor = NULL;
  CHECK(mdbx_txn_commit(txn));
  txn = NULL;
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    (void)mdbx_txn_abort(txn);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &key, &data, MDBX_FIRST, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    int operation_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_cursor_del(async, cursor, MDBX_CURRENT, &op));
    CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
    if (operation_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_cursor_del", operation_rc, __FILE__, __LINE__);
      goto bailout;
    }
    if (i + 1 < ops) {
      key = val(NULL, 0);
      data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &key, &data, MDBX_GET_CURRENT, &op));
      CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
    }
  }
  const uint64_t finish = monotime_ns();
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor && async) {
    MDBX_async_op *close_op = NULL;
    if (mdbx_async_cursor_close(async, cursor, &close_op) == MDBX_SUCCESS)
      (void)wait_success(&close_op, NULL, __FILE__, __LINE__);
  }
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_delete_loop(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &key, &data, MDBX_FIRST, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_cursor_del_loop(async, cursor, ops, MDBX_CURRENT, &completed, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_cursor_del_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops) {
    rc = fail_msg("unexpected async cursor delete loop count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  if (finish > start)
    rate = (double)completed * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor && async) {
    MDBX_async_op *close_op = NULL;
    if (mdbx_async_cursor_close(async, cursor, &close_op) == MDBX_SUCCESS)
      (void)wait_success(&close_op, NULL, __FILE__, __LINE__);
  }
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_cursor_range_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  MDBX_cursor *end = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  CHECK(mdbx_cursor_open(txn, dbi, &end));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_cursor_get(end, &key, &data, MDBX_FIRST));
  for (size_t i = 1; i < ops; ++i) {
    key = val(NULL, 0);
    data = val(NULL, 0);
    CHECK(mdbx_cursor_get(end, &key, &data, MDBX_NEXT));
  }

  uint64_t affected = 0;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_cursor_delete_range(NULL, end, true, &affected));
  const uint64_t finish = monotime_ns();
  if (affected != ops) {
    rc = fail_msg("unexpected blocking cursor range delete count", __FILE__, __LINE__);
    goto bailout;
  }
  mdbx_cursor_close(end);
  end = NULL;
  CHECK(mdbx_txn_commit(txn));
  txn = NULL;
  if (finish > start)
    rate = (double)affected * 1000000000.0 / (double)(finish - start);

bailout:
  if (end)
    mdbx_cursor_close(end);
  if (txn)
    (void)mdbx_txn_abort(txn);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_range_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *end = NULL;
  MDBX_async_op *op = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &end, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, end, &key, &data, MDBX_FIRST, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  for (size_t i = 1; i < ops; ++i) {
    key = val(NULL, 0);
    data = val(NULL, 0);
    CHECK(mdbx_async_cursor_get(async, end, &key, &data, MDBX_NEXT, &op));
    CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  }

  uint64_t affected = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_cursor_delete_range(async, NULL, end, true, &affected, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_cursor_delete_range", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (affected != ops) {
    rc = fail_msg("unexpected async cursor range delete count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_cursor_close(async, end, &op));
  end = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  if (finish > start)
    rate = (double)affected * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (end && async) {
    MDBX_async_op *close_op = NULL;
    if (mdbx_async_cursor_close(async, end, &close_op) == MDBX_SUCCESS)
      (void)wait_success(&close_op, NULL, __FILE__, __LINE__);
  }
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_cursor_bunch_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;
  CHECK(mdbx_cursor_open(txn, dbi, &cursor));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST));

  uint64_t affected = 0;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_cursor_bunch_delete(cursor, MDBX_DELETE_AFTER_INCLUDING, &affected));
  const uint64_t finish = monotime_ns();
  if (!affected) {
    rc = fail_msg("blocking cursor bunch delete affected no records", __FILE__, __LINE__);
    goto bailout;
  }
  mdbx_cursor_close(cursor);
  cursor = NULL;
  CHECK(mdbx_txn_commit(txn));
  txn = NULL;
  if (finish > start)
    rate = (double)affected * 1000000000.0 / (double)(finish - start);

bailout:
  if (cursor)
    mdbx_cursor_close(cursor);
  if (txn)
    (void)mdbx_txn_abort(txn);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_cursor_bunch_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  MDBX_val key = val(NULL, 0);
  MDBX_val data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &key, &data, MDBX_FIRST, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  uint64_t affected = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_cursor_bunch_delete(async, cursor, MDBX_DELETE_AFTER_INCLUDING, &affected, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_cursor_bunch_delete", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (!affected) {
    rc = fail_msg("async cursor bunch delete affected no records", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  cursor = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  if (finish > start)
    rate = (double)affected * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (cursor && async) {
    MDBX_async_op *close_op = NULL;
    if (mdbx_async_cursor_close(async, cursor, &close_op) == MDBX_SUCCESS)
      (void)wait_success(&close_op, NULL, __FILE__, __LINE__);
  }
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_replace(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = (uint64_t)i;
    uint64_t value_data = write_value(key_data, i, UINT64_C(404));
    uint64_t old_data_buffer = 0;
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(&value_data, sizeof(value_data));
    MDBX_val old_data = val(&old_data_buffer, sizeof(old_data_buffer));
    rc = mdbx_replace(txn, dbi, &key, &value, &old_data, 0);
    if (rc != MDBX_SUCCESS)
      break;
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_replace(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  values = calloc(window, sizeof(*values));
  old_values = calloc(window, sizeof(*old_values));
  key_data = calloc(window, sizeof(*key_data));
  value_data = calloc(window, sizeof(*value_data));
  old_data = calloc(window, sizeof(*old_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !values || !old_values || !key_data || !value_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(505));
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      rc = mdbx_async_replace(async, txn, dbi, &keys[i], &values[i], &old_values[i], 0, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += submitted;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(value_data);
  free(key_data);
  free(old_values);
  free(values);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_batch_replace(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  values = calloc(batch, sizeof(*values));
  old_values = calloc(batch, sizeof(*old_values));
  key_data = calloc(batch, sizeof(*key_data));
  value_data = calloc(batch, sizeof(*value_data));
  old_data = calloc(batch, sizeof(*old_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !values || !old_values || !key_data || !value_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(606));
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_replace_batch(async, txn, dbi, keys, values, old_values, results, chunk, 0, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_batch", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace_batch item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += chunk;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(value_data);
  free(key_data);
  free(old_values);
  free(values);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_loop_replace(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  struct async_replace_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_replace_loop(async, txn, dbi, ops, async_replace_loop_item_func,
                                async_replace_loop_result_func, &check, &completed, 0, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_replace_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async replace loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_replace_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = (uint64_t)i;
    uint64_t old_data_buffer = 0;
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val old_data = val(&old_data_buffer, sizeof(old_data_buffer));
    rc = mdbx_replace(txn, dbi, &key, NULL, &old_data, MDBX_CURRENT);
    if (rc != MDBX_SUCCESS)
      break;
    rc = expect_value(&old_data, key_data, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      break;
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_replace_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  old_values = calloc(window, sizeof(*old_values));
  key_data = calloc(window, sizeof(*key_data));
  old_data = calloc(window, sizeof(*old_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !old_values || !key_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      rc = mdbx_async_replace(async, txn, dbi, &keys[i], NULL, &old_values[i], MDBX_CURRENT, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace delete", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace delete item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_value(&old_values[i], key_data[i], __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }
    offset += submitted;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(key_data);
  free(old_values);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_batch_replace_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  old_values = calloc(batch, sizeof(*old_values));
  key_data = calloc(batch, sizeof(*key_data));
  old_data = calloc(batch, sizeof(*old_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !old_values || !key_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_replace_batch(async, txn, dbi, keys, NULL, old_values, results, chunk, MDBX_CURRENT, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_batch delete", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace_batch delete item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_value(&old_values[i], key_data[i], __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }
    offset += chunk;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(key_data);
  free(old_values);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_loop_replace_delete(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  struct async_replace_delete_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_replace_delete_loop(async, txn, dbi, ops, async_replace_delete_loop_item_func,
                                       async_replace_delete_loop_result_func, &check, &completed, MDBX_CURRENT, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_replace_delete_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async replace delete loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double blocking_replace_ex(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc != MDBX_SUCCESS)
    return -1.0;

  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    uint64_t key_data = (uint64_t)i;
    uint64_t value_data = write_value(key_data, i, UINT64_C(707));
    uint64_t old_data_buffer = 0;
    MDBX_val key = val(&key_data, sizeof(key_data));
    MDBX_val value = val(&value_data, sizeof(value_data));
    MDBX_val old_data = val(&old_data_buffer, sizeof(old_data_buffer));
    rc = mdbx_replace_ex(txn, dbi, &key, &value, &old_data, 0, preserve_value_copy, NULL);
    if (rc != MDBX_SUCCESS)
      break;
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    (void)mdbx_txn_abort(txn);
  const uint64_t finish = monotime_ns();
  if (rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_window_replace_ex(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t window) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_async_op **opv = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!window)
    window = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  opv = calloc(window, sizeof(*opv));
  keys = calloc(window, sizeof(*keys));
  values = calloc(window, sizeof(*values));
  old_values = calloc(window, sizeof(*old_values));
  key_data = calloc(window, sizeof(*key_data));
  value_data = calloc(window, sizeof(*value_data));
  old_data = calloc(window, sizeof(*old_data));
  results = calloc(window, sizeof(*results));
  if (!opv || !keys || !values || !old_values || !key_data || !value_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < window) ? ops - offset : window;
    size_t submitted = 0;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(808));
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      rc = mdbx_async_replace_ex(async, txn, dbi, &keys[i], &values[i], &old_values[i], 0, preserve_value_copy,
                                 NULL, &opv[i]);
      if (rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace_ex", rc, __FILE__, __LINE__);
        if (submitted)
          (void)mdbx_async_wait_release_all(opv, submitted, results);
        goto bailout;
      }
      submitted += 1;
    }

    rc = mdbx_async_wait_release_all(opv, submitted, results);
    if (rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_wait_release_all", rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < submitted; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace_ex item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += submitted;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(value_data);
  free(key_data);
  free(old_values);
  free(values);
  free(keys);
  free(opv);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_batch_replace_ex(MDBX_env *env, MDBX_dbi dbi, size_t ops, size_t batch) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  MDBX_val *keys = NULL;
  MDBX_val *values = NULL;
  MDBX_val *old_values = NULL;
  uint64_t *key_data = NULL;
  uint64_t *value_data = NULL;
  uint64_t *old_data = NULL;
  int *results = NULL;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!batch)
    batch = 1;
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  keys = calloc(batch, sizeof(*keys));
  values = calloc(batch, sizeof(*values));
  old_values = calloc(batch, sizeof(*old_values));
  key_data = calloc(batch, sizeof(*key_data));
  value_data = calloc(batch, sizeof(*value_data));
  old_data = calloc(batch, sizeof(*old_data));
  results = calloc(batch, sizeof(*results));
  if (!keys || !values || !old_values || !key_data || !value_data || !old_data || !results) {
    rc = fail_rc("calloc", MDBX_ENOMEM, __FILE__, __LINE__);
    goto bailout;
  }

  const uint64_t start = monotime_ns();
  for (size_t offset = 0; offset < ops;) {
    const size_t chunk = (ops - offset < batch) ? ops - offset : batch;
    for (size_t i = 0; i < chunk; ++i) {
      key_data[i] = (uint64_t)(offset + i);
      value_data[i] = write_value(key_data[i], offset + i, UINT64_C(909));
      old_data[i] = 0;
      keys[i] = val(&key_data[i], sizeof(key_data[i]));
      values[i] = val(&value_data[i], sizeof(value_data[i]));
      old_values[i] = val(&old_data[i], sizeof(old_data[i]));
      results[i] = MDBX_SUCCESS;
    }

    int batch_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_replace_ex_batch(async, txn, dbi, keys, values, old_values, results, chunk, 0,
                                      preserve_value_copy, NULL, &op));
    CHECK(wait_success(&op, &batch_rc, __FILE__, __LINE__));
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_ex_batch", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t i = 0; i < chunk; ++i) {
      if (results[i] != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_replace_ex_batch item", results[i], __FILE__, __LINE__);
        goto bailout;
      }
    }
    offset += chunk;
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  free(results);
  free(old_data);
  free(value_data);
  free(key_data);
  free(old_values);
  free(values);
  free(keys);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static double async_loop_replace_ex(MDBX_env *env, MDBX_dbi dbi, size_t ops) {
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_async_op *op = NULL;
  struct async_replace_loop_check check;
  int rc = MDBX_SUCCESS;
  double rate = -1.0;

  if (!ops)
    return -1.0;
  memset(&check, 0, sizeof(check));
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));

  size_t completed = 0;
  int operation_rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  CHECK(mdbx_async_replace_ex_loop(async, txn, dbi, ops, async_replace_loop_item_func,
                                   async_replace_loop_result_func, &check, &completed, 0, preserve_value_copy,
                                   NULL, &op));
  CHECK(wait_success(&op, &operation_rc, __FILE__, __LINE__));
  if (operation_rc != MDBX_SUCCESS) {
    rc = fail_rc("mdbx_async_replace_ex_loop", operation_rc, __FILE__, __LINE__);
    goto bailout;
  }
  if (completed != ops || check.checked != ops) {
    rc = fail_msg("unexpected async replace_ex loop completion count", __FILE__, __LINE__);
    goto bailout;
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  txn = NULL;
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  const uint64_t finish = monotime_ns();
  if (finish > start)
    rate = (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  if (txn)
    (void)mdbx_txn_abort(txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  return (rc == MDBX_SUCCESS) ? rate : -1.0;
}

static int blocking_cursor_get_loop(MDBX_cursor *cursor, size_t items, size_t target_pairs,
                                    size_t *completed_pairs) {
  if (!items)
    return fail_msg("cursor get requires a non-empty table", __FILE__, __LINE__);

  int rc = MDBX_SUCCESS;
  *completed_pairs = 0;
  while (*completed_pairs < target_pairs) {
    const MDBX_cursor_op op = (*completed_pairs % items) ? MDBX_NEXT : MDBX_FIRST;
    MDBX_val key = val(NULL, 0);
    MDBX_val data = val(NULL, 0);
    rc = mdbx_cursor_get(cursor, &key, &data, op);
    if (rc != MDBX_SUCCESS)
      return fail_rc("mdbx_cursor_get", rc, __FILE__, __LINE__);
    if (key.iov_len != sizeof(uint64_t))
      return fail_msg("unexpected cursor get key size", __FILE__, __LINE__);
    uint64_t actual_key = 0;
    memcpy(&actual_key, key.iov_base, sizeof(actual_key));
    if (actual_key >= items)
      return fail_msg("unexpected cursor get key", __FILE__, __LINE__);
    rc = expect_value(&data, actual_key, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      return rc;
    *completed_pairs += 1;
  }
  return MDBX_SUCCESS;
}

static double blocking_serial_cursor_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs) {
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
  rc = blocking_cursor_get_loop(cursor, items, target_pairs, &completed_pairs);
  const uint64_t finish = monotime_ns();
  const int close_rc = mdbx_cursor_close2(cursor);
  const int abort_rc = mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || close_rc != MDBX_SUCCESS || abort_rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static void *cursor_get_worker_main(void *arg) {
  struct cursor_get_worker *const worker = (struct cursor_get_worker *)arg;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  worker->completed_pairs = 0;
  worker->rc = mdbx_txn_begin(worker->env, NULL, MDBX_TXN_RDONLY, &txn);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = mdbx_cursor_open(txn, worker->dbi, &cursor);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = blocking_cursor_get_loop(cursor, worker->items, worker->target_pairs,
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

static double blocking_parallel_cursor_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                           size_t workers_count) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct cursor_get_worker *workers = calloc(workers_count, sizeof(*workers));
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
    const int rc = pthread_create(&threads[i], NULL, cursor_get_worker_main, &workers[i]);
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

static int blocking_cursor_batch_loop(MDBX_cursor *cursor, size_t items, size_t target_pairs, size_t batch_pairs,
                                      size_t *completed_pairs, bool large_values,
                                      size_t value_bytes) {
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
    rc = large_values ? expect_cursor_large_batch(pairs, count, items, value_bytes,
                                                  __FILE__, __LINE__)
                      : expect_cursor_batch(pairs, count, items, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    *completed_pairs += count / 2;
    op = (operation_rc == MDBX_SUCCESS) ? MDBX_NEXT : MDBX_FIRST;
  }

bailout:
  free(pairs);
  return rc;
}

static double blocking_serial_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                size_t target_pairs, size_t batch_pairs,
                                                bool large_values, size_t value_bytes) {
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
  rc = blocking_cursor_batch_loop(cursor, items, target_pairs, batch_pairs, &completed_pairs,
                                  large_values, value_bytes);
  const uint64_t finish = monotime_ns();
  const int close_rc = mdbx_cursor_close2(cursor);
  const int abort_rc = mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || close_rc != MDBX_SUCCESS || abort_rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double blocking_serial_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                           size_t batch_pairs) {
  return blocking_serial_cursor_batch_impl(env, dbi, items, target_pairs, batch_pairs, false, 0);
}

static double blocking_serial_cursor_large_batch(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                 size_t target_pairs, size_t batch_pairs,
                                                 size_t value_bytes) {
  return blocking_serial_cursor_batch_impl(env, dbi, items, target_pairs, batch_pairs, true,
                                           value_bytes);
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
                                            &worker->completed_pairs, worker->large_values,
                                            worker->value_bytes);
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

static double blocking_parallel_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                  size_t target_pairs, size_t workers_count,
                                                  size_t batch_pairs, bool large_values,
                                                  size_t value_bytes) {
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
    workers[i].large_values = large_values;
    workers[i].value_bytes = value_bytes;
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

static double blocking_parallel_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                             size_t workers_count, size_t batch_pairs) {
  return blocking_parallel_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                             batch_pairs, false, 0);
}

static double blocking_parallel_cursor_large_batch(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                   size_t target_pairs, size_t workers_count,
                                                   size_t batch_pairs, size_t value_bytes) {
  return blocking_parallel_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                             batch_pairs, true, value_bytes);
}

static int blocking_cursor_scan_loop(MDBX_cursor *cursor, size_t items, size_t target_pairs,
                                     size_t *completed_pairs, bool scan_from) {
  if (!items)
    return fail_msg("cursor scan requires a non-empty table", __FILE__, __LINE__);

  int rc = MDBX_SUCCESS;
  *completed_pairs = 0;
  while (*completed_pairs < target_pairs) {
    const size_t remaining = target_pairs - *completed_pairs;
    const size_t chunk = remaining < items ? remaining : items;
    struct cursor_scan_check check = {items, chunk, 0};
    if (scan_from) {
      uint64_t key_data = 0;
      MDBX_val key = val(&key_data, sizeof(key_data));
      MDBX_val data = val(NULL, 0);
      rc = mdbx_cursor_scan_from(cursor, cursor_scan_check_func, &check, MDBX_SET_LOWERBOUND, &key, &data,
                                 MDBX_NEXT, NULL);
    } else {
      rc = mdbx_cursor_scan(cursor, cursor_scan_check_func, &check, MDBX_FIRST, MDBX_NEXT, NULL);
    }
    if (rc != MDBX_RESULT_TRUE) {
      rc = fail_rc(scan_from ? "mdbx_cursor_scan_from" : "mdbx_cursor_scan", rc, __FILE__, __LINE__);
      goto bailout;
    }
    if (check.checked_pairs != chunk) {
      rc = fail_msg("unexpected cursor scan count", __FILE__, __LINE__);
      goto bailout;
    }
    *completed_pairs += check.checked_pairs;
  }
  rc = MDBX_SUCCESS;

bailout:
  return rc;
}

static double blocking_serial_cursor_scan_common(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                                 bool scan_from) {
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
  rc = blocking_cursor_scan_loop(cursor, items, target_pairs, &completed_pairs, scan_from);
  const uint64_t finish = monotime_ns();
  const int close_rc = mdbx_cursor_close2(cursor);
  const int abort_rc = mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || close_rc != MDBX_SUCCESS || abort_rc != MDBX_SUCCESS || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double blocking_serial_cursor_scan(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs) {
  return blocking_serial_cursor_scan_common(env, dbi, items, target_pairs, false);
}

static double blocking_serial_cursor_scan_from(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs) {
  return blocking_serial_cursor_scan_common(env, dbi, items, target_pairs, true);
}

static void *cursor_scan_worker_main(void *arg) {
  struct cursor_scan_worker *const worker = (struct cursor_scan_worker *)arg;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  worker->completed_pairs = 0;
  worker->rc = mdbx_txn_begin(worker->env, NULL, MDBX_TXN_RDONLY, &txn);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = mdbx_cursor_open(txn, worker->dbi, &cursor);
  if (worker->rc == MDBX_SUCCESS)
    worker->rc = blocking_cursor_scan_loop(cursor, worker->items, worker->target_pairs,
                                           &worker->completed_pairs, worker->scan_from);
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

static double blocking_parallel_cursor_scan_common(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                                   size_t workers_count, bool scan_from) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct cursor_scan_worker *workers = calloc(workers_count, sizeof(*workers));
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
    workers[i].scan_from = scan_from;
    const int rc = pthread_create(&threads[i], NULL, cursor_scan_worker_main, &workers[i]);
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

static double blocking_parallel_cursor_scan(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                            size_t workers_count) {
  return blocking_parallel_cursor_scan_common(env, dbi, items, target_pairs, workers_count, false);
}

static double blocking_parallel_cursor_scan_from(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                                 size_t workers_count) {
  return blocking_parallel_cursor_scan_common(env, dbi, items, target_pairs, workers_count, true);
}

static int async_worker_init(MDBX_env *env, struct async_worker *worker, MDBX_dbi dbi, size_t window) {
  int rc;
  MDBX_async_op *op = NULL;
  worker->ops = calloc(window, sizeof(*worker->ops));
  worker->data = calloc(window, sizeof(*worker->data));
  worker->key_vals = calloc(window, sizeof(*worker->key_vals));
  worker->cache_entries = calloc(window, sizeof(*worker->cache_entries));
  worker->cache_results = calloc(window, sizeof(*worker->cache_results));
  worker->keys = calloc(window, sizeof(*worker->keys));
  worker->value_counts = calloc(window, sizeof(*worker->value_counts));
  worker->results = calloc(window, sizeof(*worker->results));
  if (!worker->ops || !worker->data || !worker->key_vals || !worker->cache_entries ||
      !worker->cache_results || !worker->keys || !worker->value_counts || !worker->results)
    return MDBX_ENOMEM;
  for (size_t i = 0; i < window; ++i)
    mdbx_cache_init(&worker->cache_entries[i]);

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
  free(worker->cache_entries);
  free(worker->cache_results);
  free(worker->keys);
  free(worker->value_counts);
  free(worker->results);
}

static double async_single_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  struct async_worker worker = {0};
  if (async_worker_init(env, &worker, dbi, 1) != MDBX_SUCCESS)
    return -1.0;

  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    worker.keys[0] = key_for(i, items);
    worker.key_vals[0] = val(&worker.keys[0], sizeof(worker.keys[0]));
    worker.data[0] = val(NULL, 0);
    rc = mdbx_async_get(worker.async, worker.txn, dbi, &worker.key_vals[0], &worker.data[0],
                        &worker.ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    int operation_rc = MDBX_SUCCESS;
    rc = wait_success(&worker.ops[0], &operation_rc, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    if (operation_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_get single", operation_rc, __FILE__, __LINE__);
      goto bailout;
    }
    rc = expect_value(&worker.data[0], worker.keys[0], __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
  }
  const uint64_t finish = monotime_ns();
  async_worker_destroy(&worker);
  if (finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (worker.ops && worker.ops[0])
    (void)wait_success(&worker.ops[0], NULL, __FILE__, __LINE__);
  async_worker_destroy(&worker);
  (void)rc;
  return -1.0;
}

static double async_single_get_ex(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  struct async_worker worker = {0};
  if (async_worker_init(env, &worker, dbi, 1) != MDBX_SUCCESS)
    return -1.0;

  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    worker.keys[0] = key_for(i, items);
    worker.key_vals[0] = val(&worker.keys[0], sizeof(worker.keys[0]));
    worker.data[0] = val(NULL, 0);
    worker.value_counts[0] = 0;
    rc = mdbx_async_get_ex(worker.async, worker.txn, dbi, &worker.key_vals[0], &worker.data[0],
                           &worker.value_counts[0], &worker.ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    int operation_rc = MDBX_SUCCESS;
    rc = wait_success(&worker.ops[0], &operation_rc, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    if (operation_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_get_ex single", operation_rc, __FILE__, __LINE__);
      goto bailout;
    }
    if (worker.value_counts[0] != 1) {
      rc = fail_msg("unexpected async get_ex single value count", __FILE__, __LINE__);
      goto bailout;
    }
    rc = expect_value(&worker.data[0], worker.keys[0], __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
  }
  const uint64_t finish = monotime_ns();
  async_worker_destroy(&worker);
  if (finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (worker.ops && worker.ops[0])
    (void)wait_success(&worker.ops[0], NULL, __FILE__, __LINE__);
  async_worker_destroy(&worker);
  (void)rc;
  return -1.0;
}

static double async_single_lowerbound_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops) {
  struct async_worker worker = {0};
  if (async_worker_init(env, &worker, dbi, 1) != MDBX_SUCCESS)
    return -1.0;

  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < ops; ++i) {
    worker.keys[0] = key_for(i, items);
    worker.key_vals[0] = val(&worker.keys[0], sizeof(worker.keys[0]));
    worker.data[0] = val(NULL, 0);
    rc = mdbx_async_get_equal_or_great(worker.async, worker.txn, dbi, &worker.key_vals[0],
                                       &worker.data[0], &worker.ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    int operation_rc = MDBX_SUCCESS;
    rc = wait_success(&worker.ops[0], &operation_rc, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    if (operation_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_get_equal_or_great single", operation_rc, __FILE__, __LINE__);
      goto bailout;
    }
    rc = expect_value(&worker.data[0], worker.keys[0], __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
  }
  const uint64_t finish = monotime_ns();
  async_worker_destroy(&worker);
  if (finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  if (worker.ops && worker.ops[0])
    (void)wait_success(&worker.ops[0], NULL, __FILE__, __LINE__);
  async_worker_destroy(&worker);
  (void)rc;
  return -1.0;
}

static int async_get_window_loop(struct async_worker *worker, MDBX_dbi dbi, size_t items, size_t ops, size_t offset,
                                 size_t window, bool many) {
  size_t issued = 0;
  while (issued < ops) {
    worker->pending = 0;
    while (worker->pending < window && issued < ops) {
      const size_t slot = worker->pending;
      worker->keys[slot] = key_for(offset + issued, items);
      worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
      worker->data[slot] = val(NULL, 0);
      worker->pending += 1;
      issued += 1;
    }

    int rc = MDBX_SUCCESS;
    if (many) {
      rc = mdbx_async_get_many(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                               worker->pending, worker->ops);
      if (rc != MDBX_SUCCESS)
        return rc;
    } else {
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        rc = mdbx_async_get(worker->async, worker->txn, dbi, &worker->key_vals[slot], &worker->data[slot],
                            &worker->ops[slot]);
        if (rc != MDBX_SUCCESS)
          return rc;
      }
    }
    rc = mdbx_async_wait_release_all(worker->ops, worker->pending, worker->results);
    if (rc != MDBX_SUCCESS)
      return rc;
    for (size_t slot = 0; slot < worker->pending; ++slot) {
      const int operation_rc = worker->results[slot];
      if (operation_rc != MDBX_SUCCESS)
        return fail_rc("mdbx_async_get", operation_rc, __FILE__, __LINE__);
      rc = expect_value(&worker->data[slot], worker->keys[slot], __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        return rc;
    }
    worker->pending = 0;
  }
  return MDBX_SUCCESS;
}

static int async_get_batch_window_loop(struct async_worker *worker, MDBX_dbi dbi, size_t items, size_t ops,
                                       size_t offset, size_t window) {
  size_t issued = 0;
  while (issued < ops) {
    worker->pending = 0;
    while (worker->pending < window && issued < ops) {
      const size_t slot = worker->pending++;
      worker->keys[slot] = key_for(offset + issued, items);
      worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
      worker->data[slot] = val(NULL, 0);
      issued += 1;
    }
    int rc = mdbx_async_get_batch(worker->async, worker->txn, dbi, worker->key_vals, worker->data, worker->results,
                                  worker->pending, &worker->ops[0]);
    if (rc != MDBX_SUCCESS)
      return rc;

    int batch_rc = MDBX_SUCCESS;
    rc = wait_success(&worker->ops[0], &batch_rc, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      return rc;
    if (batch_rc != MDBX_SUCCESS)
      return fail_rc("mdbx_async_get_batch", batch_rc, __FILE__, __LINE__);
    for (size_t slot = 0; slot < worker->pending; ++slot) {
      const int operation_rc = worker->results[slot];
      if (operation_rc != MDBX_SUCCESS)
        return fail_rc("mdbx_async_get_batch item", operation_rc, __FILE__, __LINE__);
      rc = expect_value(&worker->data[slot], worker->keys[slot], __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        return rc;
    }
    worker->pending = 0;
  }
  return MDBX_SUCCESS;
}

static double async_parallel_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                      size_t workers_count, size_t window, bool many) {
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
      if (worker->pending && many) {
        rc = mdbx_async_get_many(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                                 worker->pending, worker->ops);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      } else {
        for (size_t slot = 0; slot < worker->pending; ++slot) {
          rc = mdbx_async_get(worker->async, worker->txn, dbi, &worker->key_vals[slot],
                              &worker->data[slot], &worker->ops[slot]);
          if (rc != MDBX_SUCCESS)
            goto bailout;
        }
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      rc = mdbx_async_wait_release_all(worker->ops, worker->pending, worker->results);
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

static double async_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                 size_t window) {
  return async_parallel_get_impl(env, dbi, items, ops, workers_count, window, false);
}

static double async_many_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                      size_t workers_count, size_t window) {
  return async_parallel_get_impl(env, dbi, items, ops, workers_count, window, true);
}

static void *async_thread_worker_main(void *arg) {
  struct async_thread_worker *const worker = (struct async_thread_worker *)arg;
  worker->rc = worker->batch
                   ? async_get_batch_window_loop(&worker->worker, worker->dbi, worker->items, worker->ops,
                                                 worker->offset, worker->window)
                   : async_get_window_loop(&worker->worker, worker->dbi, worker->items, worker->ops,
                                           worker->offset, worker->window, worker->many);
  if (worker->rc != MDBX_SUCCESS) {
    for (size_t slot = 0; slot < worker->worker.pending; ++slot) {
      if (worker->worker.ops[slot])
        (void)wait_success(&worker->worker.ops[slot], NULL, __FILE__, __LINE__);
    }
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                      size_t window, bool batch, bool many) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  size_t initialized_count = 0;
  size_t created_count = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].ops = base_ops + (i < extra_ops);
    workers[i].offset = offset;
    workers[i].window = window;
    workers[i].batch = batch;
    workers[i].many = many;
    offset += workers[i].ops;
    initialized_count = i + 1;
    if (async_worker_init(env, &workers[i].worker, dbi, window) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
  }
  finish = monotime_ns();

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                 size_t window) {
  return async_threaded_get_impl(env, dbi, items, ops, workers_count, window, false, false);
}

static double async_threaded_many_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                      size_t workers_count, size_t window) {
  return async_threaded_get_impl(env, dbi, items, ops, workers_count, window, false, true);
}

static double async_threaded_batch_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count,
                                       size_t window) {
  return async_threaded_get_impl(env, dbi, items, ops, workers_count, window, true, false);
}

static void *async_thread_loop_worker_main(void *arg) {
  struct async_thread_loop_worker *const worker = (struct async_thread_loop_worker *)arg;
  if (!worker->ops) {
    worker->rc = MDBX_SUCCESS;
    return NULL;
  }
  worker->check.items = worker->items;
  worker->check.offset = worker->offset;
  worker->check.checked = 0;
  worker->check.key = 0;
  worker->completed = 0;
  switch (worker->kind) {
  case async_thread_loop_get:
    worker->rc = mdbx_async_get_loop(worker->worker.async, worker->worker.txn, worker->dbi, worker->ops,
                                     async_get_loop_key_func, async_get_loop_result_func, &worker->check,
                                     &worker->completed, &worker->worker.ops[0]);
    break;
  case async_thread_loop_get_ex:
    worker->rc = mdbx_async_get_ex_loop(worker->worker.async, worker->worker.txn, worker->dbi, worker->ops,
                                        async_get_loop_key_func, async_get_ex_loop_result_func,
                                        &worker->check, &worker->completed, &worker->worker.ops[0]);
    break;
  case async_thread_loop_lowerbound:
    worker->rc = mdbx_async_get_equal_or_great_loop(
        worker->worker.async, worker->worker.txn, worker->dbi, worker->ops, async_get_loop_key_func,
        NULL, async_lowerbound_loop_result_func, &worker->check, &worker->completed,
        &worker->worker.ops[0]);
    break;
  }
  if (worker->rc == MDBX_SUCCESS) {
    worker->worker.pending = 1;
    worker->rc = wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  if (worker->rc == MDBX_SUCCESS &&
      (worker->completed != worker->ops || worker->check.checked != worker->ops))
    worker->rc = MDBX_PROBLEM;
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_loop_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                           size_t workers_count, enum async_thread_loop_kind kind) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_loop_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  size_t initialized_count = 0;
  size_t created_count = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].ops = base_ops + (i < extra_ops);
    workers[i].offset = offset;
    offset += workers[i].ops;
    workers[i].kind = kind;
    initialized_count = i + 1;
    if (async_worker_init(env, &workers[i].worker, dbi, 1) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_loop_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed += workers[i].completed;
    checked += workers[i].check.checked;
  }
  finish = monotime_ns();
  if (completed != ops || checked != ops)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                      size_t workers_count) {
  return async_threaded_loop_get_impl(env, dbi, items, ops, workers_count, async_thread_loop_get);
}

static double async_threaded_get_ex_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                             size_t workers_count) {
  return async_threaded_loop_get_impl(env, dbi, items, ops, workers_count, async_thread_loop_get_ex);
}

static double async_threaded_lowerbound_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                 size_t workers_count) {
  return async_threaded_loop_get_impl(env, dbi, items, ops, workers_count, async_thread_loop_lowerbound);
}

static void *async_thread_cache_batch_worker_main(void *arg) {
  struct async_thread_cache_batch_worker *const worker = (struct async_thread_cache_batch_worker *)arg;
  size_t issued = 0;
  worker->rc = MDBX_SUCCESS;
  while (worker->rc == MDBX_SUCCESS && issued < worker->ops) {
    struct async_worker *const async_worker = &worker->worker;
    async_worker->pending = 0;
    while (async_worker->pending < worker->window && issued < worker->ops) {
      const size_t slot = async_worker->pending++;
      async_worker->keys[slot] = key_for(worker->offset + slot, worker->items);
      async_worker->key_vals[slot] = val(&async_worker->keys[slot],
                                         sizeof(async_worker->keys[slot]));
      async_worker->data[slot] = val(NULL, 0);
      async_worker->cache_results[slot].errcode = MDBX_PROBLEM;
      async_worker->cache_results[slot].status = MDBX_CACHE_ERROR;
      issued += 1;
    }
    if (!async_worker->pending)
      continue;

    worker->rc =
        worker->singlethreaded
            ? mdbx_async_cache_get_SingleThreaded_batch(
                  async_worker->async, async_worker->txn, worker->dbi, async_worker->key_vals,
                  async_worker->data, async_worker->cache_entries, async_worker->cache_results,
                  async_worker->pending, &async_worker->ops[0])
            : mdbx_async_cache_get_batch(async_worker->async, async_worker->txn, worker->dbi,
                                         async_worker->key_vals, async_worker->data,
                                         async_worker->cache_entries, async_worker->cache_results,
                                         async_worker->pending, &async_worker->ops[0]);
    if (worker->rc != MDBX_SUCCESS)
      break;

    int batch_rc = MDBX_SUCCESS;
    worker->rc = wait_success(&async_worker->ops[0], &batch_rc, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS && batch_rc != MDBX_SUCCESS)
      worker->rc = fail_rc(worker->singlethreaded ? "mdbx_async_cache_get_SingleThreaded_batch"
                                                  : "mdbx_async_cache_get_batch",
                           batch_rc, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS) {
      for (size_t slot = 0; slot < async_worker->pending; ++slot) {
        if (async_worker->cache_results[slot].errcode != MDBX_SUCCESS) {
          worker->rc = fail_rc(worker->singlethreaded ? "mdbx_cache_get_SingleThreaded threaded batch result"
                                                      : "mdbx_cache_get threaded batch result",
                               async_worker->cache_results[slot].errcode, __FILE__, __LINE__);
          break;
        }
        worker->rc = expect_value(&async_worker->data[slot], async_worker->keys[slot],
                                  __FILE__, __LINE__);
        if (worker->rc != MDBX_SUCCESS)
          break;
      }
    }
    async_worker->pending = 0;
  }

  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_cache_batch_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                  size_t ops, size_t workers_count,
                                                  size_t window, bool singlethreaded) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cache_batch_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  size_t initialized_count = 0;
  size_t created_count = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].ops = base_ops + (i < extra_ops);
    workers[i].offset = offset;
    workers[i].window = window;
    workers[i].singlethreaded = singlethreaded;
    offset += workers[i].ops;
    initialized_count = i + 1;
    if (async_worker_init(env, &workers[i].worker, dbi, window) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cache_batch_worker_main,
                                  &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
  }
  finish = monotime_ns();

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_cache_batch_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                             size_t ops, size_t workers_count, size_t window) {
  return async_threaded_cache_batch_get_impl(env, dbi, items, ops, workers_count, window, false);
}

static double async_threaded_cache_st_batch_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                size_t ops, size_t workers_count, size_t window) {
  return async_threaded_cache_batch_get_impl(env, dbi, items, ops, workers_count, window, true);
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

static double async_batch_callback_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                size_t workers_count, size_t window) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_batch_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }
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
        rc = mdbx_async_get_batch_cb(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                                     worker->results, worker->pending, async_get_batch_check_func, &checks[i],
                                     &worker->ops[0]);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      if (!worker->pending)
        continue;
      rc = wait_success(&worker->ops[0], NULL, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      worker->pending = 0;
    }
  }
  const uint64_t finish = monotime_ns();
  size_t checked = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    checked += checks[i].checked;
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  if (checked != ops || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_get_ex_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                              size_t workers_count, size_t window) {
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
        worker->value_counts[slot] = 0;
        issued += 1;
      }
      if (worker->pending) {
        rc = mdbx_async_get_ex_batch(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                                     worker->value_counts, worker->results, worker->pending, &worker->ops[0]);
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
        rc = fail_rc("mdbx_async_get_ex_batch", batch_rc, __FILE__, __LINE__);
        goto bailout;
      }
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS) {
          rc = fail_rc("mdbx_async_get_ex_batch item", operation_rc, __FILE__, __LINE__);
          goto bailout;
        }
        if (worker->value_counts[slot] != 1) {
          rc = fail_msg("unexpected get_ex value count", __FILE__, __LINE__);
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

static double async_get_ex_many_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                             size_t workers_count, size_t window) {
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
        worker->value_counts[slot] = 0;
        issued += 1;
      }
      if (worker->pending) {
        rc = mdbx_async_get_ex_many(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                                    worker->value_counts, worker->pending, worker->ops);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      rc = mdbx_async_wait_release_all(worker->ops, worker->pending, worker->results);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS) {
          rc = fail_rc("mdbx_async_get_ex_many item", operation_rc, __FILE__, __LINE__);
          goto bailout;
        }
        if (worker->value_counts[slot] != 1) {
          rc = fail_msg("unexpected get_ex many value count", __FILE__, __LINE__);
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

static double async_cache_many_parallel_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                 size_t workers_count, size_t window, bool singlethreaded) {
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
        worker->keys[slot] = key_for(i * window + slot, items);
        worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
        worker->data[slot] = val(NULL, 0);
        worker->cache_results[slot].errcode = MDBX_PROBLEM;
        worker->cache_results[slot].status = MDBX_CACHE_ERROR;
        issued += 1;
      }
      if (worker->pending) {
        rc = singlethreaded
                 ? mdbx_async_cache_get_SingleThreaded_many(
                       worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                       worker->cache_entries, worker->cache_results, worker->pending, worker->ops)
                 : mdbx_async_cache_get_many(worker->async, worker->txn, dbi, worker->key_vals,
                                             worker->data, worker->cache_entries,
                                             worker->cache_results, worker->pending, worker->ops);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      rc = mdbx_async_wait_release_all(worker->ops, worker->pending, worker->results);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS) {
          rc = fail_rc(singlethreaded ? "mdbx_async_cache_get_SingleThreaded_many item"
                                      : "mdbx_async_cache_get_many item",
                       operation_rc, __FILE__, __LINE__);
          goto bailout;
        }
        if (worker->cache_results[slot].errcode != MDBX_SUCCESS) {
          rc = fail_rc(singlethreaded ? "mdbx_cache_get_SingleThreaded result"
                                      : "mdbx_cache_get result",
                       worker->cache_results[slot].errcode, __FILE__, __LINE__);
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

static double async_cache_many_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                            size_t workers_count, size_t window) {
  return async_cache_many_parallel_get_impl(env, dbi, items, ops, workers_count, window, false);
}

static double async_cache_st_many_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                               size_t workers_count, size_t window) {
  return async_cache_many_parallel_get_impl(env, dbi, items, ops, workers_count, window, true);
}

static double async_cache_batch_parallel_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                  size_t workers_count, size_t window, bool singlethreaded,
                                                  bool callback) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_batch_check *checks = callback ? calloc(workers_count, sizeof(*checks)) : NULL;
  if (!workers || (callback && !checks)) {
    free(workers);
    free(checks);
    return -1.0;
  }
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
        worker->keys[slot] = key_for(i * window + slot, items);
        worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
        worker->data[slot] = val(NULL, 0);
        worker->cache_results[slot].errcode = MDBX_PROBLEM;
        worker->cache_results[slot].status = MDBX_CACHE_ERROR;
        issued += 1;
      }
      if (worker->pending) {
        if (callback)
          rc = singlethreaded
                   ? mdbx_async_cache_get_SingleThreaded_batch_cb(
                         worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                         worker->cache_entries, worker->cache_results, worker->pending,
                         async_cache_batch_check_func, &checks[i], &worker->ops[0])
                   : mdbx_async_cache_get_batch_cb(worker->async, worker->txn, dbi,
                                                   worker->key_vals, worker->data,
                                                   worker->cache_entries, worker->cache_results,
                                                   worker->pending, async_cache_batch_check_func,
                                                   &checks[i], &worker->ops[0]);
        else
          rc = singlethreaded
                   ? mdbx_async_cache_get_SingleThreaded_batch(
                         worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                         worker->cache_entries, worker->cache_results, worker->pending, &worker->ops[0])
                   : mdbx_async_cache_get_batch(worker->async, worker->txn, dbi,
                                                worker->key_vals, worker->data,
                                                worker->cache_entries, worker->cache_results,
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
        rc = fail_rc(singlethreaded ? "mdbx_async_cache_get_SingleThreaded_batch"
                                    : "mdbx_async_cache_get_batch",
                     batch_rc, __FILE__, __LINE__);
        goto bailout;
      }
      if (!callback) {
        for (size_t slot = 0; slot < worker->pending; ++slot) {
          if (worker->cache_results[slot].errcode != MDBX_SUCCESS) {
            rc = fail_rc(singlethreaded ? "mdbx_cache_get_SingleThreaded batch result"
                                        : "mdbx_cache_get batch result",
                         worker->cache_results[slot].errcode, __FILE__, __LINE__);
            goto bailout;
          }
          rc = expect_value(&worker->data[slot], worker->keys[slot], __FILE__, __LINE__);
          if (rc != MDBX_SUCCESS)
            goto bailout;
        }
      }
      worker->pending = 0;
    }
  }
  const uint64_t finish = monotime_ns();
  size_t checked = 0;
  if (callback) {
    for (size_t i = 0; i < workers_count; ++i)
      checked += checks[i].checked;
  }
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
  free(workers);
  if (finish <= start || (callback && checked != ops))
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_cache_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                             size_t workers_count, size_t window) {
  return async_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window, false, false);
}

static double async_cache_st_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                size_t workers_count, size_t window) {
  return async_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window, true, false);
}

static double async_cache_batch_callback_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                      size_t workers_count, size_t window) {
  return async_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window, false, true);
}

static double async_cache_st_batch_callback_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                         size_t ops, size_t workers_count, size_t window) {
  return async_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window, true, true);
}

static double async_large_cache_batch_parallel_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                        size_t ops, size_t workers_count, size_t window,
                                                        size_t value_bytes, bool singlethreaded) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!workers)
    return -1.0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, window) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  int rc = MDBX_SUCCESS;
  for (size_t i = 0; i < workers_count; ++i) {
    struct async_worker *const worker = &workers[i];
    worker->pending = window;
    for (size_t slot = 0; slot < worker->pending; ++slot) {
      worker->keys[slot] = key_for(i * window + slot, items);
      worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
      worker->data[slot] = val(NULL, 0);
      worker->cache_results[slot].errcode = MDBX_PROBLEM;
      worker->cache_results[slot].status = MDBX_CACHE_ERROR;
    }
    rc = mdbx_async_cache_get_batch(worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                                    worker->cache_entries, worker->cache_results, worker->pending,
                                    &worker->ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    int batch_rc = MDBX_SUCCESS;
    rc = wait_success(&worker->ops[0], &batch_rc, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    if (batch_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_cache_get_batch warm", batch_rc, __FILE__, __LINE__);
      goto bailout;
    }
    for (size_t slot = 0; slot < worker->pending; ++slot) {
      if (worker->cache_results[slot].errcode != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_cache_get large warm result", worker->cache_results[slot].errcode,
                     __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_large_value(&worker->data[slot], worker->keys[slot], value_bytes, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }
    worker->pending = 0;
  }

  size_t issued = 0;
  const uint64_t start = monotime_ns();
  while (issued < ops) {
    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      worker->pending = 0;
      while (worker->pending < window && issued < ops) {
        const size_t slot = worker->pending++;
        worker->keys[slot] = key_for(i * window + slot, items);
        worker->key_vals[slot] = val(&worker->keys[slot], sizeof(worker->keys[slot]));
        worker->data[slot] = val(NULL, 0);
        worker->cache_results[slot].errcode = MDBX_PROBLEM;
        worker->cache_results[slot].status = MDBX_CACHE_ERROR;
        issued += 1;
      }
      if (worker->pending) {
        rc = singlethreaded
                 ? mdbx_async_cache_get_SingleThreaded_batch(
                       worker->async, worker->txn, dbi, worker->key_vals, worker->data,
                       worker->cache_entries, worker->cache_results, worker->pending, &worker->ops[0])
                 : mdbx_async_cache_get_batch(worker->async, worker->txn, dbi, worker->key_vals,
                                              worker->data, worker->cache_entries,
                                              worker->cache_results, worker->pending, &worker->ops[0]);
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
        rc = fail_rc(singlethreaded ? "mdbx_async_cache_get_SingleThreaded_batch large"
                                    : "mdbx_async_cache_get_batch large",
                     batch_rc, __FILE__, __LINE__);
        goto bailout;
      }
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        if (worker->cache_results[slot].errcode != MDBX_SUCCESS) {
          rc = fail_rc(singlethreaded ? "mdbx_cache_get_SingleThreaded large batch result"
                                      : "mdbx_cache_get large batch result",
                       worker->cache_results[slot].errcode, __FILE__, __LINE__);
          goto bailout;
        }
        rc = expect_large_value(&worker->data[slot], worker->keys[slot], value_bytes, __FILE__, __LINE__);
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

static double async_large_cache_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                   size_t ops, size_t workers_count, size_t window,
                                                   size_t value_bytes) {
  return async_large_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window,
                                                   value_bytes, false);
}

static double async_large_cache_st_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                      size_t ops, size_t workers_count, size_t window,
                                                      size_t value_bytes) {
  return async_large_cache_batch_parallel_get_impl(env, dbi, items, ops, workers_count, window,
                                                   value_bytes, true);
}

static double async_lowerbound_batch_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                  size_t workers_count, size_t window) {
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
        rc = mdbx_async_get_equal_or_great_batch(worker->async, worker->txn, dbi, worker->key_vals,
                                                 worker->data, worker->results, worker->pending,
                                                 &worker->ops[0]);
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
        rc = fail_rc("mdbx_async_get_equal_or_great_batch", batch_rc, __FILE__, __LINE__);
        goto bailout;
      }
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS && operation_rc != MDBX_RESULT_TRUE) {
          rc = fail_rc("mdbx_async_get_equal_or_great_batch item", operation_rc, __FILE__, __LINE__);
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

static double async_lowerbound_many_parallel_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                                 size_t workers_count, size_t window) {
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
        rc = mdbx_async_get_equal_or_great_many(worker->async, worker->txn, dbi, worker->key_vals,
                                                worker->data, worker->pending, worker->ops);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }

    for (size_t i = 0; i < workers_count; ++i) {
      struct async_worker *const worker = &workers[i];
      rc = mdbx_async_wait_release_all(worker->ops, worker->pending, worker->results);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      for (size_t slot = 0; slot < worker->pending; ++slot) {
        const int operation_rc = worker->results[slot];
        if (operation_rc != MDBX_SUCCESS && operation_rc != MDBX_RESULT_TRUE) {
          rc = fail_rc("mdbx_async_get_equal_or_great_many item", operation_rc, __FILE__, __LINE__);
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

static double async_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_loop_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, 1) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const size_t worker_ops = base_ops + (i < extra_ops);
    if (!worker_ops)
      continue;
    checks[i].items = items;
    checks[i].offset = offset;
    checks[i].checked = 0;
    checks[i].key = 0;
    workers[i].count = 0;
    offset += worker_ops;
    rc = mdbx_async_get_loop(workers[i].async, workers[i].txn, dbi, worker_ops, async_get_loop_key_func,
                             async_get_loop_result_func, &checks[i], &workers[i].count, &workers[i].ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].pending = 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (!workers[i].pending)
      continue;
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    workers[i].pending = 0;
    if (rc != MDBX_SUCCESS)
      goto bailout;
    completed += workers[i].count;
    checked += checks[i].checked;
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
  free(workers);
  if (completed != ops || checked != ops || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_cache_loop_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                        size_t workers_count, bool singlethreaded) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_loop_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  int rc = MDBX_SUCCESS;
  for (size_t i = 0; i < workers_count; ++i) {
    const size_t worker_ops = base_ops + (i < extra_ops);
    checks[i].items = items;
    checks[i].offset = offset;
    offset += worker_ops;
    if (async_worker_init(env, &workers[i], dbi, worker_ops ? worker_ops : 1) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
    if (!worker_ops)
      continue;
    size_t warm_completed = 0;
    checks[i].checked = 0;
    checks[i].key = 0;
    rc = singlethreaded
             ? mdbx_async_cache_get_SingleThreaded_loop(
                   workers[i].async, workers[i].txn, dbi, worker_ops, async_get_loop_key_func,
                   workers[i].cache_entries, async_cache_loop_result_func, &checks[i], &warm_completed,
                   &workers[i].ops[0])
             : mdbx_async_cache_get_loop(workers[i].async, workers[i].txn, dbi, worker_ops,
                                         async_get_loop_key_func, workers[i].cache_entries,
                                         async_cache_loop_result_func, &checks[i], &warm_completed,
                                         &workers[i].ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].ops[0] = NULL;
    if (warm_completed != worker_ops || checks[i].checked != worker_ops) {
      rc = MDBX_PROBLEM;
      goto bailout;
    }
  }

  offset = 0;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const size_t worker_ops = base_ops + (i < extra_ops);
    if (!worker_ops)
      continue;
    checks[i].items = items;
    checks[i].offset = offset;
    checks[i].checked = 0;
    checks[i].key = 0;
    workers[i].count = 0;
    offset += worker_ops;
    rc = singlethreaded
             ? mdbx_async_cache_get_SingleThreaded_loop(
                   workers[i].async, workers[i].txn, dbi, worker_ops, async_get_loop_key_func,
                   workers[i].cache_entries, async_cache_loop_result_func, &checks[i], &workers[i].count,
                   &workers[i].ops[0])
             : mdbx_async_cache_get_loop(workers[i].async, workers[i].txn, dbi, worker_ops,
                                         async_get_loop_key_func, workers[i].cache_entries,
                                         async_cache_loop_result_func, &checks[i], &workers[i].count,
                                         &workers[i].ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].pending = 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (!workers[i].pending)
      continue;
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    workers[i].pending = 0;
    if (rc != MDBX_SUCCESS)
      goto bailout;
    completed += workers[i].count;
    checked += checks[i].checked;
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
  free(workers);
  if (completed != ops || checked != ops || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_cache_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                   size_t workers_count) {
  return async_cache_loop_get_impl(env, dbi, items, ops, workers_count, false);
}

static double async_cache_st_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                      size_t workers_count) {
  return async_cache_loop_get_impl(env, dbi, items, ops, workers_count, true);
}

static void *async_thread_cache_loop_worker_main(void *arg) {
  struct async_thread_cache_loop_worker *const worker = (struct async_thread_cache_loop_worker *)arg;
  if (!worker->ops) {
    worker->rc = MDBX_SUCCESS;
    return NULL;
  }
  worker->check.checked = 0;
  worker->check.key = 0;
  worker->completed = 0;
  worker->rc =
      worker->singlethreaded
          ? mdbx_async_cache_get_SingleThreaded_loop(
                worker->worker.async, worker->worker.txn, worker->dbi, worker->ops,
                async_get_loop_key_func, worker->worker.cache_entries,
                async_cache_loop_result_func, &worker->check, &worker->completed,
                &worker->worker.ops[0])
          : mdbx_async_cache_get_loop(worker->worker.async, worker->worker.txn, worker->dbi,
                                      worker->ops, async_get_loop_key_func,
                                      worker->worker.cache_entries,
                                      async_cache_loop_result_func, &worker->check,
                                      &worker->completed, &worker->worker.ops[0]);
  if (worker->rc == MDBX_SUCCESS) {
    worker->worker.pending = 1;
    worker->rc = wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  if (worker->rc == MDBX_SUCCESS &&
      (worker->completed != worker->ops || worker->check.checked != worker->ops))
    worker->rc = MDBX_PROBLEM;
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_cache_loop_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                 size_t ops, size_t workers_count,
                                                 bool singlethreaded) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cache_loop_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  size_t initialized_count = 0;
  size_t created_count = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].ops = base_ops + (i < extra_ops);
    workers[i].check.items = items;
    workers[i].check.offset = offset;
    workers[i].singlethreaded = singlethreaded;
    offset += workers[i].ops;
    initialized_count = i + 1;
    if (async_worker_init(env, &workers[i].worker, dbi, workers[i].ops ? workers[i].ops : 1) !=
        MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
    if (!workers[i].ops)
      continue;
    size_t warm_completed = 0;
    workers[i].check.checked = 0;
    workers[i].check.key = 0;
    int rc =
        singlethreaded
            ? mdbx_async_cache_get_SingleThreaded_loop(
                  workers[i].worker.async, workers[i].worker.txn, dbi, workers[i].ops,
                  async_get_loop_key_func, workers[i].worker.cache_entries,
                  async_cache_loop_result_func, &workers[i].check, &warm_completed,
                  &workers[i].worker.ops[0])
            : mdbx_async_cache_get_loop(workers[i].worker.async, workers[i].worker.txn, dbi,
                                        workers[i].ops, async_get_loop_key_func,
                                        workers[i].worker.cache_entries,
                                        async_cache_loop_result_func, &workers[i].check,
                                        &warm_completed, &workers[i].worker.ops[0]);
    if (rc != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
    rc = wait_success(&workers[i].worker.ops[0], NULL, __FILE__, __LINE__);
    if (rc != MDBX_SUCCESS || warm_completed != workers[i].ops ||
        workers[i].check.checked != workers[i].ops) {
      failed = 1;
      goto bailout;
    }
    workers[i].worker.ops[0] = NULL;
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cache_loop_worker_main,
                                  &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed += workers[i].completed;
    checked += workers[i].check.checked;
  }
  finish = monotime_ns();
  if (completed != ops || checked != ops)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i) {
    if (workers[i].worker.pending && workers[i].worker.ops && workers[i].worker.ops[0])
      (void)wait_success(&workers[i].worker.ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i].worker);
  }
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_cache_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                            size_t ops, size_t workers_count) {
  return async_threaded_cache_loop_get_impl(env, dbi, items, ops, workers_count, false);
}

static double async_threaded_cache_st_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                               size_t ops, size_t workers_count) {
  return async_threaded_cache_loop_get_impl(env, dbi, items, ops, workers_count, true);
}

static double async_get_ex_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops, size_t workers_count) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_loop_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, 1) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const size_t worker_ops = base_ops + (i < extra_ops);
    if (!worker_ops)
      continue;
    checks[i].items = items;
    checks[i].offset = offset;
    checks[i].checked = 0;
    checks[i].key = 0;
    workers[i].count = 0;
    offset += worker_ops;
    rc = mdbx_async_get_ex_loop(workers[i].async, workers[i].txn, dbi, worker_ops, async_get_loop_key_func,
                                async_get_ex_loop_result_func, &checks[i], &workers[i].count, &workers[i].ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].pending = 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (!workers[i].pending)
      continue;
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    workers[i].pending = 0;
    if (rc != MDBX_SUCCESS)
      goto bailout;
    completed += workers[i].count;
    checked += checks[i].checked;
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
  free(workers);
  if (completed != ops || checked != ops || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_lowerbound_loop_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t ops,
                                        size_t workers_count) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_get_loop_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_worker_init(env, &workers[i], dbi, 1) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_ops = ops / workers_count;
  const size_t extra_ops = ops % workers_count;
  size_t offset = 0;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const size_t worker_ops = base_ops + (i < extra_ops);
    if (!worker_ops)
      continue;
    checks[i].items = items;
    checks[i].offset = offset;
    checks[i].checked = 0;
    checks[i].key = 0;
    workers[i].count = 0;
    offset += worker_ops;
    rc = mdbx_async_get_equal_or_great_loop(workers[i].async, workers[i].txn, dbi, worker_ops,
                                            async_get_loop_key_func, NULL, async_lowerbound_loop_result_func,
                                            &checks[i], &workers[i].count, &workers[i].ops[0]);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].pending = 1;
  }

  size_t completed = 0;
  size_t checked = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    if (!workers[i].pending)
      continue;
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    workers[i].pending = 0;
    if (rc != MDBX_SUCCESS)
      goto bailout;
    completed += workers[i].count;
    checked += checks[i].checked;
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
  free(workers);
  if (completed != ops || checked != ops || finish <= start)
    return -1.0;
  return (double)ops * 1000000000.0 / (double)(finish - start);

bailout:
  for (size_t i = 0; i < workers_count; ++i) {
    if (workers[i].ops && workers[i].ops[0])
      (void)wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    async_worker_destroy(&workers[i]);
  }
  free(checks);
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

static double async_loop_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                           size_t workers_count, size_t batch_pairs, MDBX_cursor_op from_op) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_cursor_batch_check *checks = calloc(workers_count, sizeof(*checks));
  if (!workers || !checks) {
    free(workers);
    free(checks);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_batch_worker_init(env, &workers[i], dbi, batch_pairs) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  int rc = MDBX_SUCCESS;
  const uint64_t start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    checks[i].items = items;
    workers[i].count = 0;
    if (from_op) {
      workers[i].keys[0] = 0;
      workers[i].key_vals[0] = val(&workers[i].keys[0], sizeof(workers[i].keys[0]));
      workers[i].data[0] = val(NULL, 0);
      rc = mdbx_async_cursor_get_batches_from(
          workers[i].async, workers[i].cursor, base_pairs + (i < extra_pairs), batch_pairs,
          from_op, &workers[i].key_vals[0], &workers[i].data[0],
          async_cursor_batch_check_func, &checks[i], &workers[i].count, &workers[i].ops[0]);
    } else {
      rc = mdbx_async_cursor_get_batches(workers[i].async, workers[i].cursor, base_pairs + (i < extra_pairs),
                                         batch_pairs, async_cursor_batch_check_func, &checks[i],
                                         &workers[i].count, &workers[i].ops[0]);
    }
    if (rc != MDBX_SUCCESS)
      goto bailout;
    workers[i].pending = 1;
  }

  size_t completed_pairs = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    rc = wait_success(&workers[i].ops[0], NULL, __FILE__, __LINE__);
    workers[i].pending = 0;
    if (rc != MDBX_SUCCESS)
      goto bailout;
    completed_pairs += workers[i].count;
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(checks);
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
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_loop_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                      size_t workers_count, size_t batch_pairs) {
  return async_loop_cursor_batch_impl(env, dbi, items, target_pairs, workers_count, batch_pairs, 0);
}

static double async_loop_cursor_batch_from(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                           size_t target_pairs, size_t workers_count,
                                           size_t batch_pairs) {
  return async_loop_cursor_batch_impl(env, dbi, items, target_pairs, workers_count, batch_pairs,
                                      MDBX_SET_LOWERBOUND);
}

static double async_loop_cursor_batch_setkey(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                             size_t target_pairs, size_t workers_count,
                                             size_t batch_pairs) {
  return async_loop_cursor_batch_impl(env, dbi, items, target_pairs, workers_count, batch_pairs,
                                      MDBX_SET_KEY);
}

static void *async_thread_cursor_batch_loop_worker_main(void *arg) {
  struct async_thread_cursor_batch_loop_worker *const worker =
      (struct async_thread_cursor_batch_loop_worker *)arg;
  worker->check.items = worker->items;
  worker->completed_pairs = 0;
  worker->worker.count = 0;
  if (worker->loop_from) {
    worker->worker.keys[0] = 0;
    worker->worker.key_vals[0] = val(&worker->worker.keys[0], sizeof(worker->worker.keys[0]));
    worker->worker.data[0] = val(NULL, 0);
    worker->rc = mdbx_async_cursor_get_batches_from(
        worker->worker.async, worker->worker.cursor, worker->target_pairs, worker->batch_pairs,
        MDBX_SET_LOWERBOUND, &worker->worker.key_vals[0], &worker->worker.data[0],
        async_cursor_batch_check_func, &worker->check, &worker->worker.count,
        &worker->worker.ops[0]);
  } else {
    worker->rc = mdbx_async_cursor_get_batches(worker->worker.async, worker->worker.cursor,
                                               worker->target_pairs, worker->batch_pairs,
                                               async_cursor_batch_check_func, &worker->check,
                                               &worker->worker.count, &worker->worker.ops[0]);
  }
  if (worker->rc == MDBX_SUCCESS) {
    worker->worker.pending = 1;
    worker->rc = wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  if (worker->rc == MDBX_SUCCESS) {
    worker->completed_pairs = worker->worker.count;
    if (worker->completed_pairs < worker->target_pairs)
      worker->rc = fail_msg("unexpected threaded async cursor batch loop count", __FILE__, __LINE__);
  }
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_loop_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                    size_t target_pairs, size_t workers_count,
                                                    size_t batch_pairs, bool loop_from) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cursor_batch_loop_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  size_t initialized_count = 0;
  size_t created_count = 0;
  size_t completed_pairs = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].target_pairs = base_pairs + (i < extra_pairs);
    workers[i].batch_pairs = batch_pairs;
    workers[i].loop_from = loop_from;
    initialized_count = i + 1;
    if (async_cursor_batch_worker_init(env, &workers[i].worker, dbi, batch_pairs) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cursor_batch_loop_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed_pairs += workers[i].completed_pairs;
  }
  finish = monotime_ns();
  if (completed_pairs < target_pairs)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_loop_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                               size_t target_pairs, size_t workers_count,
                                               size_t batch_pairs) {
  return async_threaded_loop_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                               batch_pairs, false);
}

static double async_threaded_loop_cursor_batch_from(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                    size_t target_pairs, size_t workers_count,
                                                    size_t batch_pairs) {
  return async_threaded_loop_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                               batch_pairs, true);
}

static double async_parallel_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                               size_t target_pairs, size_t workers_count,
                                               size_t batch_pairs, bool large_values,
                                               size_t value_bytes) {
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
      rc = large_values ? expect_cursor_large_batch(worker->pairs, worker->count, items,
                                                    value_bytes, __FILE__, __LINE__)
                        : expect_cursor_batch(worker->pairs, worker->count, items,
                                              __FILE__, __LINE__);
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

static double async_parallel_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                          size_t workers_count, size_t batch_pairs) {
  return async_parallel_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                          batch_pairs, false, 0);
}

static double async_parallel_cursor_large_batch(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                size_t target_pairs, size_t workers_count,
                                                size_t batch_pairs, size_t value_bytes) {
  return async_parallel_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                          batch_pairs, true, value_bytes);
}

static void *async_thread_cursor_batch_worker_main(void *arg) {
  struct async_thread_cursor_batch_worker *const worker = (struct async_thread_cursor_batch_worker *)arg;
  worker->completed_pairs = 0;
  worker->rc = MDBX_SUCCESS;
  while (worker->rc == MDBX_SUCCESS && worker->completed_pairs < worker->target_pairs) {
    int operation_rc = MDBX_SUCCESS;
    worker->worker.count = 0;
    worker->rc = mdbx_async_cursor_get_batch(worker->worker.async, worker->worker.cursor,
                                             &worker->worker.count, worker->worker.pairs,
                                             worker->batch_pairs * 2,
                                             worker->worker.cursor_started ? MDBX_NEXT : MDBX_FIRST,
                                             &worker->worker.ops[0]);
    if (worker->rc == MDBX_SUCCESS) {
      worker->worker.pending = 1;
      worker->rc = wait_success(&worker->worker.ops[0], &operation_rc, __FILE__, __LINE__);
      worker->worker.pending = 0;
    }
    if (worker->rc == MDBX_SUCCESS && operation_rc != MDBX_SUCCESS && operation_rc != MDBX_RESULT_TRUE)
      worker->rc = fail_rc("mdbx_async_cursor_get_batch", operation_rc, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS && worker->worker.count == 0)
      worker->rc = fail_msg("unexpected empty threaded async cursor batch", __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS)
      worker->rc = worker->large_values
                       ? expect_cursor_large_batch(worker->worker.pairs, worker->worker.count,
                                                   worker->items, worker->value_bytes,
                                                   __FILE__, __LINE__)
                       : expect_cursor_batch(worker->worker.pairs, worker->worker.count,
                                             worker->items, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS) {
      worker->completed_pairs += worker->worker.count / 2;
      worker->worker.cursor_started = operation_rc == MDBX_SUCCESS;
    }
  }
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_cursor_batch_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                               size_t target_pairs, size_t workers_count,
                                               size_t batch_pairs, bool large_values,
                                               size_t value_bytes) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cursor_batch_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  size_t initialized_count = 0;
  size_t created_count = 0;
  size_t completed_pairs = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].target_pairs = base_pairs + (i < extra_pairs);
    workers[i].batch_pairs = batch_pairs;
    workers[i].large_values = large_values;
    workers[i].value_bytes = value_bytes;
    initialized_count = i + 1;
    if (async_cursor_batch_worker_init(env, &workers[i].worker, dbi, batch_pairs) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cursor_batch_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed_pairs += workers[i].completed_pairs;
  }
  finish = monotime_ns();
  if (completed_pairs < target_pairs)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_cursor_batch(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                          size_t workers_count, size_t batch_pairs) {
  return async_threaded_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                          batch_pairs, false, 0);
}

static double async_threaded_cursor_large_batch(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                size_t target_pairs, size_t workers_count,
                                                size_t batch_pairs, size_t value_bytes) {
  return async_threaded_cursor_batch_impl(env, dbi, items, target_pairs, workers_count,
                                          batch_pairs, true, value_bytes);
}

static int async_cursor_scan_worker_init(MDBX_env *env, struct async_worker *worker, MDBX_dbi dbi);

static double async_parallel_cursor_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                        size_t workers_count) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  size_t *targets = calloc(workers_count, sizeof(*targets));
  size_t *completed = calloc(workers_count, sizeof(*completed));
  if (!workers || !targets || !completed) {
    free(completed);
    free(targets);
    free(workers);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_scan_worker_init(env, &workers[i], dbi) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  for (size_t i = 0; i < workers_count; ++i)
    targets[i] = base_pairs + (i < extra_pairs);

  int rc = MDBX_SUCCESS;
  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count; ++i) {
      if (completed[i] >= targets[i])
        continue;
      const MDBX_cursor_op op = (completed[i] % items) ? MDBX_NEXT : MDBX_FIRST;
      workers[i].key_vals[0] = val(NULL, 0);
      workers[i].data[0] = val(NULL, 0);
      rc = mdbx_async_cursor_get(workers[i].async, workers[i].cursor, &workers[i].key_vals[0],
                                 &workers[i].data[0], op, &workers[i].ops[0]);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      workers[i].pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      if (!workers[i].pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&workers[i].ops[0], &operation_rc, __FILE__, __LINE__);
      workers[i].pending = 0;
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_get", operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      if (workers[i].key_vals[0].iov_len != sizeof(uint64_t)) {
        rc = fail_msg("unexpected async cursor get key size", __FILE__, __LINE__);
        goto bailout;
      }
      uint64_t actual_key = 0;
      memcpy(&actual_key, workers[i].key_vals[0].iov_base, sizeof(actual_key));
      if (actual_key >= items) {
        rc = fail_msg("unexpected async cursor get key", __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_value(&workers[i].data[0], actual_key, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      completed[i] += 1;
      completed_pairs += 1;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(completed);
  free(targets);
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
  free(completed);
  free(targets);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_parallel_cursor_get_setkey(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                               size_t target_pairs, size_t workers_count) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  size_t *targets = calloc(workers_count, sizeof(*targets));
  size_t *completed = calloc(workers_count, sizeof(*completed));
  if (!workers || !targets || !completed) {
    free(completed);
    free(targets);
    free(workers);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_scan_worker_init(env, &workers[i], dbi) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  for (size_t i = 0; i < workers_count; ++i)
    targets[i] = base_pairs + (i < extra_pairs);

  int rc = MDBX_SUCCESS;
  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count; ++i) {
      if (completed[i] >= targets[i])
        continue;
      workers[i].keys[0] = key_for(completed[i], items);
      workers[i].key_vals[0] = val(&workers[i].keys[0], sizeof(workers[i].keys[0]));
      workers[i].data[0] = val(NULL, 0);
      rc = mdbx_async_cursor_get(workers[i].async, workers[i].cursor, &workers[i].key_vals[0],
                                 &workers[i].data[0], MDBX_SET_KEY, &workers[i].ops[0]);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      workers[i].pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      if (!workers[i].pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&workers[i].ops[0], &operation_rc, __FILE__, __LINE__);
      workers[i].pending = 0;
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_get set-key", operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      if (workers[i].key_vals[0].iov_len != sizeof(uint64_t)) {
        rc = fail_msg("unexpected async cursor get set-key key size", __FILE__, __LINE__);
        goto bailout;
      }
      uint64_t actual_key = 0;
      memcpy(&actual_key, workers[i].key_vals[0].iov_base, sizeof(actual_key));
      if (actual_key != workers[i].keys[0]) {
        rc = fail_msg("unexpected async cursor get set-key key", __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_value(&workers[i].data[0], actual_key, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      completed[i] += 1;
      completed_pairs += 1;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(completed);
  free(targets);
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
  free(completed);
  free(targets);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_parallel_cursor_large_get_setkey(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                     size_t target_pairs, size_t workers_count,
                                                     size_t value_bytes) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  size_t *targets = calloc(workers_count, sizeof(*targets));
  size_t *completed = calloc(workers_count, sizeof(*completed));
  if (!workers || !targets || !completed) {
    free(completed);
    free(targets);
    free(workers);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_scan_worker_init(env, &workers[i], dbi) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  for (size_t i = 0; i < workers_count; ++i)
    targets[i] = base_pairs + (i < extra_pairs);

  int rc = MDBX_SUCCESS;
  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count; ++i) {
      if (completed[i] >= targets[i])
        continue;
      workers[i].keys[0] = key_for(completed[i], items);
      workers[i].key_vals[0] = val(&workers[i].keys[0], sizeof(workers[i].keys[0]));
      workers[i].data[0] = val(NULL, 0);
      rc = mdbx_async_cursor_get(workers[i].async, workers[i].cursor, &workers[i].key_vals[0],
                                 &workers[i].data[0], MDBX_SET_KEY, &workers[i].ops[0]);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      workers[i].pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      if (!workers[i].pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&workers[i].ops[0], &operation_rc, __FILE__, __LINE__);
      workers[i].pending = 0;
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_cursor_get large set-key", operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      if (workers[i].key_vals[0].iov_len != sizeof(uint64_t)) {
        rc = fail_msg("unexpected async large cursor get set-key key size", __FILE__, __LINE__);
        goto bailout;
      }
      uint64_t actual_key = 0;
      memcpy(&actual_key, workers[i].key_vals[0].iov_base, sizeof(actual_key));
      if (actual_key != workers[i].keys[0]) {
        rc = fail_msg("unexpected async large cursor get set-key key", __FILE__, __LINE__);
        goto bailout;
      }
      rc = expect_large_value(&workers[i].data[0], actual_key, value_bytes, __FILE__, __LINE__);
      if (rc != MDBX_SUCCESS)
        goto bailout;
      completed[i] += 1;
      completed_pairs += 1;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(completed);
  free(targets);
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
  free(completed);
  free(targets);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_loop_cursor_get_impl(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                         size_t workers_count, bool loop_from) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct async_cursor_get_loop_check *checks = calloc(workers_count, sizeof(*checks));
  size_t *targets = calloc(workers_count, sizeof(*targets));
  size_t *completed = calloc(workers_count, sizeof(*completed));
  if (!workers || !checks || !targets || !completed) {
    free(completed);
    free(targets);
    free(checks);
    free(workers);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_scan_worker_init(env, &workers[i], dbi) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  for (size_t i = 0; i < workers_count; ++i)
    targets[i] = base_pairs + (i < extra_pairs);

  int rc = MDBX_SUCCESS;
  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count; ++i) {
      if (completed[i] >= targets[i])
        continue;
      const size_t remaining = targets[i] - completed[i];
      const size_t chunk = remaining < items ? remaining : items;
      checks[i].items = items;
      checks[i].checked_pairs = 0;
      workers[i].count = 0;
      if (loop_from) {
        workers[i].keys[0] = 0;
        workers[i].key_vals[0] = val(&workers[i].keys[0], sizeof(workers[i].keys[0]));
        workers[i].data[0] = val(NULL, 0);
        rc = mdbx_async_cursor_get_loop_from(workers[i].async, workers[i].cursor, chunk, MDBX_SET_LOWERBOUND,
                                             &workers[i].key_vals[0], &workers[i].data[0], MDBX_NEXT,
                                             async_cursor_get_loop_check_func, &checks[i],
                                             &workers[i].count, &workers[i].ops[0]);
      } else {
        rc = mdbx_async_cursor_get_loop(workers[i].async, workers[i].cursor, chunk, MDBX_FIRST, MDBX_NEXT,
                                        async_cursor_get_loop_check_func, &checks[i], &workers[i].count,
                                        &workers[i].ops[0]);
      }
      if (rc != MDBX_SUCCESS)
        goto bailout;
      workers[i].pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      if (!workers[i].pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&workers[i].ops[0], &operation_rc, __FILE__, __LINE__);
      workers[i].pending = 0;
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_SUCCESS) {
        rc = fail_rc(loop_from ? "mdbx_async_cursor_get_loop_from" : "mdbx_async_cursor_get_loop",
                     operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      if (workers[i].count != checks[i].checked_pairs) {
        rc = fail_msg("unexpected async cursor get loop count", __FILE__, __LINE__);
        goto bailout;
      }
      completed[i] += checks[i].checked_pairs;
      completed_pairs += checks[i].checked_pairs;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(completed);
  free(targets);
  free(checks);
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
  free(completed);
  free(targets);
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_loop_cursor_get(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                    size_t workers_count) {
  return async_loop_cursor_get_impl(env, dbi, items, target_pairs, workers_count, false);
}

static double async_loop_cursor_get_from(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                         size_t workers_count) {
  return async_loop_cursor_get_impl(env, dbi, items, target_pairs, workers_count, true);
}

static void *async_thread_cursor_get_loop_worker_main(void *arg) {
  struct async_thread_cursor_get_loop_worker *const worker = (struct async_thread_cursor_get_loop_worker *)arg;
  worker->completed_pairs = 0;
  worker->rc = MDBX_SUCCESS;
  while (worker->rc == MDBX_SUCCESS && worker->completed_pairs < worker->target_pairs) {
    const size_t remaining = worker->target_pairs - worker->completed_pairs;
    const size_t chunk = remaining < worker->items ? remaining : worker->items;
    int operation_rc = MDBX_SUCCESS;
    worker->check.items = worker->items;
    worker->check.checked_pairs = 0;
    worker->worker.count = 0;
    if (worker->loop_from) {
      worker->worker.keys[0] = 0;
      worker->worker.key_vals[0] = val(&worker->worker.keys[0], sizeof(worker->worker.keys[0]));
      worker->worker.data[0] = val(NULL, 0);
      worker->rc = mdbx_async_cursor_get_loop_from(
          worker->worker.async, worker->worker.cursor, chunk, MDBX_SET_LOWERBOUND,
          &worker->worker.key_vals[0], &worker->worker.data[0], MDBX_NEXT,
          async_cursor_get_loop_check_func, &worker->check, &worker->worker.count,
          &worker->worker.ops[0]);
    } else {
      worker->rc = mdbx_async_cursor_get_loop(worker->worker.async, worker->worker.cursor, chunk,
                                              MDBX_FIRST, MDBX_NEXT, async_cursor_get_loop_check_func,
                                              &worker->check, &worker->worker.count, &worker->worker.ops[0]);
    }
    if (worker->rc == MDBX_SUCCESS) {
      worker->worker.pending = 1;
      worker->rc = wait_success(&worker->worker.ops[0], &operation_rc, __FILE__, __LINE__);
      worker->worker.pending = 0;
    }
    if (worker->rc == MDBX_SUCCESS && operation_rc != MDBX_SUCCESS)
      worker->rc = fail_rc(worker->loop_from ? "mdbx_async_cursor_get_loop_from" :
                                               "mdbx_async_cursor_get_loop",
                           operation_rc, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS &&
        (worker->worker.count != chunk || worker->check.checked_pairs != chunk))
      worker->rc = fail_msg("unexpected threaded async cursor get loop count", __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS)
      worker->completed_pairs += worker->worker.count;
  }
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_cursor_get_loop_impl(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                  size_t target_pairs, size_t workers_count,
                                                  bool loop_from) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cursor_get_loop_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  size_t initialized_count = 0;
  size_t created_count = 0;
  size_t completed_pairs = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].target_pairs = base_pairs + (i < extra_pairs);
    workers[i].loop_from = loop_from;
    initialized_count = i + 1;
    if (async_cursor_scan_worker_init(env, &workers[i].worker, dbi) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cursor_get_loop_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed_pairs += workers[i].completed_pairs;
  }
  finish = monotime_ns();
  if (completed_pairs != target_pairs)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_cursor_get_loop(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                             size_t workers_count) {
  return async_threaded_cursor_get_loop_impl(env, dbi, items, target_pairs, workers_count, false);
}

static double async_threaded_cursor_get_loop_from(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                  size_t target_pairs, size_t workers_count) {
  return async_threaded_cursor_get_loop_impl(env, dbi, items, target_pairs, workers_count, true);
}

static int async_cursor_scan_worker_init(MDBX_env *env, struct async_worker *worker, MDBX_dbi dbi) {
  int rc;
  MDBX_async_op *op = NULL;
  CHECK(async_worker_init(env, worker, dbi, 1));
  CHECK(mdbx_async_cursor_open(worker->async, worker->txn, dbi, &worker->cursor, &op));
  CHECK(wait_success(&op, NULL, __FILE__, __LINE__));
  return MDBX_SUCCESS;

bailout:
  if (op)
    (void)wait_success(&op, NULL, __FILE__, __LINE__);
  return rc ? rc : MDBX_PROBLEM;
}

static double async_parallel_cursor_scan_common(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                                size_t workers_count, bool scan_from) {
  struct async_worker *workers = calloc(workers_count, sizeof(*workers));
  struct cursor_scan_check *checks = calloc(workers_count, sizeof(*checks));
  size_t *targets = calloc(workers_count, sizeof(*targets));
  size_t *completed = calloc(workers_count, sizeof(*completed));
  if (!workers || !checks || !targets || !completed) {
    free(completed);
    free(targets);
    free(checks);
    free(workers);
    return -1.0;
  }
  for (size_t i = 0; i < workers_count; ++i) {
    if (async_cursor_scan_worker_init(env, &workers[i], dbi) != MDBX_SUCCESS) {
      workers_count = i + 1;
      goto bailout;
    }
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  for (size_t i = 0; i < workers_count; ++i)
    targets[i] = base_pairs + (i < extra_pairs);

  int rc = MDBX_SUCCESS;
  size_t completed_pairs = 0;
  const uint64_t start = monotime_ns();
  while (completed_pairs < target_pairs) {
    for (size_t i = 0; i < workers_count; ++i) {
      if (completed[i] >= targets[i])
        continue;
      const size_t remaining = targets[i] - completed[i];
      const size_t chunk = remaining < items ? remaining : items;
      checks[i].items = items;
      checks[i].target_pairs = chunk;
      checks[i].checked_pairs = 0;
      if (scan_from) {
        workers[i].keys[0] = 0;
        workers[i].key_vals[0] = val(&workers[i].keys[0], sizeof(workers[i].keys[0]));
        workers[i].data[0] = val(NULL, 0);
        rc = mdbx_async_cursor_scan_from(workers[i].async, workers[i].cursor, cursor_scan_check_func, &checks[i],
                                         MDBX_SET_LOWERBOUND, &workers[i].key_vals[0], &workers[i].data[0],
                                         MDBX_NEXT, NULL, &workers[i].ops[0]);
      } else {
        rc = mdbx_async_cursor_scan(workers[i].async, workers[i].cursor, cursor_scan_check_func, &checks[i],
                                    MDBX_FIRST, MDBX_NEXT, NULL, &workers[i].ops[0]);
      }
      if (rc != MDBX_SUCCESS)
        goto bailout;
      workers[i].pending = 1;
    }

    for (size_t i = 0; i < workers_count; ++i) {
      if (!workers[i].pending)
        continue;
      int operation_rc = MDBX_SUCCESS;
      rc = wait_success(&workers[i].ops[0], &operation_rc, __FILE__, __LINE__);
      workers[i].pending = 0;
      if (rc != MDBX_SUCCESS)
        goto bailout;
      if (operation_rc != MDBX_RESULT_TRUE) {
        rc = fail_rc(scan_from ? "mdbx_async_cursor_scan_from" : "mdbx_async_cursor_scan", operation_rc,
                     __FILE__, __LINE__);
        goto bailout;
      }
      if (checks[i].checked_pairs != checks[i].target_pairs) {
        rc = fail_msg("unexpected async cursor scan count", __FILE__, __LINE__);
        goto bailout;
      }
      completed[i] += checks[i].checked_pairs;
      completed_pairs += checks[i].checked_pairs;
    }
  }
  const uint64_t finish = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i)
    async_worker_destroy(&workers[i]);
  free(completed);
  free(targets);
  free(checks);
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
  free(completed);
  free(targets);
  free(checks);
  free(workers);
  (void)rc;
  return -1.0;
}

static double async_parallel_cursor_scan(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                         size_t workers_count) {
  return async_parallel_cursor_scan_common(env, dbi, items, target_pairs, workers_count, false);
}

static double async_parallel_cursor_scan_from(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                              size_t workers_count) {
  return async_parallel_cursor_scan_common(env, dbi, items, target_pairs, workers_count, true);
}

static void *async_thread_cursor_scan_worker_main(void *arg) {
  struct async_thread_cursor_scan_worker *const worker = (struct async_thread_cursor_scan_worker *)arg;
  worker->completed_pairs = 0;
  worker->rc = MDBX_SUCCESS;
  while (worker->rc == MDBX_SUCCESS && worker->completed_pairs < worker->target_pairs) {
    const size_t remaining = worker->target_pairs - worker->completed_pairs;
    const size_t chunk = remaining < worker->items ? remaining : worker->items;
    int operation_rc = MDBX_SUCCESS;
    worker->check.items = worker->items;
    worker->check.target_pairs = chunk;
    worker->check.checked_pairs = 0;
    if (worker->scan_from) {
      worker->worker.keys[0] = 0;
      worker->worker.key_vals[0] = val(&worker->worker.keys[0], sizeof(worker->worker.keys[0]));
      worker->worker.data[0] = val(NULL, 0);
      worker->rc = mdbx_async_cursor_scan_from(worker->worker.async, worker->worker.cursor,
                                               cursor_scan_check_func, &worker->check,
                                               MDBX_SET_LOWERBOUND, &worker->worker.key_vals[0],
                                               &worker->worker.data[0], MDBX_NEXT, NULL,
                                               &worker->worker.ops[0]);
    } else {
      worker->rc = mdbx_async_cursor_scan(worker->worker.async, worker->worker.cursor,
                                          cursor_scan_check_func, &worker->check, MDBX_FIRST,
                                          MDBX_NEXT, NULL, &worker->worker.ops[0]);
    }
    if (worker->rc == MDBX_SUCCESS) {
      worker->worker.pending = 1;
      worker->rc = wait_success(&worker->worker.ops[0], &operation_rc, __FILE__, __LINE__);
      worker->worker.pending = 0;
    }
    if (worker->rc == MDBX_SUCCESS && operation_rc != MDBX_RESULT_TRUE)
      worker->rc = fail_rc(worker->scan_from ? "mdbx_async_cursor_scan_from" : "mdbx_async_cursor_scan",
                           operation_rc, __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS && worker->check.checked_pairs != chunk)
      worker->rc = fail_msg("unexpected threaded async cursor scan count", __FILE__, __LINE__);
    if (worker->rc == MDBX_SUCCESS)
      worker->completed_pairs += worker->check.checked_pairs;
  }
  if (worker->rc != MDBX_SUCCESS && worker->worker.pending && worker->worker.ops[0]) {
    (void)wait_success(&worker->worker.ops[0], NULL, __FILE__, __LINE__);
    worker->worker.pending = 0;
  }
  return NULL;
}

static double async_threaded_cursor_scan_common(MDBX_env *env, MDBX_dbi dbi, size_t items,
                                                size_t target_pairs, size_t workers_count,
                                                bool scan_from) {
  pthread_t *threads = calloc(workers_count, sizeof(*threads));
  struct async_thread_cursor_scan_worker *workers = calloc(workers_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -1.0;
  }

  const size_t base_pairs = target_pairs / workers_count;
  const size_t extra_pairs = target_pairs % workers_count;
  size_t initialized_count = 0;
  size_t created_count = 0;
  size_t completed_pairs = 0;
  uint64_t start = 0;
  uint64_t finish = 0;
  int failed = 0;
  for (size_t i = 0; i < workers_count; ++i) {
    workers[i].dbi = dbi;
    workers[i].items = items;
    workers[i].target_pairs = base_pairs + (i < extra_pairs);
    workers[i].scan_from = scan_from;
    initialized_count = i + 1;
    if (async_cursor_scan_worker_init(env, &workers[i].worker, dbi) != MDBX_SUCCESS) {
      failed = 1;
      goto bailout;
    }
  }

  start = monotime_ns();
  for (size_t i = 0; i < workers_count; ++i) {
    const int rc = pthread_create(&threads[i], NULL, async_thread_cursor_scan_worker_main, &workers[i]);
    if (rc != 0) {
      workers[i].rc = rc;
      failed = 1;
      break;
    }
    created_count = i + 1;
  }

  for (size_t i = 0; i < created_count; ++i) {
    const int rc = pthread_join(threads[i], NULL);
    if (rc != 0 || workers[i].rc != MDBX_SUCCESS)
      failed = 1;
    completed_pairs += workers[i].completed_pairs;
  }
  finish = monotime_ns();
  if (completed_pairs != target_pairs)
    failed = 1;

bailout:
  for (size_t i = 0; i < initialized_count; ++i)
    async_worker_destroy(&workers[i].worker);
  free(threads);
  free(workers);
  if (failed || finish <= start)
    return -1.0;
  return (double)completed_pairs * 1000000000.0 / (double)(finish - start);
}

static double async_threaded_cursor_scan(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                         size_t workers_count) {
  return async_threaded_cursor_scan_common(env, dbi, items, target_pairs, workers_count, false);
}

static double async_threaded_cursor_scan_from(MDBX_env *env, MDBX_dbi dbi, size_t items, size_t target_pairs,
                                              size_t workers_count) {
  return async_threaded_cursor_scan_common(env, dbi, items, target_pairs, workers_count, true);
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
  MDBX_dbi large_dbi = 0;
  int rc = MDBX_SUCCESS;
  const size_t items = env_size("MDBX_ASYNC_BENCH_ITEMS", DEFAULT_ITEMS);
  const size_t ops = env_size("MDBX_ASYNC_BENCH_OPS", DEFAULT_OPS);
  const size_t write_ops = env_size("MDBX_ASYNC_BENCH_WRITE_OPS", DEFAULT_WRITE_OPS);
  const size_t write_batch = env_size("MDBX_ASYNC_BENCH_WRITE_BATCH", DEFAULT_WRITE_BATCH);
  const size_t delete_ops = (write_ops < items) ? write_ops : items;
  const size_t replace_ops = delete_ops;
  const size_t workers = env_size("MDBX_ASYNC_BENCH_WORKERS", DEFAULT_WORKERS);
  const size_t window = env_size("MDBX_ASYNC_BENCH_WINDOW", DEFAULT_WINDOW);
  const size_t large_items = env_size("MDBX_ASYNC_BENCH_LARGE_ITEMS", DEFAULT_LARGE_ITEMS);
  const size_t large_ops = env_size("MDBX_ASYNC_BENCH_LARGE_OPS", DEFAULT_LARGE_OPS);
  const size_t large_value_bytes = env_size("MDBX_ASYNC_BENCH_LARGE_VALUE_BYTES", DEFAULT_LARGE_VALUE_BYTES);
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
  CHECK(mdbx_env_set_maxdbs(env, 4));
  CHECK(mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM | MDBX_SAFE_NOSYNC | MDBX_NOMETASYNC, 0664));
  CHECK(seed_database(env, &dbi, items));

  printf("async-api-bench items=%zu ops=%zu write-ops=%zu replace-ops=%zu delete-ops=%zu write-batch=%zu "
         "workers=%zu window=%zu cursor-batch-pairs=%zu large-items=%zu large-ops=%zu large-value=%zu\n",
         items, ops, write_ops, replace_ops, delete_ops, write_batch, workers, window, cursor_batch_pairs,
         large_items, large_ops, large_value_bytes);
  const double blocking_serial = blocking_serial_get(env, dbi, items, ops);
  const double blocking_parallel = blocking_parallel_get(env, dbi, items, ops, workers);
  const double async_single = async_single_get(env, dbi, items, ops);
  const double async_single_get_ex_rate = async_single_get_ex(env, dbi, items, ops);
  const double async_single_lowerbound = async_single_lowerbound_get(env, dbi, items, ops);
  const double async_parallel = async_parallel_get(env, dbi, items, ops, workers, window);
  const double async_many_parallel = async_many_parallel_get(env, dbi, items, ops, workers, window);
  const double async_threaded_parallel = async_threaded_get(env, dbi, items, ops, workers, window);
  const double async_threaded_many_parallel = async_threaded_many_get(env, dbi, items, ops, workers, window);
  const double async_threaded_batch_parallel = async_threaded_batch_get(env, dbi, items, ops, workers, window);
  const double async_batch_parallel = async_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double async_batch_callback_parallel =
      async_batch_callback_parallel_get(env, dbi, items, ops, workers, window);
  const double async_get_ex_batch_parallel =
      async_get_ex_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double async_get_ex_many_parallel =
      async_get_ex_many_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_many_parallel =
      async_cache_many_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_st_many_parallel =
      async_cache_st_many_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_batch_parallel =
      async_cache_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_st_batch_parallel =
      async_cache_st_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_batch_callback_parallel =
      async_cache_batch_callback_parallel_get(env, dbi, items, ops, workers, window);
  const double async_cache_st_batch_callback_parallel =
      async_cache_st_batch_callback_parallel_get(env, dbi, items, ops, workers, window);
  const double async_threaded_cache_batch_parallel =
      async_threaded_cache_batch_get(env, dbi, items, ops, workers, window);
  const double async_threaded_cache_st_batch_parallel =
      async_threaded_cache_st_batch_get(env, dbi, items, ops, workers, window);
  const double async_cache_loop_parallel = async_cache_loop_get(env, dbi, items, ops, workers);
  const double async_cache_st_loop_parallel = async_cache_st_loop_get(env, dbi, items, ops, workers);
  const double async_threaded_cache_loop_parallel =
      async_threaded_cache_loop_get(env, dbi, items, ops, workers);
  const double async_threaded_cache_st_loop_parallel =
      async_threaded_cache_st_loop_get(env, dbi, items, ops, workers);
  const double async_lowerbound_batch_parallel =
      async_lowerbound_batch_parallel_get(env, dbi, items, ops, workers, window);
  const double async_lowerbound_many_parallel =
      async_lowerbound_many_parallel_get(env, dbi, items, ops, workers, window);
  const double async_loop_parallel = async_loop_get(env, dbi, items, ops, workers);
  const double async_get_ex_loop_parallel = async_get_ex_loop_get(env, dbi, items, ops, workers);
  const double async_lowerbound_loop_parallel = async_lowerbound_loop_get(env, dbi, items, ops, workers);
  const double async_threaded_loop_parallel = async_threaded_loop_get(env, dbi, items, ops, workers);
  const double async_threaded_get_ex_loop_parallel =
      async_threaded_get_ex_loop_get(env, dbi, items, ops, workers);
  const double async_threaded_lowerbound_loop_parallel =
      async_threaded_lowerbound_loop_get(env, dbi, items, ops, workers);
  const double blocking_cursor_get_serial = blocking_serial_cursor_get(env, dbi, items, cursor_pairs);
  const double blocking_cursor_get_parallel = blocking_parallel_cursor_get(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_get_parallel = async_parallel_cursor_get(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_get_setkey_parallel =
      async_parallel_cursor_get_setkey(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_get_loop_parallel = async_loop_cursor_get(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_get_loop_from_parallel =
      async_loop_cursor_get_from(env, dbi, items, cursor_pairs, workers);
  const double async_threaded_cursor_get_loop_parallel =
      async_threaded_cursor_get_loop(env, dbi, items, cursor_pairs, workers);
  const double async_threaded_cursor_get_loop_from_parallel =
      async_threaded_cursor_get_loop_from(env, dbi, items, cursor_pairs, workers);
  const double blocking_cursor_serial = blocking_serial_cursor_batch(env, dbi, items, cursor_pairs, cursor_batch_pairs);
  const double blocking_cursor_parallel =
      blocking_parallel_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_cursor_parallel =
      async_parallel_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_threaded_cursor_parallel =
      async_threaded_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_loop_cursor_parallel =
      async_loop_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_loop_cursor_from_parallel =
      async_loop_cursor_batch_from(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_loop_cursor_setkey_parallel =
      async_loop_cursor_batch_setkey(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_threaded_loop_cursor_parallel =
      async_threaded_loop_cursor_batch(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double async_threaded_loop_cursor_from_parallel =
      async_threaded_loop_cursor_batch_from(env, dbi, items, cursor_pairs, workers, cursor_batch_pairs);
  const double blocking_cursor_scan_serial = blocking_serial_cursor_scan(env, dbi, items, cursor_pairs);
  const double blocking_cursor_scan_parallel = blocking_parallel_cursor_scan(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_scan_parallel = async_parallel_cursor_scan(env, dbi, items, cursor_pairs, workers);
  const double async_threaded_cursor_scan_parallel =
      async_threaded_cursor_scan(env, dbi, items, cursor_pairs, workers);
  const double blocking_cursor_scan_from_serial = blocking_serial_cursor_scan_from(env, dbi, items, cursor_pairs);
  const double blocking_cursor_scan_from_parallel =
      blocking_parallel_cursor_scan_from(env, dbi, items, cursor_pairs, workers);
  const double async_cursor_scan_from_parallel =
      async_parallel_cursor_scan_from(env, dbi, items, cursor_pairs, workers);
  const double async_threaded_cursor_scan_from_parallel =
      async_threaded_cursor_scan_from(env, dbi, items, cursor_pairs, workers);
  CHECK(seed_large_database(env, &large_dbi, large_items, large_value_bytes));
  const double async_large_cache_batch_parallel =
      async_large_cache_batch_parallel_get(env, large_dbi, large_items, large_ops, workers, window,
                                           large_value_bytes);
  const double async_large_cache_st_batch_parallel =
      async_large_cache_st_batch_parallel_get(env, large_dbi, large_items, large_ops, workers, window,
                                              large_value_bytes);
  const size_t large_cursor_pairs = large_ops < cursor_batch_pairs ? cursor_batch_pairs : large_ops;
  const double blocking_cursor_large_batch_serial =
      blocking_serial_cursor_large_batch(env, large_dbi, large_items, large_cursor_pairs,
                                         cursor_batch_pairs, large_value_bytes);
  const double blocking_cursor_large_batch_parallel =
      blocking_parallel_cursor_large_batch(env, large_dbi, large_items, large_cursor_pairs,
                                           workers, cursor_batch_pairs, large_value_bytes);
  const double async_cursor_large_batch_parallel =
      async_parallel_cursor_large_batch(env, large_dbi, large_items, large_cursor_pairs, workers,
                                        cursor_batch_pairs, large_value_bytes);
  const double async_cursor_large_setkey_parallel =
      async_parallel_cursor_large_get_setkey(env, large_dbi, large_items, large_ops, workers,
                                             large_value_bytes);
  const double async_threaded_cursor_large_batch_parallel =
      async_threaded_cursor_large_batch(env, large_dbi, large_items, large_cursor_pairs, workers,
                                        cursor_batch_pairs, large_value_bytes);
  CHECK(drop_database(env, large_dbi));
  large_dbi = 0;
  const double blocking_put = blocking_write_put(env, dbi, items, write_ops);
  const double async_put = async_window_put(env, dbi, items, write_ops, window);
  const double async_put_batch = async_batch_put(env, dbi, items, write_ops, write_batch);
  const double async_put_loop = async_loop_put(env, dbi, items, write_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_cursor_put = blocking_cursor_write_put(env, dbi, items, write_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_put = async_window_cursor_put(env, dbi, items, write_ops, window);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_put_batch = async_cursor_batch_put(env, dbi, items, write_ops, write_batch);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_put_loop = async_cursor_loop_put(env, dbi, items, write_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_replace_rate = blocking_replace(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_rate = async_window_replace(env, dbi, replace_ops, window);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_batch_rate = async_batch_replace(env, dbi, replace_ops, write_batch);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_loop_rate = async_loop_replace(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_replace_ex_rate = blocking_replace_ex(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_ex_rate = async_window_replace_ex(env, dbi, replace_ops, window);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_ex_batch_rate = async_batch_replace_ex(env, dbi, replace_ops, write_batch);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_ex_loop_rate = async_loop_replace_ex(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_replace_delete_rate = blocking_replace_delete(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_delete_rate = async_window_replace_delete(env, dbi, replace_ops, window);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_delete_batch_rate = async_batch_replace_delete(env, dbi, replace_ops, write_batch);
  CHECK(seed_database(env, &dbi, items));
  const double async_replace_delete_loop_rate = async_loop_replace_delete(env, dbi, replace_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_del = blocking_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_del = async_window_delete(env, dbi, delete_ops, window);
  CHECK(seed_database(env, &dbi, items));
  const double async_del_batch = async_batch_delete(env, dbi, delete_ops, write_batch);
  CHECK(seed_database(env, &dbi, items));
  const double async_del_loop = async_loop_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_cursor_del = blocking_cursor_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_del = async_cursor_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_del_loop = async_cursor_delete_loop(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double blocking_cursor_range_del = blocking_cursor_range_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, items));
  const double async_cursor_range_del = async_cursor_range_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, delete_ops));
  const double blocking_cursor_bunch_del = blocking_cursor_bunch_delete(env, dbi, delete_ops);
  CHECK(seed_database(env, &dbi, delete_ops));
  const double async_cursor_bunch_del = async_cursor_bunch_delete(env, dbi, delete_ops);
  print_rate("blocking serial get", blocking_serial);
  print_rate("blocking parallel get", blocking_parallel);
  print_rate("async single get", async_single);
  print_rate("async single get_ex", async_single_get_ex_rate);
  print_rate("async single lowerbound", async_single_lowerbound);
  print_rate("async parallel get", async_parallel);
  print_rate("async many parallel get", async_many_parallel);
  print_rate("async threaded get", async_threaded_parallel);
  print_rate("async threaded many get", async_threaded_many_parallel);
  print_rate("async threaded batch get", async_threaded_batch_parallel);
  print_rate("async batch parallel get", async_batch_parallel);
  print_rate("async batch callback get", async_batch_callback_parallel);
  print_rate("async get_ex batch", async_get_ex_batch_parallel);
  print_rate("async get_ex many", async_get_ex_many_parallel);
  print_rate("async cache many", async_cache_many_parallel);
  print_rate("async cache st many", async_cache_st_many_parallel);
  print_rate("async cache batch", async_cache_batch_parallel);
  print_rate("async cache st batch", async_cache_st_batch_parallel);
  print_rate("async cache batch cb", async_cache_batch_callback_parallel);
  print_rate("async cache st batch cb", async_cache_st_batch_callback_parallel);
  print_rate("async large cache batch", async_large_cache_batch_parallel);
  print_rate("async large cache st batch", async_large_cache_st_batch_parallel);
  print_rate("blocking cursor large batch", blocking_cursor_large_batch_serial);
  print_rate("parallel cursor large batch", blocking_cursor_large_batch_parallel);
  print_rate("async cursor large batch", async_cursor_large_batch_parallel);
  print_rate("async cursor large set-key", async_cursor_large_setkey_parallel);
  print_rate("async threaded cursor large", async_threaded_cursor_large_batch_parallel);
  print_rate("async threaded cache batch", async_threaded_cache_batch_parallel);
  print_rate("async threaded cache st batch", async_threaded_cache_st_batch_parallel);
  print_rate("async cache loop", async_cache_loop_parallel);
  print_rate("async cache st loop", async_cache_st_loop_parallel);
  print_rate("async threaded cache loop", async_threaded_cache_loop_parallel);
  print_rate("async threaded cache st loop", async_threaded_cache_st_loop_parallel);
  print_rate("async lowerbound batch", async_lowerbound_batch_parallel);
  print_rate("async lowerbound many", async_lowerbound_many_parallel);
  print_rate("async get loop", async_loop_parallel);
  print_rate("async get_ex loop", async_get_ex_loop_parallel);
  print_rate("async lowerbound loop", async_lowerbound_loop_parallel);
  print_rate("async threaded get loop", async_threaded_loop_parallel);
  print_rate("async threaded get_ex loop", async_threaded_get_ex_loop_parallel);
  print_rate("async threaded lower loop", async_threaded_lowerbound_loop_parallel);
  print_rate("blocking cursor get", blocking_cursor_get_serial);
  print_rate("parallel cursor get", blocking_cursor_get_parallel);
  print_rate("async cursor get", async_cursor_get_parallel);
  print_rate("async cursor get set-key", async_cursor_get_setkey_parallel);
  print_rate("async cursor get loop", async_cursor_get_loop_parallel);
  print_rate("async cursor get loop_from", async_cursor_get_loop_from_parallel);
  print_rate("async threaded cursor get loop", async_threaded_cursor_get_loop_parallel);
  print_rate("async threaded cget loop_from", async_threaded_cursor_get_loop_from_parallel);
  print_rate("blocking cursor batch", blocking_cursor_serial);
  print_rate("parallel cursor batch", blocking_cursor_parallel);
  print_rate("async cursor batch", async_cursor_parallel);
  print_rate("async threaded cursor batch", async_threaded_cursor_parallel);
  print_rate("async cursor loop", async_loop_cursor_parallel);
  print_rate("async cursor loop_from", async_loop_cursor_from_parallel);
  print_rate("async cursor loop set-key", async_loop_cursor_setkey_parallel);
  print_rate("async threaded cursor loop", async_threaded_loop_cursor_parallel);
  print_rate("async threaded cursor loop_from", async_threaded_loop_cursor_from_parallel);
  print_rate("blocking cursor scan", blocking_cursor_scan_serial);
  print_rate("parallel cursor scan", blocking_cursor_scan_parallel);
  print_rate("async cursor scan", async_cursor_scan_parallel);
  print_rate("async threaded cursor scan", async_threaded_cursor_scan_parallel);
  print_rate("blocking cursor scan_from", blocking_cursor_scan_from_serial);
  print_rate("parallel cursor scan_from", blocking_cursor_scan_from_parallel);
  print_rate("async cursor scan_from", async_cursor_scan_from_parallel);
  print_rate("async threaded cursor scan_from", async_threaded_cursor_scan_from_parallel);
  print_rate("blocking write put", blocking_put);
  print_rate("async write put", async_put);
  print_rate("async batch write put", async_put_batch);
  print_rate("async loop write put", async_put_loop);
  print_rate("blocking cursor put", blocking_cursor_put);
  print_rate("async cursor put", async_cursor_put);
  print_rate("async cursor batch put", async_cursor_put_batch);
  print_rate("async cursor loop put", async_cursor_put_loop);
  print_rate("blocking replace", blocking_replace_rate);
  print_rate("async replace", async_replace_rate);
  print_rate("async batch replace", async_replace_batch_rate);
  print_rate("async loop replace", async_replace_loop_rate);
  print_rate("blocking replace_ex", blocking_replace_ex_rate);
  print_rate("async replace_ex", async_replace_ex_rate);
  print_rate("async batch replace_ex", async_replace_ex_batch_rate);
  print_rate("async loop replace_ex", async_replace_ex_loop_rate);
  print_rate("blocking replace delete", blocking_replace_delete_rate);
  print_rate("async replace delete", async_replace_delete_rate);
  print_rate("async batch replace del", async_replace_delete_batch_rate);
  print_rate("async loop replace del", async_replace_delete_loop_rate);
  print_rate("blocking delete", blocking_del);
  print_rate("async delete", async_del);
  print_rate("async batch delete", async_del_batch);
  print_rate("async loop delete", async_del_loop);
  print_rate("blocking cursor delete", blocking_cursor_del);
  print_rate("async cursor delete", async_cursor_del);
  print_rate("async cursor del loop", async_cursor_del_loop);
  print_rate("blocking cursor range del", blocking_cursor_range_del);
  print_rate("async cursor range del", async_cursor_range_del);
  print_rate("blocking cursor bunch del", blocking_cursor_bunch_del);
  print_rate("async cursor bunch del", async_cursor_bunch_del);
  if (blocking_parallel > 0.0 && async_single > 0.0)
    printf("%-28s %8.3f\n", "async-single/blocking par", async_single / blocking_parallel);
  if (blocking_serial > 0.0 && async_single > 0.0)
    printf("%-28s %8.3f\n", "async-single/blocking ser", async_single / blocking_serial);
  if (blocking_parallel > 0.0 && async_single_get_ex_rate > 0.0)
    printf("%-28s %8.3f\n", "async-single-ex/par", async_single_get_ex_rate / blocking_parallel);
  if (blocking_serial > 0.0 && async_single_get_ex_rate > 0.0)
    printf("%-28s %8.3f\n", "async-single-ex/ser", async_single_get_ex_rate / blocking_serial);
  if (blocking_parallel > 0.0 && async_single_lowerbound > 0.0)
    printf("%-28s %8.3f\n", "async-single-lower/par", async_single_lowerbound / blocking_parallel);
  if (blocking_serial > 0.0 && async_single_lowerbound > 0.0)
    printf("%-28s %8.3f\n", "async-single-lower/ser", async_single_lowerbound / blocking_serial);
  if (blocking_parallel > 0.0 && async_parallel > 0.0)
    printf("%-28s %8.3f\n", "async/blocking parallel", async_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-many/blocking par", async_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread/blocking par", async_threaded_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-many/par", async_threaded_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-batch/par", async_threaded_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch/blocking parallel", async_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch-cb/blocking par", async_batch_callback_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_get_ex_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-batch/par", async_get_ex_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_get_ex_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-many/par", async_get_ex_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-many/par", async_cache_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_st_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-many/par", async_cache_st_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch/par", async_cache_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch/par", async_cache_st_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch-cb/par",
           async_cache_batch_callback_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_st_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch-cb/par",
           async_cache_st_batch_callback_parallel / blocking_parallel);
  if (blocking_cursor_large_batch_parallel > 0.0 && async_cursor_large_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-large/par",
           async_cursor_large_batch_parallel / blocking_cursor_large_batch_parallel);
  if (blocking_cursor_large_batch_serial > 0.0 && async_cursor_large_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-large/ser",
           async_cursor_large_batch_parallel / blocking_cursor_large_batch_serial);
  if (blocking_cursor_large_batch_parallel > 0.0 && async_cursor_large_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-clarge-setkey/par",
           async_cursor_large_setkey_parallel / blocking_cursor_large_batch_parallel);
  if (blocking_cursor_large_batch_serial > 0.0 && async_cursor_large_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-clarge-setkey/ser",
           async_cursor_large_setkey_parallel / blocking_cursor_large_batch_serial);
  if (async_cursor_large_batch_parallel > 0.0 && async_cursor_large_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-clarge-setkey/batch",
           async_cursor_large_setkey_parallel / async_cursor_large_batch_parallel);
  if (blocking_cursor_large_batch_parallel > 0.0 && async_threaded_cursor_large_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-clarge/par",
           async_threaded_cursor_large_batch_parallel / blocking_cursor_large_batch_parallel);
  if (blocking_cursor_large_batch_serial > 0.0 && async_threaded_cursor_large_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-clarge/ser",
           async_threaded_cursor_large_batch_parallel / blocking_cursor_large_batch_serial);
  if (async_cursor_large_batch_parallel > 0.0 && async_threaded_cursor_large_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-clarge/batch",
           async_threaded_cursor_large_batch_parallel / async_cursor_large_batch_parallel);
  if (blocking_parallel > 0.0 && async_threaded_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-batch/par",
           async_threaded_cache_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-b/par",
           async_threaded_cache_st_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-loop/par", async_cache_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-loop/par", async_cache_st_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-loop/par",
           async_threaded_cache_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-l/par",
           async_threaded_cache_st_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_lowerbound_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-batch/par", async_lowerbound_batch_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_lowerbound_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-many/par", async_lowerbound_many_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop/blocking par", async_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_get_ex_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-loop/par", async_get_ex_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_lowerbound_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-loop/par", async_lowerbound_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-loop/par", async_threaded_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_get_ex_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-get-ex-loop/par",
           async_threaded_get_ex_loop_parallel / blocking_parallel);
  if (blocking_parallel > 0.0 && async_threaded_lowerbound_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-lower-loop/par",
           async_threaded_lowerbound_loop_parallel / blocking_parallel);
  if (async_parallel > 0.0 && async_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-many/async", async_many_parallel / async_parallel);
  if (async_threaded_parallel > 0.0 && async_threaded_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-many/thread",
           async_threaded_many_parallel / async_threaded_parallel);
  if (async_batch_parallel > 0.0 && async_get_ex_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-batch/batch", async_get_ex_batch_parallel / async_batch_parallel);
  if (async_get_ex_many_parallel > 0.0 && async_get_ex_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-batch/many", async_get_ex_batch_parallel / async_get_ex_many_parallel);
  if (async_get_ex_loop_parallel > 0.0 && async_get_ex_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-batch/loop", async_get_ex_batch_parallel / async_get_ex_loop_parallel);
  if (async_get_ex_loop_parallel > 0.0 && async_threaded_get_ex_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-get-ex-loop/loop",
           async_threaded_get_ex_loop_parallel / async_get_ex_loop_parallel);
  if (async_many_parallel > 0.0 && async_cache_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-many/many", async_cache_many_parallel / async_many_parallel);
  if (async_cache_many_parallel > 0.0 && async_cache_st_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st/cache", async_cache_st_many_parallel / async_cache_many_parallel);
  if (async_cache_many_parallel > 0.0 && async_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch/many", async_cache_batch_parallel / async_cache_many_parallel);
  if (async_cache_st_many_parallel > 0.0 && async_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch/many",
           async_cache_st_batch_parallel / async_cache_st_many_parallel);
  if (async_cache_batch_parallel > 0.0 && async_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch/batch",
           async_cache_st_batch_parallel / async_cache_batch_parallel);
  if (async_cache_batch_parallel > 0.0 && async_cache_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch-cb/batch",
           async_cache_batch_callback_parallel / async_cache_batch_parallel);
  if (async_cache_st_batch_parallel > 0.0 && async_cache_st_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch-cb/b",
           async_cache_st_batch_callback_parallel / async_cache_st_batch_parallel);
  if (async_cache_batch_callback_parallel > 0.0 && async_cache_st_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch-cb/cb",
           async_cache_st_batch_callback_parallel / async_cache_batch_callback_parallel);
  if (async_large_cache_batch_parallel > 0.0 && async_large_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-large-cache-st/batch",
           async_large_cache_st_batch_parallel / async_large_cache_batch_parallel);
  if (async_cache_batch_parallel > 0.0 && async_threaded_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-batch/b",
           async_threaded_cache_batch_parallel / async_cache_batch_parallel);
  if (async_cache_st_batch_parallel > 0.0 && async_threaded_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-b/b",
           async_threaded_cache_st_batch_parallel / async_cache_st_batch_parallel);
  if (async_threaded_cache_batch_parallel > 0.0 && async_threaded_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st/cache",
           async_threaded_cache_st_batch_parallel / async_threaded_cache_batch_parallel);
  if (async_cache_batch_parallel > 0.0 && async_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-loop/batch", async_cache_loop_parallel / async_cache_batch_parallel);
  if (async_cache_st_batch_parallel > 0.0 && async_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-loop/batch",
           async_cache_st_loop_parallel / async_cache_st_batch_parallel);
  if (async_cache_many_parallel > 0.0 && async_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-loop/many", async_cache_loop_parallel / async_cache_many_parallel);
  if (async_cache_st_many_parallel > 0.0 && async_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-loop/many",
           async_cache_st_loop_parallel / async_cache_st_many_parallel);
  if (async_cache_loop_parallel > 0.0 && async_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-loop/loop",
           async_cache_st_loop_parallel / async_cache_loop_parallel);
  if (async_cache_loop_parallel > 0.0 && async_threaded_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-loop/loop",
           async_threaded_cache_loop_parallel / async_cache_loop_parallel);
  if (async_cache_st_loop_parallel > 0.0 && async_threaded_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-l/l",
           async_threaded_cache_st_loop_parallel / async_cache_st_loop_parallel);
  if (async_threaded_cache_loop_parallel > 0.0 && async_threaded_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-l/t",
           async_threaded_cache_st_loop_parallel / async_threaded_cache_loop_parallel);
  if (async_batch_parallel > 0.0 && async_lowerbound_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-batch/batch", async_lowerbound_batch_parallel / async_batch_parallel);
  if (async_lowerbound_many_parallel > 0.0 && async_lowerbound_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-batch/many", async_lowerbound_batch_parallel / async_lowerbound_many_parallel);
  if (async_lowerbound_loop_parallel > 0.0 && async_lowerbound_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-batch/loop", async_lowerbound_batch_parallel / async_lowerbound_loop_parallel);
  if (async_lowerbound_loop_parallel > 0.0 && async_threaded_lowerbound_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-lower-loop/loop",
           async_threaded_lowerbound_loop_parallel / async_lowerbound_loop_parallel);
  if (blocking_serial > 0.0 && async_parallel > 0.0)
    printf("%-28s %8.3f\n", "async/blocking serial", async_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-many/blocking ser", async_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread/blocking ser", async_threaded_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-many/ser", async_threaded_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-batch/ser", async_threaded_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch/blocking serial", async_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-batch-cb/blocking ser", async_batch_callback_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_get_ex_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-batch/ser", async_get_ex_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_get_ex_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-many/ser", async_get_ex_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-many/ser", async_cache_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_st_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-many/ser", async_cache_st_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch/ser", async_cache_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch/ser", async_cache_st_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-batch-cb/ser",
           async_cache_batch_callback_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_st_batch_callback_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-batch-cb/ser",
           async_cache_st_batch_callback_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_cache_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-batch/ser",
           async_threaded_cache_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_cache_st_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-b/ser",
           async_threaded_cache_st_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-loop/ser", async_cache_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cache-st-loop/ser", async_cache_st_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_cache_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-loop/ser",
           async_threaded_cache_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_cache_st_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cache-st-l/ser",
           async_threaded_cache_st_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_lowerbound_batch_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-batch/ser", async_lowerbound_batch_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_lowerbound_many_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-many/ser", async_lowerbound_many_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop/blocking ser", async_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_get_ex_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-ex-loop/ser", async_get_ex_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_lowerbound_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-lower-loop/ser", async_lowerbound_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-loop/ser", async_threaded_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_get_ex_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-get-ex-loop/ser",
           async_threaded_get_ex_loop_parallel / blocking_serial);
  if (blocking_serial > 0.0 && async_threaded_lowerbound_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-lower-loop/ser",
           async_threaded_lowerbound_loop_parallel / blocking_serial);
  if (blocking_cursor_get_parallel > 0.0 && async_cursor_get_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-get/par", async_cursor_get_parallel / blocking_cursor_get_parallel);
  if (blocking_cursor_get_serial > 0.0 && async_cursor_get_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-get/ser", async_cursor_get_parallel / blocking_cursor_get_serial);
  if (blocking_cursor_get_parallel > 0.0 && async_cursor_get_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-setkey/par",
           async_cursor_get_setkey_parallel / blocking_cursor_get_parallel);
  if (blocking_cursor_get_serial > 0.0 && async_cursor_get_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-setkey/ser",
           async_cursor_get_setkey_parallel / blocking_cursor_get_serial);
  if (async_cursor_get_parallel > 0.0 && async_cursor_get_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-setkey/get",
           async_cursor_get_setkey_parallel / async_cursor_get_parallel);
  if (blocking_cursor_get_parallel > 0.0 && async_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-get-loop/par",
           async_cursor_get_loop_parallel / blocking_cursor_get_parallel);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-loop-from/par",
           async_cursor_get_loop_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_get_serial > 0.0 && async_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-get-loop/ser",
           async_cursor_get_loop_parallel / blocking_cursor_get_serial);
  if (blocking_cursor_scan_from_serial > 0.0 && async_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-loop-from/ser",
           async_cursor_get_loop_from_parallel / blocking_cursor_scan_from_serial);
  if (async_cursor_get_parallel > 0.0 && async_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-get-loop/get",
           async_cursor_get_loop_parallel / async_cursor_get_parallel);
  if (async_cursor_scan_from_parallel > 0.0 && async_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cget-loop-from/scan",
           async_cursor_get_loop_from_parallel / async_cursor_scan_from_parallel);
  if (blocking_cursor_get_parallel > 0.0 && async_threaded_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-loop/par",
           async_threaded_cursor_get_loop_parallel / blocking_cursor_get_parallel);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_threaded_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-from/par",
           async_threaded_cursor_get_loop_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_get_serial > 0.0 && async_threaded_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-loop/ser",
           async_threaded_cursor_get_loop_parallel / blocking_cursor_get_serial);
  if (blocking_cursor_scan_from_serial > 0.0 && async_threaded_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-from/ser",
           async_threaded_cursor_get_loop_from_parallel / blocking_cursor_scan_from_serial);
  if (async_cursor_get_parallel > 0.0 && async_threaded_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-loop/get",
           async_threaded_cursor_get_loop_parallel / async_cursor_get_parallel);
  if (async_cursor_get_loop_parallel > 0.0 && async_threaded_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-loop/loop",
           async_threaded_cursor_get_loop_parallel / async_cursor_get_loop_parallel);
  if (async_cursor_get_loop_from_parallel > 0.0 && async_threaded_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-from/loop",
           async_threaded_cursor_get_loop_from_parallel / async_cursor_get_loop_from_parallel);
  if (async_threaded_cursor_scan_from_parallel > 0.0 && async_threaded_cursor_get_loop_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cget-from/scan",
           async_threaded_cursor_get_loop_from_parallel / async_threaded_cursor_scan_from_parallel);
  if (async_cursor_get_parallel > 0.0 && async_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-batch/get", async_cursor_parallel / async_cursor_get_parallel);
  if (async_cursor_parallel > 0.0 && async_cursor_get_loop_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-get-loop/batch", async_cursor_get_loop_parallel / async_cursor_parallel);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-loop-from/par",
           async_loop_cursor_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_scan_from_serial > 0.0 && async_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-loop-from/ser",
           async_loop_cursor_from_parallel / blocking_cursor_scan_from_serial);
  if (async_cursor_scan_from_parallel > 0.0 && async_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-loop-from/scan",
           async_loop_cursor_from_parallel / async_cursor_scan_from_parallel);
  if (blocking_cursor_parallel > 0.0 && async_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor/blocking par", async_cursor_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor/blocking ser", async_cursor_parallel / blocking_cursor_serial);
  if (blocking_cursor_parallel > 0.0 && async_threaded_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cbatch/par",
           async_threaded_cursor_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_threaded_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cbatch/ser",
           async_threaded_cursor_parallel / blocking_cursor_serial);
  if (async_cursor_parallel > 0.0 && async_threaded_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cbatch/batch",
           async_threaded_cursor_parallel / async_cursor_parallel);
  if (blocking_cursor_parallel > 0.0 && async_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop-cursor/par", async_loop_cursor_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop-cursor/ser", async_loop_cursor_parallel / blocking_cursor_serial);
  if (blocking_cursor_parallel > 0.0 && async_loop_cursor_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop-csetkey/par",
           async_loop_cursor_setkey_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_loop_cursor_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop-csetkey/ser",
           async_loop_cursor_setkey_parallel / blocking_cursor_serial);
  if (async_loop_cursor_from_parallel > 0.0 && async_loop_cursor_setkey_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-loop-csetkey/from",
           async_loop_cursor_setkey_parallel / async_loop_cursor_from_parallel);
  if (blocking_cursor_parallel > 0.0 && async_threaded_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cursor-loop/par",
           async_threaded_loop_cursor_parallel / blocking_cursor_parallel);
  if (blocking_cursor_serial > 0.0 && async_threaded_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cursor-loop/ser",
           async_threaded_loop_cursor_parallel / blocking_cursor_serial);
  if (async_cursor_parallel > 0.0 && async_threaded_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cursor/batch",
           async_threaded_loop_cursor_parallel / async_cursor_parallel);
  if (async_loop_cursor_parallel > 0.0 && async_threaded_loop_cursor_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cursor/loop",
           async_threaded_loop_cursor_parallel / async_loop_cursor_parallel);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_threaded_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cur-from/par",
           async_threaded_loop_cursor_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_scan_from_serial > 0.0 && async_threaded_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cur-from/ser",
           async_threaded_loop_cursor_from_parallel / blocking_cursor_scan_from_serial);
  if (async_loop_cursor_from_parallel > 0.0 && async_threaded_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cur-from/loop",
           async_threaded_loop_cursor_from_parallel / async_loop_cursor_from_parallel);
  if (async_threaded_cursor_scan_from_parallel > 0.0 && async_threaded_loop_cursor_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-cur-from/scan",
           async_threaded_loop_cursor_from_parallel / async_threaded_cursor_scan_from_parallel);
  if (blocking_cursor_scan_parallel > 0.0 && async_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-scan/par", async_cursor_scan_parallel / blocking_cursor_scan_parallel);
  if (blocking_cursor_scan_serial > 0.0 && async_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-scan/ser", async_cursor_scan_parallel / blocking_cursor_scan_serial);
  if (blocking_cursor_scan_parallel > 0.0 && async_threaded_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan/par",
           async_threaded_cursor_scan_parallel / blocking_cursor_scan_parallel);
  if (blocking_cursor_scan_serial > 0.0 && async_threaded_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan/ser",
           async_threaded_cursor_scan_parallel / blocking_cursor_scan_serial);
  if (async_cursor_scan_parallel > 0.0 && async_threaded_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan/scan",
           async_threaded_cursor_scan_parallel / async_cursor_scan_parallel);
  if (async_loop_cursor_parallel > 0.0 && async_cursor_scan_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-scan/cursor-loop", async_cursor_scan_parallel / async_loop_cursor_parallel);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-scan-from/par", async_cursor_scan_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_scan_from_serial > 0.0 && async_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-scan-from/ser", async_cursor_scan_from_parallel / blocking_cursor_scan_from_serial);
  if (blocking_cursor_scan_from_parallel > 0.0 && async_threaded_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan-from/par",
           async_threaded_cursor_scan_from_parallel / blocking_cursor_scan_from_parallel);
  if (blocking_cursor_scan_from_serial > 0.0 && async_threaded_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan-from/ser",
           async_threaded_cursor_scan_from_parallel / blocking_cursor_scan_from_serial);
  if (async_cursor_scan_from_parallel > 0.0 && async_threaded_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-thread-scan-from/scan",
           async_threaded_cursor_scan_from_parallel / async_cursor_scan_from_parallel);
  if (async_cursor_scan_parallel > 0.0 && async_cursor_scan_from_parallel > 0.0)
    printf("%-28s %8.3f\n", "async-scan-from/scan", async_cursor_scan_from_parallel / async_cursor_scan_parallel);
  if (blocking_put > 0.0 && async_put > 0.0)
    printf("%-28s %8.3f\n", "async-put/blocking put", async_put / blocking_put);
  if (blocking_put > 0.0 && async_put_batch > 0.0)
    printf("%-28s %8.3f\n", "async-put-batch/blocking", async_put_batch / blocking_put);
  if (blocking_put > 0.0 && async_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-put-loop/blocking", async_put_loop / blocking_put);
  if (async_put > 0.0 && async_put_batch > 0.0)
    printf("%-28s %8.3f\n", "async-put-batch/async-put", async_put_batch / async_put);
  if (async_put > 0.0 && async_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-put-loop/async-put", async_put_loop / async_put);
  if (async_put_batch > 0.0 && async_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-put-loop/batch", async_put_loop / async_put_batch);
  if (blocking_cursor_put > 0.0 && async_cursor_put_batch > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-put-batch/block", async_cursor_put_batch / blocking_cursor_put);
  if (blocking_cursor_put > 0.0 && async_cursor_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-put-loop/block", async_cursor_put_loop / blocking_cursor_put);
  if (async_cursor_put > 0.0 && async_cursor_put_batch > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-put-batch/async", async_cursor_put_batch / async_cursor_put);
  if (async_cursor_put > 0.0 && async_cursor_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-put-loop/async", async_cursor_put_loop / async_cursor_put);
  if (async_cursor_put_batch > 0.0 && async_cursor_put_loop > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-put-loop/batch", async_cursor_put_loop / async_cursor_put_batch);
  if (blocking_replace_rate > 0.0 && async_replace_rate > 0.0)
    printf("%-28s %8.3f\n", "async-replace/blocking", async_replace_rate / blocking_replace_rate);
  if (blocking_replace_rate > 0.0 && async_replace_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-batch/blocking", async_replace_batch_rate / blocking_replace_rate);
  if (blocking_replace_rate > 0.0 && async_replace_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-loop/blocking", async_replace_loop_rate / blocking_replace_rate);
  if (async_replace_rate > 0.0 && async_replace_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-batch/async", async_replace_batch_rate / async_replace_rate);
  if (async_replace_rate > 0.0 && async_replace_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-loop/async", async_replace_loop_rate / async_replace_rate);
  if (async_replace_batch_rate > 0.0 && async_replace_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-loop/batch", async_replace_loop_rate / async_replace_batch_rate);
  if (blocking_replace_ex_rate > 0.0 && async_replace_ex_rate > 0.0)
    printf("%-28s %8.3f\n", "async-replace-ex/blocking", async_replace_ex_rate / blocking_replace_ex_rate);
  if (blocking_replace_ex_rate > 0.0 && async_replace_ex_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-ex-batch/block", async_replace_ex_batch_rate / blocking_replace_ex_rate);
  if (blocking_replace_ex_rate > 0.0 && async_replace_ex_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-ex-loop/block", async_replace_ex_loop_rate / blocking_replace_ex_rate);
  if (async_replace_ex_rate > 0.0 && async_replace_ex_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-ex-batch/async", async_replace_ex_batch_rate / async_replace_ex_rate);
  if (async_replace_ex_rate > 0.0 && async_replace_ex_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-ex-loop/async", async_replace_ex_loop_rate / async_replace_ex_rate);
  if (async_replace_ex_batch_rate > 0.0 && async_replace_ex_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-ex-loop/batch", async_replace_ex_loop_rate / async_replace_ex_batch_rate);
  if (blocking_replace_delete_rate > 0.0 && async_replace_delete_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del/blocking", async_replace_delete_rate / blocking_replace_delete_rate);
  if (blocking_replace_delete_rate > 0.0 && async_replace_delete_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del-batch/block", async_replace_delete_batch_rate / blocking_replace_delete_rate);
  if (blocking_replace_delete_rate > 0.0 && async_replace_delete_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del-loop/block", async_replace_delete_loop_rate / blocking_replace_delete_rate);
  if (async_replace_delete_rate > 0.0 && async_replace_delete_batch_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del-batch/async", async_replace_delete_batch_rate / async_replace_delete_rate);
  if (async_replace_delete_rate > 0.0 && async_replace_delete_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del-loop/async", async_replace_delete_loop_rate / async_replace_delete_rate);
  if (async_replace_delete_batch_rate > 0.0 && async_replace_delete_loop_rate > 0.0)
    printf("%-28s %8.3f\n", "async-repl-del-loop/batch", async_replace_delete_loop_rate / async_replace_delete_batch_rate);
  if (blocking_del > 0.0 && async_del > 0.0)
    printf("%-28s %8.3f\n", "async-del/blocking del", async_del / blocking_del);
  if (blocking_del > 0.0 && async_del_batch > 0.0)
    printf("%-28s %8.3f\n", "async-del-batch/blocking", async_del_batch / blocking_del);
  if (blocking_del > 0.0 && async_del_loop > 0.0)
    printf("%-28s %8.3f\n", "async-del-loop/blocking", async_del_loop / blocking_del);
  if (async_del > 0.0 && async_del_batch > 0.0)
    printf("%-28s %8.3f\n", "async-del-batch/async-del", async_del_batch / async_del);
  if (async_del > 0.0 && async_del_loop > 0.0)
    printf("%-28s %8.3f\n", "async-del-loop/async-del", async_del_loop / async_del);
  if (async_del_batch > 0.0 && async_del_loop > 0.0)
    printf("%-28s %8.3f\n", "async-del-loop/batch", async_del_loop / async_del_batch);
  if (blocking_cursor_del > 0.0 && async_cursor_del > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-del/block", async_cursor_del / blocking_cursor_del);
  if (blocking_cursor_del > 0.0 && async_cursor_del_loop > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-del-loop/block", async_cursor_del_loop / blocking_cursor_del);
  if (async_del > 0.0 && async_cursor_del > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-del/async-del", async_cursor_del / async_del);
  if (async_cursor_del > 0.0 && async_cursor_del_loop > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-del-loop/async", async_cursor_del_loop / async_cursor_del);
  if (blocking_cursor_range_del > 0.0 && async_cursor_range_del > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-range/block", async_cursor_range_del / blocking_cursor_range_del);
  if (blocking_cursor_bunch_del > 0.0 && async_cursor_bunch_del > 0.0)
    printf("%-28s %8.3f\n", "async-cursor-bunch/block", async_cursor_bunch_del / blocking_cursor_bunch_del);

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
