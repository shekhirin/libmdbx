/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Focused smoke coverage for the public asynchronous executor API.
 */

#include "mdbx.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ITEM_COUNT 64
#define LARGE_ITEM_COUNT 4
#define LARGE_VALUE_BYTES 10000
#define ASYNC_READ_BATCH_COUNT 64
#define ASYNC_READ_BATCH_VALUE_BYTES 768

struct async_probe {
  unsigned calls;
};

struct async_block_probe {
#if defined(_WIN32) || defined(_WIN64)
  HANDLE release_event;
#else
  int release_pipe[2];
#endif
  unsigned calls;
};

struct enum_probe {
  unsigned calls;
  bool saw_target;
};

struct reader_probe {
  unsigned calls;
};

struct gc_probe {
  unsigned calls;
};

struct chk_probe {
  unsigned stage_begin_calls;
  unsigned stage_end_calls;
};

struct preserve_probe {
  unsigned calls;
};

struct scan_probe {
  uint64_t target;
  unsigned calls;
};

struct batch_probe {
  unsigned calls;
  size_t pairs;
};

struct cursor_get_loop_probe {
  unsigned calls;
};

struct cursor_get_loop_abort_probe {
  unsigned calls;
  size_t fail_index;
  int errcode;
};

struct get_batch_probe {
  unsigned calls;
  size_t successes;
};

struct get_ex_batch_probe {
  unsigned calls;
  size_t successes;
  size_t values;
};

struct get_equal_or_great_batch_probe {
  unsigned calls;
  size_t successes;
  size_t greater_results;
};

struct cache_batch_probe {
  unsigned calls;
  size_t successes;
  size_t hits;
};

struct get_loop_probe {
  uint64_t key;
  size_t keys;
  size_t results;
};

struct cache_loop_probe {
  uint64_t key;
  uint64_t offset;
  size_t keys;
  size_t results;
  size_t hits;
};

struct async_read_loop_probe {
  uint64_t key;
  size_t keys;
  size_t results;
  size_t hits;
  uint8_t (*values)[ASYNC_READ_BATCH_VALUE_BYTES];
  int expected_result;
  bool reverse;
};

struct async_large_cursor_probe {
  size_t results;
  size_t batches;
  size_t target;
};

struct async_dupsort_loop_probe {
  uint64_t keys[3];
  uint64_t values[3];
  int expected_results[3];
  size_t keys_seen;
  size_t results_seen;
};

struct async_dupsort_cache_loop_probe {
  uint64_t keys[2];
  int expected_errcodes[2];
  MDBX_cache_status_t expected_statuses[2];
  size_t keys_seen;
  size_t results_seen;
};

struct async_dupsort_cursor_probe {
  uint64_t keys[4];
  uint64_t values[4];
  size_t items_seen;
};

struct get_ex_loop_probe {
  uint64_t key;
  size_t keys;
  size_t results;
  size_t values;
};

struct get_equal_or_great_loop_probe {
  uint64_t key;
  uint8_t greater_key[sizeof(uint64_t) + 1];
  size_t keys;
  size_t data;
  size_t results;
  size_t greater_results;
};

struct put_loop_probe {
  uint64_t keys[4];
  uint64_t values[4];
  size_t items;
  size_t results;
};

struct del_loop_probe {
  uint64_t keys[3];
  size_t keys_seen;
  size_t results;
};

struct replace_loop_probe {
  uint64_t keys[3];
  uint64_t new_values[3];
  uint64_t old_buffers[3];
  uint64_t expected_old[3];
  size_t items;
  size_t results;
};

struct replace_delete_loop_probe {
  uint64_t keys[3];
  uint64_t old_buffers[3];
  uint64_t expected_old[3];
  size_t items;
  size_t results;
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

#define REQUIRE(condition, message)                                                                                    \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      rc = fail_msg((message), __FILE__, __LINE__);                                                                    \
      goto bailout;                                                                                                    \
    }                                                                                                                  \
  } while (0)

#define CHECK_OP(op)                                                                                                   \
  do {                                                                                                                 \
    rc = wait_success(#op, &(op), __FILE__, __LINE__);                                                                 \
    if (rc != MDBX_SUCCESS)                                                                                            \
      goto bailout;                                                                                                    \
  } while (0)

static unsigned long long smoke_run_id(void) { return (unsigned long long)(uintptr_t)&smoke_run_id; }

static bool env_enabled(const char *name) {
  const char *const value = getenv(name);
  return value && value[0] && strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
         strcmp(value, "FALSE") != 0 && strcmp(value, "no") != 0 && strcmp(value, "NO") != 0;
}

static int set_env_var(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
  return _putenv_s(name, value) == 0 ? MDBX_SUCCESS : fail_msg("_putenv_s failed", __FILE__, __LINE__);
#else
  return setenv(name, value, 1) == 0 ? MDBX_SUCCESS : fail_rc("setenv", errno, __FILE__, __LINE__);
#endif
}

static int unset_env_var(const char *name) {
#if defined(_WIN32) || defined(_WIN64)
  return _putenv_s(name, "") == 0 ? MDBX_SUCCESS : fail_msg("_putenv_s failed", __FILE__, __LINE__);
#else
  return unsetenv(name) == 0 ? MDBX_SUCCESS : fail_rc("unsetenv", errno, __FILE__, __LINE__);
#endif
}

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

static int async_probe_func(MDBX_env *env, void *context) {
  struct async_probe *const probe = (struct async_probe *)context;
  if (!env || !probe)
    return MDBX_EINVAL;
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int async_block_probe_func(MDBX_env *env, void *context) {
  struct async_block_probe *const probe = (struct async_block_probe *)context;
  if (!env || !probe)
    return MDBX_EINVAL;
  probe->calls += 1;
#if defined(_WIN32) || defined(_WIN64)
  return WaitForSingleObject(probe->release_event, INFINITE) == WAIT_OBJECT_0 ? MDBX_SUCCESS : MDBX_EIO;
#else
  char byte = 0;
  ssize_t bytes;
  do {
    bytes = read(probe->release_pipe[0], &byte, 1);
  } while (bytes < 0 && errno == EINTR);
  return bytes == 1 ? MDBX_SUCCESS : MDBX_EIO;
#endif
}

static int async_block_probe_prepare(struct async_block_probe *probe) {
  if (!probe)
    return MDBX_EINVAL;
  memset(probe, 0, sizeof(*probe));
#if defined(_WIN32) || defined(_WIN64)
  probe->release_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  return probe->release_event ? MDBX_SUCCESS : MDBX_EIO;
#else
  probe->release_pipe[0] = -1;
  probe->release_pipe[1] = -1;
  return pipe(probe->release_pipe) == 0 ? MDBX_SUCCESS : MDBX_EIO;
#endif
}

static int async_block_probe_release(struct async_block_probe *probe) {
  if (!probe)
    return MDBX_EINVAL;
#if defined(_WIN32) || defined(_WIN64)
  return SetEvent(probe->release_event) ? MDBX_SUCCESS : MDBX_EIO;
#else
  const char byte = 1;
  ssize_t bytes;
  do {
    bytes = write(probe->release_pipe[1], &byte, 1);
  } while (bytes < 0 && errno == EINTR);
  return bytes == 1 ? MDBX_SUCCESS : MDBX_EIO;
#endif
}

static void async_block_probe_close(struct async_block_probe *probe) {
  if (!probe)
    return;
#if defined(_WIN32) || defined(_WIN64)
  if (probe->release_event) {
    CloseHandle(probe->release_event);
    probe->release_event = NULL;
  }
#else
  if (probe->release_pipe[0] >= 0) {
    close(probe->release_pipe[0]);
    probe->release_pipe[0] = -1;
  }
  if (probe->release_pipe[1] >= 0) {
    close(probe->release_pipe[1]);
    probe->release_pipe[1] = -1;
  }
#endif
}

static int enum_probe_func(void *ctx, const MDBX_txn *txn, const MDBX_val *name, MDBX_db_flags_t flags,
                           const struct MDBX_stat *stat, MDBX_dbi dbi) {
  (void)txn;
  (void)flags;
  static const char target[] = "async-enum-target";
  struct enum_probe *const probe = (struct enum_probe *)ctx;
  if (!probe || !name)
    return MDBX_EINVAL;
  probe->calls += 1;
  if (name->iov_len == sizeof(target) - 1 && memcmp(name->iov_base, target, sizeof(target) - 1) == 0) {
    probe->saw_target = true;
    if (!stat || stat->ms_entries != 1 || dbi == 0)
      return MDBX_PROBLEM;
  }
  return MDBX_SUCCESS;
}

static int reader_probe_func(void *ctx, int num, int slot, mdbx_pid_t pid, mdbx_tid_t thread, uint64_t txnid,
                             uint64_t lag, size_t bytes_used, size_t bytes_retained) {
  (void)num;
  (void)slot;
  (void)thread;
  (void)txnid;
  (void)lag;
  (void)bytes_used;
  (void)bytes_retained;
  struct reader_probe *const probe = (struct reader_probe *)ctx;
  if (!probe || !pid)
    return MDBX_EINVAL;
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int hsr_probe_func(const MDBX_env *env, const MDBX_txn *txn, mdbx_pid_t pid, mdbx_tid_t tid, uint64_t laggard,
                          unsigned gap, size_t space, int retry) {
  (void)env;
  (void)txn;
  (void)pid;
  (void)tid;
  (void)laggard;
  (void)gap;
  (void)space;
  (void)retry;
  return -1;
}

static int gc_probe_func(void *ctx, const MDBX_txn *txn, uint64_t span_txnid, size_t span_pgno, size_t span_length,
                         bool span_is_reclaimable) {
  (void)span_txnid;
  (void)span_pgno;
  (void)span_is_reclaimable;
  struct gc_probe *const probe = (struct gc_probe *)ctx;
  if (!probe || !txn || !span_length)
    return MDBX_EINVAL;
  probe->calls += 1;
  return MDBX_RESULT_FALSE;
}

static struct chk_probe *active_chk_probe;

static int chk_probe_stage_begin(MDBX_chk_context_t *ctx, MDBX_chk_stage_t stage) {
  (void)stage;
  if (!ctx || !ctx->env || !active_chk_probe)
    return MDBX_EINVAL;
  active_chk_probe->stage_begin_calls += 1;
  return MDBX_SUCCESS;
}

static int chk_probe_stage_end(MDBX_chk_context_t *ctx, MDBX_chk_stage_t stage, int err) {
  (void)stage;
  if (!ctx || !ctx->env || !active_chk_probe)
    return MDBX_EINVAL;
  active_chk_probe->stage_end_calls += 1;
  return err;
}

static int preserve_probe_func(void *context, MDBX_val *target, const void *src, size_t bytes) {
  struct preserve_probe *const probe = (struct preserve_probe *)context;
  if (!probe || !target || !src)
    return MDBX_EINVAL;
  probe->calls += 1;
  if (target->iov_len < bytes) {
    target->iov_base = NULL;
    target->iov_len = bytes;
    return MDBX_RESULT_TRUE;
  }
  memcpy(target->iov_base, src, bytes);
  target->iov_len = bytes;
  return MDBX_SUCCESS;
}

static int async_custom_cmp(const MDBX_val *a, const MDBX_val *b) {
  if (!a || !b)
    return 0;
  const size_t common = a->iov_len < b->iov_len ? a->iov_len : b->iov_len;
  const int diff = common ? memcmp(a->iov_base, b->iov_base, common) : 0;
  if (diff)
    return diff;
  return (a->iov_len > b->iov_len) - (a->iov_len < b->iov_len);
}

static int scan_probe_func(void *context, MDBX_val *key, MDBX_val *value, void *arg) {
  (void)arg;
  struct scan_probe *const probe = (struct scan_probe *)context;
  if (!probe || !key || !value || key->iov_len != sizeof(uint64_t) || value->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = 0;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, value->iov_base, sizeof(actual_value));
  if (actual_value != expected_value(actual_key))
    return MDBX_PROBLEM;
  probe->calls += 1;
  return actual_key == probe->target ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

static int batch_probe_func(void *context, const MDBX_val *pairs, size_t count) {
  struct batch_probe *const probe = (struct batch_probe *)context;
  if (!probe || !pairs || count == 0 || (count & 1))
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; i += 2) {
    if (pairs[i].iov_len != sizeof(uint64_t) || pairs[i + 1].iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_key = 0;
    uint64_t actual_value = 0;
    memcpy(&actual_key, pairs[i].iov_base, sizeof(actual_key));
    memcpy(&actual_value, pairs[i + 1].iov_base, sizeof(actual_value));
    if (actual_key >= ITEM_COUNT || actual_value != expected_value(actual_key))
      return MDBX_PROBLEM;
  }
  probe->calls += 1;
  probe->pairs += count / 2;
  return MDBX_SUCCESS;
}

static int cursor_get_loop_probe_func(void *context, size_t index, const MDBX_val *key,
                                      const MDBX_val *data) {
  (void)index;
  struct cursor_get_loop_probe *const probe = (struct cursor_get_loop_probe *)context;
  if (!probe || !key || !data || key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = 0;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key >= ITEM_COUNT || actual_value != expected_value(actual_key))
    return MDBX_PROBLEM;
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int cursor_get_loop_abort_probe_func(void *context, size_t index, const MDBX_val *key,
                                            const MDBX_val *data) {
  struct cursor_get_loop_abort_probe *const probe = (struct cursor_get_loop_abort_probe *)context;
  if (!probe || !key || !data || key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = 0;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key >= ITEM_COUNT || actual_value != expected_value(actual_key))
    return MDBX_PROBLEM;
  probe->calls += 1;
  return index == probe->fail_index ? probe->errcode : MDBX_SUCCESS;
}

static int get_batch_probe_func(void *context, const MDBX_val keys[], MDBX_val data[], const int results[],
                                size_t count) {
  struct get_batch_probe *const probe = (struct get_batch_probe *)context;
  if (!probe || !keys || !data || !results)
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; ++i) {
    if (results[i] != MDBX_SUCCESS || keys[i].iov_len != sizeof(uint64_t) || data[i].iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_key = 0;
    uint64_t actual_value = 0;
    memcpy(&actual_key, keys[i].iov_base, sizeof(actual_key));
    memcpy(&actual_value, data[i].iov_base, sizeof(actual_value));
    if (actual_value != expected_value(actual_key))
      return MDBX_PROBLEM;
    probe->successes += 1;
  }
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int get_ex_batch_probe_func(void *context, MDBX_val keys[], MDBX_val data[], const size_t values_counts[],
                                   const int results[], size_t count) {
  struct get_ex_batch_probe *const probe = (struct get_ex_batch_probe *)context;
  if (!probe || !keys || !data || !values_counts || !results)
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; ++i) {
    if (results[i] != MDBX_SUCCESS || values_counts[i] != 1 || keys[i].iov_len != sizeof(uint64_t) ||
        data[i].iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    uint64_t actual_value = 0;
    memcpy(&actual_key, keys[i].iov_base, sizeof(actual_key));
    memcpy(&actual_value, data[i].iov_base, sizeof(actual_value));
    if (actual_value != expected_value(actual_key))
      return MDBX_PROBLEM;
    probe->successes += 1;
    probe->values += values_counts[i];
  }
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int get_equal_or_great_batch_probe_func(void *context, MDBX_val keys[], MDBX_val data[], const int results[],
                                               size_t count) {
  struct get_equal_or_great_batch_probe *const probe = (struct get_equal_or_great_batch_probe *)context;
  if (!probe || !keys || !data || !results)
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; ++i) {
    if (results[i] != MDBX_SUCCESS && results[i] != MDBX_RESULT_TRUE)
      return MDBX_PROBLEM;
    if (keys[i].iov_len != sizeof(uint64_t) || data[i].iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    uint64_t actual_value = 0;
    memcpy(&actual_key, keys[i].iov_base, sizeof(actual_key));
    memcpy(&actual_value, data[i].iov_base, sizeof(actual_value));
    if (actual_value != expected_value(actual_key))
      return MDBX_PROBLEM;
    probe->successes += 1;
    if (results[i] == MDBX_RESULT_TRUE)
      probe->greater_results += 1;
  }
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int cache_batch_probe_func(void *context, const MDBX_val keys[], MDBX_val data[],
                                  const MDBX_cache_result_t results[], size_t count) {
  struct cache_batch_probe *const probe = (struct cache_batch_probe *)context;
  if (!probe || !keys || !data || !results)
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; ++i) {
    if (results[i].errcode != MDBX_SUCCESS || keys[i].iov_len != sizeof(uint64_t) ||
        data[i].iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_key = 0;
    uint64_t actual_value = 0;
    memcpy(&actual_key, keys[i].iov_base, sizeof(actual_key));
    memcpy(&actual_value, data[i].iov_base, sizeof(actual_value));
    if (actual_value != expected_value(actual_key))
      return MDBX_PROBLEM;
    probe->successes += 1;
    if (results[i].status == MDBX_CACHE_HIT)
      probe->hits += 1;
  }
  probe->calls += 1;
  return MDBX_SUCCESS;
}

static int get_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct get_loop_probe *const probe = (struct get_loop_probe *)context;
  if (!probe || !key || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  probe->key = (uint64_t)index;
  *key = val(&probe->key, sizeof(probe->key));
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int get_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data, int result) {
  struct get_loop_probe *const probe = (struct get_loop_probe *)context;
  if (!probe || !key || !data || result != MDBX_SUCCESS || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index)
    return MDBX_PROBLEM;
  if (data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_value = 0;
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_value != expected_value((uint64_t)index))
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int cache_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct cache_loop_probe *const probe = (struct cache_loop_probe *)context;
  if (!probe || !key || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  probe->key = probe->offset + (uint64_t)index;
  *key = val(&probe->key, sizeof(probe->key));
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int cache_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                  MDBX_cache_result_t result) {
  struct cache_loop_probe *const probe = (struct cache_loop_probe *)context;
  if (!probe || !key || !data || result.errcode != MDBX_SUCCESS || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  const uint64_t expected_key = probe->offset + (uint64_t)index;
  if (actual_key != expected_key || actual_value != expected_value(expected_key))
    return MDBX_PROBLEM;
  if (result.status == MDBX_CACHE_HIT)
    probe->hits += 1;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  probe->key = (uint64_t)index;
  *key = val(&probe->key, sizeof(probe->key));
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int async_read_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                       const MDBX_val *data, int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data || result != probe->expected_result ||
      index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (key->iov_len != sizeof(uint64_t) || data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      memcmp(data->iov_base, probe->values[index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int expect_large_value(const MDBX_val *data, uint64_t key, const char *file, int line);

static int async_read_cache_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                             const MDBX_val *data, MDBX_cache_result_t result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data || result.errcode != probe->expected_result ||
      index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (result.errcode != MDBX_SUCCESS) {
    if (result.status != MDBX_CACHE_ERROR || key->iov_len != sizeof(uint64_t) ||
        data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      memcmp(data->iov_base, probe->values[index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  if (result.status == MDBX_CACHE_HIT)
    probe->hits += 1;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_cache_loop_result_func(void *context, size_t index,
                                                   const MDBX_val *key,
                                                   const MDBX_val *data,
                                                   MDBX_cache_result_t result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || !data || result.errcode != probe->expected_result ||
      index >= LARGE_ITEM_COUNT)
    return MDBX_PROBLEM;
  if (result.errcode != MDBX_SUCCESS) {
    if (result.status != MDBX_CACHE_ERROR || key->iov_len != sizeof(uint64_t) ||
        data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      expect_large_value(data, actual_key, __FILE__, __LINE__) != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  if (result.status == MDBX_CACHE_HIT)
    probe->hits += 1;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_loop_result_func(void *context, size_t index,
                                             const MDBX_val *key,
                                             const MDBX_val *data,
                                             int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || !data || result != probe->expected_result ||
      index >= LARGE_ITEM_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (key->iov_len != sizeof(uint64_t) || data->iov_base != NULL ||
        data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      expect_large_value(data, actual_key, __FILE__, __LINE__) != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_get_ex_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                              const MDBX_val *data, size_t values_count, int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data || result != probe->expected_result ||
      index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (values_count != 0 || key->iov_len != sizeof(uint64_t) ||
        data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (values_count != 1)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      memcmp(data->iov_base, probe->values[index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_get_ex_loop_result_func(void *context, size_t index,
                                                    const MDBX_val *key,
                                                    const MDBX_val *data,
                                                    size_t values_count,
                                                    int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || !data || result != probe->expected_result ||
      index >= LARGE_ITEM_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (values_count != 0 || key->iov_len != sizeof(uint64_t) ||
        data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (values_count != 1 || key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      expect_large_value(data, actual_key, __FILE__, __LINE__) != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct async_dupsort_loop_probe *const probe = (struct async_dupsort_loop_probe *)context;
  if (!probe || !key || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  probe->keys_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                          const MDBX_val *data, int result) {
  struct async_dupsort_loop_probe *const probe = (struct async_dupsort_loop_probe *)context;
  if (!probe || !key || !data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]) ||
      result != probe->expected_results[index])
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != probe->keys[index])
    return MDBX_PROBLEM;
  if (result == MDBX_SUCCESS) {
    if (data->iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_value = UINT64_MAX;
    memcpy(&actual_value, data->iov_base, sizeof(actual_value));
    if (actual_value != probe->values[index])
      return MDBX_PROBLEM;
  } else if (data->iov_base != NULL || data->iov_len != 0) {
    return MDBX_PROBLEM;
  }
  probe->results_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_get_ex_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                                 const MDBX_val *data, size_t values_count,
                                                 int result) {
  struct async_dupsort_loop_probe *const probe = (struct async_dupsort_loop_probe *)context;
  const size_t expected_counts[3] = {2, 0, 1};
  if (!probe || !key || !data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]) ||
      result != probe->expected_results[index] || values_count != expected_counts[index])
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != probe->keys[index])
    return MDBX_PROBLEM;
  if (result == MDBX_SUCCESS) {
    if (data->iov_len != sizeof(uint64_t))
      return MDBX_PROBLEM;
    uint64_t actual_value = UINT64_MAX;
    memcpy(&actual_value, data->iov_base, sizeof(actual_value));
    if (actual_value != probe->values[index])
      return MDBX_PROBLEM;
  } else if (data->iov_base != NULL || data->iov_len != 0) {
    return MDBX_PROBLEM;
  }
  probe->results_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_cache_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct async_dupsort_cache_loop_probe *const probe =
      (struct async_dupsort_cache_loop_probe *)context;
  if (!probe || !key || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  probe->keys_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_cache_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                                const MDBX_val *data, MDBX_cache_result_t result) {
  struct async_dupsort_cache_loop_probe *const probe =
      (struct async_dupsort_cache_loop_probe *)context;
  if (!probe || !key || !data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]) ||
      result.errcode != probe->expected_errcodes[index] ||
      result.status != probe->expected_statuses[index])
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != probe->keys[index])
    return MDBX_PROBLEM;
  if (data->iov_base != NULL || data->iov_len != 0)
    return MDBX_PROBLEM;
  probe->results_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_cursor_loop_func(void *context, size_t index, const MDBX_val *key,
                                          const MDBX_val *data) {
  struct async_dupsort_cursor_probe *const probe =
      (struct async_dupsort_cursor_probe *)context;
  if (!probe || !key || !data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  uint64_t actual_value = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key != probe->keys[index] || actual_value != probe->values[index])
    return MDBX_PROBLEM;
  probe->items_seen += 1;
  return MDBX_SUCCESS;
}

static int async_dupsort_cursor_scan_func(void *context, MDBX_val *key, MDBX_val *data,
                                          void *arg) {
  struct async_dupsort_cursor_probe *const probe =
      (struct async_dupsort_cursor_probe *)context;
  (void)arg;
  if (!probe || !key || !data)
    return MDBX_PROBLEM;
  if (probe->items_seen >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  const size_t index = probe->items_seen;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  uint64_t actual_value = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key != probe->keys[index] || actual_value != probe->values[index])
    return MDBX_PROBLEM;
  probe->items_seen += 1;
  return probe->items_seen == sizeof(probe->keys) / sizeof(probe->keys[0])
             ? MDBX_SUCCESS
             : MDBX_RESULT_FALSE;
}

static int async_read_lowerbound_loop_data_func(void *context, size_t index, const MDBX_val *key,
                                                MDBX_val *data) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || !data || index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  *data = val(NULL, 0);
  return MDBX_SUCCESS;
}

static int async_read_lowerbound_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                                  const MDBX_val *data, int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data || result != probe->expected_result ||
      index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (key->iov_len != sizeof(uint64_t) || data->iov_base != NULL || data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      memcmp(data->iov_base, probe->values[index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_lowerbound_loop_result_func(void *context, size_t index,
                                                        const MDBX_val *key,
                                                        const MDBX_val *data,
                                                        int result) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !key || !data || result != probe->expected_result ||
      index >= LARGE_ITEM_COUNT)
    return MDBX_PROBLEM;
  if (result != MDBX_SUCCESS) {
    if (key->iov_len != sizeof(uint64_t) || data->iov_base != NULL ||
        data->iov_len != 0)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != index)
      return MDBX_PROBLEM;
    probe->results += 1;
    return MDBX_SUCCESS;
  }
  if (key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != index ||
      expect_large_value(data, actual_key, __FILE__, __LINE__) != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_cursor_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                              const MDBX_val *data) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data || index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  const size_t ascending_index = probe->hits + index;
  const size_t expected_index = probe->reverse
                                    ? ASYNC_READ_BATCH_COUNT - 1 - ascending_index
                                    : ascending_index;
  if (expected_index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != expected_index ||
      memcmp(data->iov_base, probe->values[expected_index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_cursor_batches_result_func(void *context, const MDBX_val *pairs,
                                                 size_t count) {
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !pairs || count == 0 || (count & 1))
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; i += 2) {
    const size_t expected_index = probe->hits + probe->results;
    if (expected_index >= ASYNC_READ_BATCH_COUNT)
      return MDBX_PROBLEM;
    const MDBX_val *const key = &pairs[i];
    const MDBX_val *const data = &pairs[i + 1];
    if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
      return MDBX_PROBLEM;
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    if (actual_key != expected_index ||
        memcmp(data->iov_base, probe->values[expected_index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
      return MDBX_PROBLEM;
    probe->results += 1;
  }
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int async_read_cursor_scan_result_func(void *context, MDBX_val *key, MDBX_val *data,
                                              void *arg) {
  (void)arg;
  struct async_read_loop_probe *const probe = (struct async_read_loop_probe *)context;
  if (!probe || !probe->values || !key || !data)
    return MDBX_PROBLEM;
  const size_t ascending_index = probe->hits + probe->results;
  const size_t expected_index = probe->reverse
                                    ? ASYNC_READ_BATCH_COUNT - 1 - ascending_index
                                    : ascending_index;
  if (expected_index >= ASYNC_READ_BATCH_COUNT)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != ASYNC_READ_BATCH_VALUE_BYTES)
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != expected_index ||
      memcmp(data->iov_base, probe->values[expected_index], ASYNC_READ_BATCH_VALUE_BYTES) != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return probe->results == ASYNC_READ_BATCH_COUNT - probe->hits ? MDBX_RESULT_TRUE
                                                                : MDBX_RESULT_FALSE;
}

static int put_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *data) {
  struct put_loop_probe *const probe = (struct put_loop_probe *)context;
  if (!probe || !key || !data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  probe->keys[index] = UINT64_C(1000) + (uint64_t)index;
  probe->values[index] = expected_value(probe->keys[index]);
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  *data = val(&probe->values[index], sizeof(probe->values[index]));
  probe->items += 1;
  return MDBX_SUCCESS;
}

static int put_loop_result_func(void *context, size_t index, const MDBX_val *key, MDBX_val *data, int result) {
  struct put_loop_probe *const probe = (struct put_loop_probe *)context;
  if (!probe || !key || !data || result != MDBX_SUCCESS ||
      index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  if (key->iov_base != &probe->keys[index] || key->iov_len != sizeof(probe->keys[index]))
    return MDBX_PROBLEM;
  if (data->iov_base != &probe->values[index] || data->iov_len != sizeof(probe->values[index]))
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int del_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct del_loop_probe *const probe = (struct del_loop_probe *)context;
  if (!probe || !key || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  probe->keys_seen += 1;
  return MDBX_SUCCESS;
}

static int del_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                bool has_data, int result) {
  struct del_loop_probe *const probe = (struct del_loop_probe *)context;
  if (!probe || !key || !data || has_data || result != MDBX_SUCCESS ||
      index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  if (key->iov_base != &probe->keys[index] || key->iov_len != sizeof(probe->keys[index]))
    return MDBX_PROBLEM;
  if (data->iov_base != NULL || data->iov_len != 0)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int replace_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *new_data,
                                  MDBX_val *old_data) {
  struct replace_loop_probe *const probe = (struct replace_loop_probe *)context;
  if (!probe || !key || !new_data || !old_data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  probe->new_values[index] = expected_value(probe->keys[index]) + UINT64_C(9000) + (uint64_t)index;
  probe->old_buffers[index] = 0;
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  *new_data = val(&probe->new_values[index], sizeof(probe->new_values[index]));
  *old_data = val(&probe->old_buffers[index], sizeof(probe->old_buffers[index]));
  probe->items += 1;
  return MDBX_SUCCESS;
}

static int replace_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *new_data,
                                    MDBX_val *old_data, int result) {
  struct replace_loop_probe *const probe = (struct replace_loop_probe *)context;
  if (!probe || !key || !new_data || !old_data || result != MDBX_SUCCESS ||
      index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return MDBX_PROBLEM;
  if (key->iov_base != &probe->keys[index] || key->iov_len != sizeof(probe->keys[index]))
    return MDBX_PROBLEM;
  if (new_data->iov_base != &probe->new_values[index] || new_data->iov_len != sizeof(probe->new_values[index]))
    return MDBX_PROBLEM;
  if (old_data->iov_len != sizeof(probe->old_buffers[index]))
    return MDBX_PROBLEM;
  uint64_t old_value = 0;
  memcpy(&old_value, old_data->iov_base, sizeof(old_value));
  if (old_value != probe->expected_old[index])
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int replace_delete_loop_item_func(void *context, size_t index, MDBX_val *key, MDBX_val *old_data) {
  struct replace_delete_loop_probe *const probe = (struct replace_delete_loop_probe *)context;
  if (!probe || !key || !old_data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return fail_msg("missing async replace delete loop item state", __FILE__, __LINE__);
  probe->old_buffers[index] = 0;
  *key = val(&probe->keys[index], sizeof(probe->keys[index]));
  *old_data = val(&probe->old_buffers[index], sizeof(probe->old_buffers[index]));
  probe->items += 1;
  return MDBX_SUCCESS;
}

static int replace_delete_loop_result_func(void *context, size_t index, const MDBX_val *key, MDBX_val *old_data,
                                           int result) {
  struct replace_delete_loop_probe *const probe = (struct replace_delete_loop_probe *)context;
  if (!probe || !key || !old_data || index >= sizeof(probe->keys) / sizeof(probe->keys[0]))
    return fail_msg("missing async replace delete loop result state", __FILE__, __LINE__);
  if (result != MDBX_SUCCESS)
    return fail_rc("mdbx_async_replace_delete_loop item", result, __FILE__, __LINE__);
  if (key->iov_base != &probe->keys[index] || key->iov_len != sizeof(probe->keys[index]))
    return fail_msg("unexpected async replace delete loop key", __FILE__, __LINE__);
  if (old_data->iov_len != sizeof(probe->old_buffers[index]))
    return fail_msg("unexpected async replace delete loop old value size", __FILE__, __LINE__);
  uint64_t old_value = 0;
  memcpy(&old_value, old_data->iov_base, sizeof(old_value));
  if (old_value != probe->expected_old[index])
    return fail_msg("unexpected async replace delete loop old value", __FILE__, __LINE__);
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int get_ex_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct get_ex_loop_probe *const probe = (struct get_ex_loop_probe *)context;
  if (!probe || !key || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  probe->key = (uint64_t)index;
  *key = val(&probe->key, sizeof(probe->key));
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int get_ex_loop_result_func(void *context, size_t index, const MDBX_val *key, const MDBX_val *data,
                                   size_t values_count, int result) {
  struct get_ex_loop_probe *const probe = (struct get_ex_loop_probe *)context;
  if (!probe || !key || !data || result != MDBX_SUCCESS || index >= ITEM_COUNT)
    return MDBX_PROBLEM;
  if (values_count != 1 || key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key != index || actual_value != expected_value((uint64_t)index))
    return MDBX_PROBLEM;
  probe->results += 1;
  probe->values += values_count;
  return MDBX_SUCCESS;
}

static int get_equal_or_great_loop_key_func(void *context, size_t index, MDBX_val *key) {
  struct get_equal_or_great_loop_probe *const probe = (struct get_equal_or_great_loop_probe *)context;
  if (!probe || !key || index + 1 >= ITEM_COUNT)
    return MDBX_PROBLEM;
  probe->key = (uint64_t)index;
  if (index & 1) {
    memcpy(probe->greater_key, &probe->key, sizeof(probe->key));
    probe->greater_key[sizeof(probe->key)] = 0;
    *key = val(probe->greater_key, sizeof(probe->greater_key));
  } else {
    *key = val(&probe->key, sizeof(probe->key));
  }
  probe->keys += 1;
  return MDBX_SUCCESS;
}

static int get_equal_or_great_loop_data_func(void *context, size_t index, const MDBX_val *key, MDBX_val *data) {
  struct get_equal_or_great_loop_probe *const probe = (struct get_equal_or_great_loop_probe *)context;
  if (!probe || !key || !data || index + 1 >= ITEM_COUNT)
    return MDBX_PROBLEM;
  *data = val(NULL, 0);
  probe->data += 1;
  return MDBX_SUCCESS;
}

static int get_equal_or_great_loop_result_func(void *context, size_t index, const MDBX_val *key,
                                               const MDBX_val *data, int result) {
  struct get_equal_or_great_loop_probe *const probe = (struct get_equal_or_great_loop_probe *)context;
  if (!probe || !key || !data || index + 1 >= ITEM_COUNT)
    return MDBX_PROBLEM;
  const uint64_t expected_key = (uint64_t)(index + (index & 1));
  if ((index & 1) ? result != MDBX_RESULT_TRUE : result != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  if (key->iov_len != sizeof(uint64_t) || data->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  uint64_t actual_value = 0;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  memcpy(&actual_value, data->iov_base, sizeof(actual_value));
  if (actual_key != expected_key || actual_value != expected_value(expected_key))
    return MDBX_PROBLEM;
  probe->results += 1;
  if (result == MDBX_RESULT_TRUE)
    probe->greater_results += 1;
  return MDBX_SUCCESS;
}

static int wait_result(const char *expr, MDBX_async_op **op, int *operation_result, const char *file, int line) {
  if (!op || !*op)
    return fail_msg("missing async operation handle", file, line);

  int result = MDBX_SUCCESS;
  int rc = mdbx_async_wait(*op, &result);
  if (rc != MDBX_SUCCESS)
    return fail_rc(expr, rc, file, line);

  rc = mdbx_async_op_release(*op);
  if (rc != MDBX_SUCCESS)
    return fail_rc("mdbx_async_op_release", rc, file, line);
  *op = NULL;

  if (operation_result)
    *operation_result = result;
  return MDBX_SUCCESS;
}

static int wait_success(const char *expr, MDBX_async_op **op, const char *file, int line) {
  int result = MDBX_SUCCESS;
  int rc = wait_result(expr, op, &result, file, line);
  if (rc != MDBX_SUCCESS)
    return rc;
  return result == MDBX_SUCCESS ? MDBX_SUCCESS : fail_rc(expr, result, file, line);
}

static int wait_many_result(const char *expr, MDBX_async_op **ops, size_t count, int *operation_results,
                            const char *file, int line) {
  if (count && (!ops || !operation_results))
    return fail_msg("missing async operation batch", file, line);

  int rc = mdbx_async_wait_release_all(ops, count, operation_results);
  if (rc != MDBX_SUCCESS)
    return fail_rc(expr, rc, file, line);
  return MDBX_SUCCESS;
}

static int wait_many_success(const char *expr, MDBX_async_op **ops, size_t count, int *operation_results,
                             const char *file, int line) {
  int rc = wait_many_result(expr, ops, count, operation_results, file, line);
  if (rc != MDBX_SUCCESS)
    return rc;
  for (size_t i = 0; i < count; ++i) {
    if (operation_results[i] != MDBX_SUCCESS)
      return fail_rc(expr, operation_results[i], file, line);
  }
  return MDBX_SUCCESS;
}

static int expect_payload(const MDBX_val *data, uint64_t expected, const char *file, int line) {
  if (data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected value size", file, line);
  uint64_t actual = 0;
  memcpy(&actual, data->iov_base, sizeof(actual));
  if (actual != expected)
    return fail_msg("unexpected value payload", file, line);
  return MDBX_SUCCESS;
}

static int expect_value(const MDBX_val *data, uint64_t key, const char *file, int line) {
  return expect_payload(data, expected_value(key), file, line);
}

static int expect_empty_value(const MDBX_val *data, const char *file, int line) {
  if (data->iov_base != NULL || data->iov_len != 0)
    return fail_msg("unexpected non-empty value", file, line);
  return MDBX_SUCCESS;
}

static bool async_read_fault_result(int rc) {
  return rc == MDBX_EIO || rc == MDBX_BAD_TXN;
}

static int expect_large_value(const MDBX_val *data, uint64_t key, const char *file, int line) {
  if (!data || !data->iov_base || data->iov_len != LARGE_VALUE_BYTES)
    return fail_msg("unexpected large value size", file, line);
  const uint8_t *const bytes = (const uint8_t *)data->iov_base;
  for (size_t i = 0; i < data->iov_len; ++i) {
    const uint8_t expected = (uint8_t)(key + i * 31u + i / 7u);
    if (bytes[i] != expected)
      return fail_msg("unexpected large value payload", file, line);
  }
  return MDBX_SUCCESS;
}

static int async_read_large_cursor_check_item(struct async_large_cursor_probe *probe,
                                              const MDBX_val *key,
                                              const MDBX_val *data) {
  if (!probe || !key || !data || probe->results >= probe->target ||
      key->iov_len != sizeof(uint64_t))
    return MDBX_PROBLEM;
  uint64_t actual_key = UINT64_MAX;
  memcpy(&actual_key, key->iov_base, sizeof(actual_key));
  if (actual_key != (uint64_t)probe->results)
    return MDBX_PROBLEM;
  const int rc = expect_large_value(data, actual_key, __FILE__, __LINE__);
  if (rc != MDBX_SUCCESS)
    return MDBX_PROBLEM;
  probe->results += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_cursor_loop_result_func(void *context, size_t index,
                                                    const MDBX_val *key,
                                                    const MDBX_val *data) {
  struct async_large_cursor_probe *const probe =
      (struct async_large_cursor_probe *)context;
  if (!probe || index != probe->results)
    return MDBX_PROBLEM;
  return async_read_large_cursor_check_item(probe, key, data);
}

static int async_read_large_cursor_batches_result_func(void *context,
                                                       const MDBX_val *pairs,
                                                       size_t count) {
  struct async_large_cursor_probe *const probe =
      (struct async_large_cursor_probe *)context;
  if (!probe || !pairs || count == 0 || (count & 1))
    return MDBX_PROBLEM;
  for (size_t i = 0; i < count; i += 2) {
    const int rc = async_read_large_cursor_check_item(probe, &pairs[i], &pairs[i + 1]);
    if (rc != MDBX_SUCCESS)
      return rc;
  }
  probe->batches += 1;
  return MDBX_SUCCESS;
}

static int async_read_large_cursor_scan_result_func(void *context, MDBX_val *key,
                                                    MDBX_val *data, void *arg) {
  (void)arg;
  struct async_large_cursor_probe *const probe =
      (struct async_large_cursor_probe *)context;
  const int rc = async_read_large_cursor_check_item(probe, key, data);
  if (rc != MDBX_SUCCESS)
    return rc;
  return probe->results == probe->target ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

static int verify_copied_value(const char *path, uint64_t key_value, const char *file, int line) {
  MDBX_env *copy_env = NULL;
  MDBX_txn *copy_txn = NULL;
  MDBX_dbi copy_dbi = 0;
  MDBX_val key = val(&key_value, sizeof(key_value));
  MDBX_val data = val(NULL, 0);

  int rc = mdbx_env_create(&copy_env);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  rc = mdbx_env_open(copy_env, path, MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE, 0);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  rc = mdbx_txn_begin(copy_env, NULL, MDBX_TXN_RDONLY, &copy_txn);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  rc = mdbx_dbi_open(copy_txn, NULL, MDBX_DB_DEFAULTS, &copy_dbi);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  rc = mdbx_get(copy_txn, copy_dbi, &key, &data);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  rc = expect_value(&data, key_value, file, line);

bailout:
  if (copy_txn)
    (void)mdbx_txn_abort(copy_txn);
  if (copy_env)
    (void)mdbx_env_close(copy_env);
  return rc == MDBX_SUCCESS ? MDBX_SUCCESS : fail_rc("verify_copied_value", rc, file, line);
}

static int exercise_async_preopen_recovery(const char *path, const char *file, int line) {
  MDBX_env *recovery_env = NULL;
  MDBX_async *preopen_async = NULL;
  MDBX_async_op *recovery_op = NULL;
  MDBX_envinfo snapinfo;
  int close_result = MDBX_SUCCESS;
  int rc = MDBX_SUCCESS;

  memset(&snapinfo, 0, sizeof(snapinfo));
  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &preopen_async));
  REQUIRE(mdbx_async_env(preopen_async) == NULL, "unbound async executor returned an environment");
  CHECK(mdbx_async_preopen_snapinfo(preopen_async, path, &snapinfo, sizeof(snapinfo), &recovery_op));
  rc = wait_success("mdbx_async_preopen_snapinfo", &recovery_op, file, line);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  REQUIRE(snapinfo.mi_dxb_pagesize > 0 && snapinfo.mi_geo.current > 0, "async preopen snapinfo returned empty data");

  CHECK(mdbx_async_env_create(preopen_async, &recovery_env, &recovery_op));
  rc = wait_success("mdbx_async_env_create recovery", &recovery_op, file, line);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  REQUIRE(mdbx_async_env(preopen_async) == recovery_env, "async recovery env create did not bind the executor");
  CHECK(mdbx_async_env_open_for_recovery(preopen_async, recovery_env, path, 0, true, &recovery_op));
  rc = wait_success("mdbx_async_env_open_for_recovery", &recovery_op, file, line);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  REQUIRE(mdbx_async_env(preopen_async) == recovery_env, "async recovery-open did not bind the executor");

  CHECK(mdbx_async_env_turn_for_recovery(preopen_async, 0, &recovery_op));
  rc = wait_success("mdbx_async_env_turn_for_recovery", &recovery_op, file, line);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  CHECK(mdbx_async_env_close_ex(preopen_async, true, &recovery_op));
  CHECK(wait_result("mdbx_async_env_close_ex recovery", &recovery_op, &close_result, file, line));
  if (close_result != MDBX_BUSY)
    recovery_env = NULL;
  REQUIRE(close_result == MDBX_SUCCESS, "unexpected async recovery environment close result");
  REQUIRE(mdbx_async_env(preopen_async) == NULL, "async recovery close did not unbind the executor");
  CHECK(mdbx_async_destroy(preopen_async, true));
  preopen_async = NULL;
  recovery_env = NULL;
  return MDBX_SUCCESS;

bailout:
  if (recovery_op) {
    int operation_result = MDBX_SUCCESS;
    (void)mdbx_async_wait(recovery_op, &operation_result);
    (void)mdbx_async_op_release(recovery_op);
  }
  if (preopen_async)
    (void)mdbx_async_destroy(preopen_async, true);
  if (recovery_env)
    (void)mdbx_env_close(recovery_env);
  return rc ? rc : fail_msg("async preopen recovery failed", file, line);
}

enum async_read_mode {
  async_read_sync_get,
  async_read_sync_get_ex,
  async_read_sync_lowerbound,
  async_read_sync_cursor_get,
  async_read_sync_cursor_get_miss,
  async_read_sync_cursor_get_set,
  async_read_sync_cursor_get_range,
  async_read_sync_cursor_get_upperbound,
  async_read_sync_cursor_get_to_key_equal,
  async_read_sync_cursor_get_to_key_equal_miss,
  async_read_sync_cursor_get_to_key_gte,
  async_read_sync_cursor_get_to_key_gt,
  async_read_sync_cursor_get_last,
  async_read_sync_cursor_get_prev,
  async_read_sync_cursor_get_prev_positioned,
  async_read_sync_cursor_get_nodup,
  async_read_sync_cursor_get_batch,
  async_read_sync_cursor_get_batch_nodup,
  async_read_sync_cursor_scan,
  async_read_sync_cursor_scan_current,
  async_read_sync_cursor_scan_current_prev,
  async_read_sync_cursor_scan_prev,
  async_read_sync_cursor_scan_nodup,
  async_read_sync_cursor_scan_from,
  async_read_sync_cursor_scan_from_prev,
  async_read_sync_cursor_scan_from_nodup,
  async_read_get,
  async_read_get_notfound,
  async_read_get_cache_hit,
  async_read_get_ex,
  async_read_lowerbound,
  async_read_get_many,
  async_read_get_ex_many,
  async_read_lowerbound_many,
  async_read_get_many_mixed,
  async_read_get_ex_many_mixed,
  async_read_lowerbound_many_mixed,
  async_read_cursor_get,
  async_read_cursor_get_miss,
  async_read_cursor_get_set,
  async_read_cursor_get_range,
  async_read_cursor_get_upperbound,
  async_read_cursor_get_to_key_equal,
  async_read_cursor_get_to_key_equal_miss,
  async_read_cursor_get_to_key_gte,
  async_read_cursor_get_to_key_gt,
  async_read_cursor_get_last,
  async_read_cursor_get_prev,
  async_read_cursor_get_prev_positioned,
  async_read_cursor_get_batch,
  async_read_cursor_get_batch_nodup,
  async_read_cache_get,
  async_read_cache_get_notfound,
  async_read_cache_get_singlethreaded,
  async_read_cache_get_many,
  async_read_cache_get_singlethreaded_many,
  async_read_cache_get_many_mixed,
  async_read_cache_get_singlethreaded_many_mixed,
  async_read_large_get,
  async_read_large_get_ex,
  async_read_large_get_many,
  async_read_large_get_ex_many,
  async_read_large_get_batch,
  async_read_large_get_ex_batch,
  async_read_large_get_loop,
  async_read_large_get_ex_loop,
  async_read_large_lowerbound,
  async_read_large_lowerbound_many,
  async_read_large_lowerbound_batch,
  async_read_large_lowerbound_loop,
  async_read_large_cursor_batch,
  async_read_large_cursor_loop,
  async_read_large_cursor_batches,
  async_read_large_cursor_scan,
  async_read_large_cache_get,
  async_read_large_cache_get_singlethreaded,
  async_read_large_cache_get_many,
  async_read_large_cache_get_singlethreaded_many,
  async_read_large_cache_get_batch,
  async_read_large_cache_get_singlethreaded_batch,
  async_read_large_cache_get_loop,
  async_read_large_cache_get_singlethreaded_loop,
  async_read_abort_order,
  async_read_abort_order_batch,
  async_read_abort_order_loop,
  async_read_get_batch,
  async_read_get_ex_batch,
  async_read_lowerbound_batch,
  async_read_get_batch_mixed,
  async_read_get_ex_batch_mixed,
  async_read_lowerbound_batch_mixed,
  async_read_cache_get_batch,
  async_read_cache_get_singlethreaded_batch,
  async_read_cache_get_batch_mixed,
  async_read_cache_get_singlethreaded_batch_mixed,
  async_read_get_loop,
  async_read_cache_get_loop,
  async_read_cache_get_singlethreaded_loop,
  async_read_get_ex_loop,
  async_read_lowerbound_loop,
  async_read_cursor_get_loop,
  async_read_cursor_get_loop_prev,
  async_read_cursor_get_loop_current,
  async_read_cursor_get_loop_current_prev,
  async_read_cursor_get_loop_nodup,
  async_read_cursor_get_loop_from,
  async_read_cursor_get_loop_from_prev,
  async_read_cursor_get_loop_from_setkey,
  async_read_cursor_get_loop_from_setkey_miss,
  async_read_cursor_get_loop_from_equal,
  async_read_cursor_get_loop_from_equal_miss,
  async_read_cursor_get_loop_from_gte,
  async_read_cursor_get_loop_from_gt,
  async_read_cursor_get_loop_from_upperbound,
  async_read_cursor_get_loop_from_nodup,
  async_read_cursor_get_batches,
  async_read_cursor_get_batches_current,
  async_read_cursor_get_batches_from,
  async_read_cursor_get_batches_from_setkey,
  async_read_cursor_get_batches_from_setkey_miss,
  async_read_cursor_get_batches_from_equal,
  async_read_cursor_get_batches_from_equal_miss,
  async_read_cursor_get_batches_from_gte,
  async_read_cursor_get_batches_from_gt,
  async_read_cursor_get_batches_from_upperbound,
  async_read_cursor_scan,
  async_read_cursor_scan_current,
  async_read_cursor_scan_current_prev,
  async_read_cursor_scan_prev,
  async_read_cursor_scan_nodup,
  async_read_cursor_scan_from,
  async_read_cursor_scan_from_prev,
  async_read_cursor_scan_from_setkey,
  async_read_cursor_scan_from_setkey_miss,
  async_read_cursor_scan_from_equal,
  async_read_cursor_scan_from_equal_miss,
  async_read_cursor_scan_from_gte,
  async_read_cursor_scan_from_gt,
  async_read_cursor_scan_from_upperbound,
  async_read_cursor_scan_from_nodup
};

static int exercise_async_read_path(const char *path, bool inject_fault, enum async_read_mode mode) {
  MDBX_async *async = NULL;
  MDBX_env *env = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  struct async_block_probe abort_probe;
  bool abort_probe_prepared = false;
  MDBX_dbi dbi = 0;
  MDBX_async_op *op = NULL;
  MDBX_async_op *abort_ops[7];
  MDBX_async_op *many_ops[ASYNC_READ_BATCH_COUNT];
  MDBX_async_read_stats read_stats;
  int rc = MDBX_SUCCESS;
  int operation_result = MDBX_SUCCESS;
  int abort_results[7];
  int many_operation_results[ASYNC_READ_BATCH_COUNT];
  size_t values_count = SIZE_MAX;
  const uint64_t key = 42;
  const uint64_t missing_key = 43;
  const uint64_t payload = expected_value(key);
  MDBX_val key_value = val((void *)&key, sizeof(key));
  MDBX_val missing_key_value = val((void *)&missing_key, sizeof(missing_key));
  uint8_t missing_batch_key_bytes[sizeof(uint64_t)];
  MDBX_val missing_batch_key_value = val(missing_batch_key_bytes, sizeof(missing_batch_key_bytes));
  MDBX_val put_value = val((void *)&payload, sizeof(payload));
  MDBX_val data = val(NULL, 0);
  MDBX_cache_entry_t cache_entry;
  MDBX_cache_result_t cache_result = {MDBX_PROBLEM, MDBX_CACHE_ERROR};
  const char *read_name = "mdbx_async_get cold single read";
  uint8_t large_value[LARGE_VALUE_BYTES];
  MDBX_val large_put_value = val(large_value, sizeof(large_value));
  uint8_t large_batch_values[LARGE_ITEM_COUNT][LARGE_VALUE_BYTES];
  uint64_t batch_keys[ASYNC_READ_BATCH_COUNT];
  MDBX_val batch_key_values[ASYNC_READ_BATCH_COUNT];
  MDBX_val batch_put_values[ASYNC_READ_BATCH_COUNT];
  MDBX_val batch_data[ASYNC_READ_BATCH_COUNT];
  size_t batch_values_counts[ASYNC_READ_BATCH_COUNT];
  int batch_results[ASYNC_READ_BATCH_COUNT];
  MDBX_cache_entry_t batch_cache_entries[ASYNC_READ_BATCH_COUNT];
  MDBX_cache_result_t batch_cache_results[ASYNC_READ_BATCH_COUNT];
  uint8_t batch_values[ASYNC_READ_BATCH_COUNT][ASYNC_READ_BATCH_VALUE_BYTES];
  MDBX_val cursor_batch_pairs[ASYNC_READ_BATCH_COUNT * 2];
  MDBX_val cursor_prime_pairs[4];
  size_t cursor_batch_count = 0;
  size_t cursor_prime_count = 0;
  unsigned cursor_batch_start_index = 0;
  size_t cursor_stream_completed = 0;
  int cursor_batch_result = MDBX_SUCCESS;
  int cursor_prime_result = MDBX_SUCCESS;
  struct async_large_cursor_probe large_cursor_probe = {0, 0, LARGE_ITEM_COUNT};
  struct async_read_loop_probe loop_read_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  MDBX_val abort_get_batch_data[ASYNC_READ_BATCH_COUNT];
  int abort_get_batch_results[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_get_ex_batch_keys[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_get_ex_batch_data[ASYNC_READ_BATCH_COUNT];
  size_t abort_get_ex_batch_counts[ASYNC_READ_BATCH_COUNT];
  int abort_get_ex_batch_results[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_lower_batch_keys[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_lower_batch_data[ASYNC_READ_BATCH_COUNT];
  int abort_lower_batch_results[ASYNC_READ_BATCH_COUNT];
  MDBX_cache_entry_t abort_cache_entries[ASYNC_READ_BATCH_COUNT];
  MDBX_cache_result_t abort_cache_results[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_cache_data[ASYNC_READ_BATCH_COUNT];
  MDBX_val abort_cursor_pairs[ASYNC_READ_BATCH_COUNT * 2];
  size_t abort_cursor_count = 0;
  struct async_read_loop_probe abort_get_loop_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  struct async_read_loop_probe abort_get_ex_loop_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  struct async_read_loop_probe abort_lower_loop_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  struct async_read_loop_probe abort_cache_loop_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  struct async_read_loop_probe abort_cursor_loop_probe = {0, 0, 0, 0, batch_values, MDBX_SUCCESS, false};
  MDBX_cache_entry_t abort_loop_cache_entries[ASYNC_READ_BATCH_COUNT];
  size_t abort_get_loop_completed = 0;
  size_t abort_get_ex_loop_completed = 0;
  size_t abort_lower_loop_completed = 0;
  size_t abort_cache_loop_completed = 0;
  size_t abort_cursor_loop_completed = 0;
  size_t loop_completed = 0;
  MDBX_val cursor_from_key = val(NULL, 0);
  MDBX_val cursor_from_data = val(NULL, 0);
  MDBX_val zero_key_value = val(NULL, 0);
  bool cursor_is_async = false;
  const bool sync_read = mode == async_read_sync_get || mode == async_read_sync_get_ex ||
                         mode == async_read_sync_lowerbound ||
                         mode == async_read_sync_cursor_get ||
                         mode == async_read_sync_cursor_get_miss ||
                         mode == async_read_sync_cursor_get_set ||
                         mode == async_read_sync_cursor_get_range ||
                         mode == async_read_sync_cursor_get_upperbound ||
                         mode == async_read_sync_cursor_get_to_key_equal ||
                         mode == async_read_sync_cursor_get_to_key_equal_miss ||
                         mode == async_read_sync_cursor_get_to_key_gte ||
                         mode == async_read_sync_cursor_get_to_key_gt ||
                         mode == async_read_sync_cursor_get_last ||
                         mode == async_read_sync_cursor_get_prev ||
                         mode == async_read_sync_cursor_get_prev_positioned ||
                         mode == async_read_sync_cursor_get_nodup ||
                         mode == async_read_sync_cursor_get_batch ||
                         mode == async_read_sync_cursor_get_batch_nodup ||
                         mode == async_read_sync_cursor_scan ||
                         mode == async_read_sync_cursor_scan_current ||
                         mode == async_read_sync_cursor_scan_current_prev ||
                         mode == async_read_sync_cursor_scan_prev ||
                         mode == async_read_sync_cursor_scan_nodup ||
                         mode == async_read_sync_cursor_scan_from ||
                         mode == async_read_sync_cursor_scan_from_prev ||
                         mode == async_read_sync_cursor_scan_from_nodup;
  const bool loop_read = mode == async_read_get_loop || mode == async_read_cache_get_loop ||
                         mode == async_read_cache_get_singlethreaded_loop ||
                         mode == async_read_large_cache_get_loop ||
                         mode == async_read_large_cache_get_singlethreaded_loop ||
                         mode == async_read_large_get_loop ||
                         mode == async_read_large_get_ex_loop ||
                         mode == async_read_get_ex_loop || mode == async_read_lowerbound_loop ||
                         mode == async_read_large_lowerbound_loop;
  const bool large_indexed_read = mode == async_read_large_get_many ||
                                  mode == async_read_large_get_ex_many ||
                                  mode == async_read_large_get_batch ||
                                  mode == async_read_large_get_ex_batch ||
                                  mode == async_read_large_get_loop ||
                                  mode == async_read_large_get_ex_loop ||
                                  mode == async_read_large_lowerbound_many ||
                                  mode == async_read_large_lowerbound_batch ||
                                  mode == async_read_large_lowerbound_loop ||
                                  mode == async_read_large_cache_get_many ||
                                  mode == async_read_large_cache_get_singlethreaded_many ||
                                  mode == async_read_large_cache_get_batch ||
                                  mode == async_read_large_cache_get_singlethreaded_batch ||
                                  mode == async_read_large_cache_get_loop ||
                                  mode == async_read_large_cache_get_singlethreaded_loop;
  const bool many_read = mode == async_read_get_many ||
                         mode == async_read_get_ex_many ||
                         mode == async_read_lowerbound_many ||
                         mode == async_read_large_lowerbound_many ||
                         mode == async_read_large_get_many ||
                         mode == async_read_large_get_ex_many ||
                         mode == async_read_get_many_mixed ||
                         mode == async_read_get_ex_many_mixed ||
                         mode == async_read_lowerbound_many_mixed ||
                         mode == async_read_cache_get_many ||
                         mode == async_read_cache_get_singlethreaded_many ||
                         mode == async_read_large_cache_get_many ||
                         mode == async_read_large_cache_get_singlethreaded_many ||
                         mode == async_read_cache_get_many_mixed ||
                         mode == async_read_cache_get_singlethreaded_many_mixed;
  const bool mixed_batch_read = mode == async_read_get_many_mixed ||
                                mode == async_read_get_ex_many_mixed ||
                                mode == async_read_lowerbound_many_mixed ||
                                mode == async_read_cache_get_many_mixed ||
                                mode == async_read_cache_get_singlethreaded_many_mixed ||
                                mode == async_read_get_batch_mixed ||
                                mode == async_read_get_ex_batch_mixed ||
                                mode == async_read_lowerbound_batch_mixed ||
                                mode == async_read_cache_get_batch_mixed ||
                                mode == async_read_cache_get_singlethreaded_batch_mixed;
  const bool abort_order_batch_read = mode == async_read_abort_order_batch;
  const bool abort_order_loop_read = mode == async_read_abort_order_loop;
  const bool abort_order_read = mode == async_read_abort_order ||
                                abort_order_batch_read || abort_order_loop_read;
  const bool sync_cursor_stream_read = mode == async_read_sync_cursor_get_batch ||
                                       mode == async_read_sync_cursor_get_batch_nodup ||
                                       mode == async_read_sync_cursor_scan ||
                                       mode == async_read_sync_cursor_scan_current ||
                                       mode == async_read_sync_cursor_scan_current_prev ||
                                       mode == async_read_sync_cursor_scan_prev ||
                                       mode == async_read_sync_cursor_scan_nodup ||
                                       mode == async_read_sync_cursor_scan_from ||
                                       mode == async_read_sync_cursor_scan_from_prev ||
                                       mode == async_read_sync_cursor_scan_from_nodup;
  const bool cursor_stream_read = mode == async_read_cursor_get_batch ||
                                  mode == async_read_cursor_get_batch_nodup ||
                                  mode == async_read_cursor_get_loop ||
                                  mode == async_read_cursor_get_loop_prev ||
                                  mode == async_read_cursor_get_loop_current ||
                                  mode == async_read_cursor_get_loop_current_prev ||
                                  mode == async_read_cursor_get_loop_nodup ||
                                  mode == async_read_cursor_get_loop_from ||
                                  mode == async_read_cursor_get_loop_from_prev ||
                                  mode == async_read_cursor_get_loop_from_setkey ||
                                  mode == async_read_cursor_get_loop_from_setkey_miss ||
                                  mode == async_read_cursor_get_loop_from_equal ||
                                  mode == async_read_cursor_get_loop_from_equal_miss ||
                                  mode == async_read_cursor_get_loop_from_gte ||
                                  mode == async_read_cursor_get_loop_from_gt ||
                                  mode == async_read_cursor_get_loop_from_upperbound ||
                                  mode == async_read_cursor_get_loop_from_nodup ||
                                  mode == async_read_cursor_get_batches ||
                                  mode == async_read_cursor_get_batches_current ||
                                  mode == async_read_cursor_get_batches_from ||
                                  mode == async_read_cursor_get_batches_from_setkey ||
                                  mode == async_read_cursor_get_batches_from_setkey_miss ||
                                  mode == async_read_cursor_get_batches_from_equal ||
                                  mode == async_read_cursor_get_batches_from_equal_miss ||
                                  mode == async_read_cursor_get_batches_from_gte ||
                                  mode == async_read_cursor_get_batches_from_gt ||
                                  mode == async_read_cursor_get_batches_from_upperbound ||
                                  mode == async_read_cursor_scan ||
                                  mode == async_read_cursor_scan_current ||
                                  mode == async_read_cursor_scan_current_prev ||
                                  mode == async_read_cursor_scan_prev ||
                                  mode == async_read_cursor_scan_nodup ||
                                  mode == async_read_cursor_scan_from ||
                                  mode == async_read_cursor_scan_from_prev ||
                                  mode == async_read_cursor_scan_from_setkey ||
                                  mode == async_read_cursor_scan_from_setkey_miss ||
                                  mode == async_read_cursor_scan_from_equal ||
                                  mode == async_read_cursor_scan_from_equal_miss ||
                                  mode == async_read_cursor_scan_from_gte ||
                                  mode == async_read_cursor_scan_from_gt ||
                                  mode == async_read_cursor_scan_from_upperbound ||
                                  mode == async_read_cursor_scan_from_nodup ||
                                  sync_cursor_stream_read;
  const bool cursor_scan_read = mode == async_read_cursor_scan ||
                                mode == async_read_cursor_scan_current ||
                                mode == async_read_cursor_scan_current_prev ||
                                mode == async_read_cursor_scan_prev ||
                                mode == async_read_cursor_scan_nodup ||
                                mode == async_read_cursor_scan_from ||
                                mode == async_read_cursor_scan_from_prev ||
                                mode == async_read_cursor_scan_from_setkey ||
                                mode == async_read_cursor_scan_from_setkey_miss ||
                                mode == async_read_cursor_scan_from_equal ||
                                mode == async_read_cursor_scan_from_equal_miss ||
                                mode == async_read_cursor_scan_from_gte ||
                                mode == async_read_cursor_scan_from_gt ||
                                mode == async_read_cursor_scan_from_upperbound ||
                                mode == async_read_cursor_scan_from_nodup ||
                                mode == async_read_sync_cursor_scan ||
                                mode == async_read_sync_cursor_scan_current ||
                                mode == async_read_sync_cursor_scan_current_prev ||
                                mode == async_read_sync_cursor_scan_prev ||
                                mode == async_read_sync_cursor_scan_nodup ||
                                mode == async_read_sync_cursor_scan_from ||
                                mode == async_read_sync_cursor_scan_from_prev ||
                                mode == async_read_sync_cursor_scan_from_nodup;
  const bool indexed_batch_read = mode == async_read_get_batch ||
                                  mode == async_read_get_ex_batch ||
                                  mode == async_read_lowerbound_batch ||
                                  mode == async_read_cache_get_batch ||
                                  mode == async_read_cache_get_singlethreaded_batch ||
                                  mode == async_read_large_get_batch ||
                                  mode == async_read_large_get_ex_batch ||
                                  mode == async_read_large_lowerbound_batch ||
                                  mode == async_read_large_cache_get_batch ||
                                  mode == async_read_large_cache_get_singlethreaded_batch ||
                                  mixed_batch_read ||
                                  many_read ||
                                  loop_read || abort_order_batch_read ||
                                  abort_order_loop_read;
  const bool upperbound_read = mode == async_read_sync_cursor_get_upperbound ||
                               mode == async_read_cursor_get_upperbound;
  const bool to_key_equal_read = mode == async_read_sync_cursor_get_to_key_equal ||
                                 mode == async_read_cursor_get_to_key_equal;
  const bool to_key_equal_miss_read = mode == async_read_sync_cursor_get_to_key_equal_miss ||
                                      mode == async_read_cursor_get_to_key_equal_miss;
  const bool to_key_gte_read = mode == async_read_sync_cursor_get_to_key_gte ||
                               mode == async_read_cursor_get_to_key_gte;
  const bool to_key_gt_read = mode == async_read_sync_cursor_get_to_key_gt ||
                              mode == async_read_cursor_get_to_key_gt;
  const bool cursor_last_read = mode == async_read_sync_cursor_get_last ||
                                mode == async_read_sync_cursor_get_prev ||
                                mode == async_read_sync_cursor_get_prev_positioned ||
                                mode == async_read_cursor_get_last ||
                                mode == async_read_cursor_get_prev ||
                                mode == async_read_cursor_get_prev_positioned;
  const bool batch_dataset_read = mode == async_read_sync_cursor_get_nodup ||
                                  cursor_last_read ||
                                  upperbound_read ||
                                  to_key_equal_read || to_key_equal_miss_read ||
                                  to_key_gte_read || to_key_gt_read;
  const unsigned forward_seek_expected_index =
      to_key_equal_miss_read ? 0u : (upperbound_read || to_key_gt_read) ? 2u : 1u;
  const bool batch_read = indexed_batch_read || cursor_stream_read;
  const size_t indexed_read_count =
      large_indexed_read ? LARGE_ITEM_COUNT : ASYNC_READ_BATCH_COUNT;
  const bool cursor_from_equal_miss_read =
      mode == async_read_cursor_get_loop_from_equal_miss ||
      mode == async_read_cursor_get_batches_from_equal_miss ||
      mode == async_read_cursor_scan_from_equal_miss;
  const bool cursor_from_setkey_miss_read =
      mode == async_read_cursor_get_loop_from_setkey_miss ||
      mode == async_read_cursor_get_batches_from_setkey_miss ||
      mode == async_read_cursor_scan_from_setkey_miss;
  const bool cursor_from_exclusive_read =
      mode == async_read_cursor_get_loop_from_gt ||
      mode == async_read_cursor_get_loop_from_upperbound ||
      mode == async_read_cursor_get_batches_from_gt ||
      mode == async_read_cursor_get_batches_from_upperbound ||
      mode == async_read_cursor_scan_from_gt ||
      mode == async_read_cursor_scan_from_upperbound;
  const size_t cursor_stream_target =
      cursor_from_exclusive_read ? ASYNC_READ_BATCH_COUNT - 1 : ASYNC_READ_BATCH_COUNT;
  const bool large_cursor_stream_read =
      mode == async_read_large_cursor_loop ||
      mode == async_read_large_cursor_batches ||
      mode == async_read_large_cursor_scan;
  const bool notfound_read = mode == async_read_get_notfound ||
                             mode == async_read_cache_get_notfound ||
                             mode == async_read_sync_cursor_get_miss ||
                             mode == async_read_cursor_get_miss;
  const bool cache_single_read = mode == async_read_cache_get ||
                                 mode == async_read_cache_get_singlethreaded;
  const bool cache_batch_read = mode == async_read_cache_get_batch ||
                                mode == async_read_cache_get_singlethreaded_batch ||
                                mode == async_read_cache_get_many ||
                                mode == async_read_cache_get_singlethreaded_many ||
                                mode == async_read_cache_get_batch_mixed ||
                                mode == async_read_cache_get_singlethreaded_batch_mixed ||
                                mode == async_read_large_cache_get_many ||
                                mode == async_read_large_cache_get_singlethreaded_many ||
                                mode == async_read_large_cache_get_batch ||
                                mode == async_read_large_cache_get_singlethreaded_batch ||
                                mode == async_read_cache_get_many_mixed ||
                                mode == async_read_cache_get_singlethreaded_many_mixed;
  const bool cache_loop_read = mode == async_read_cache_get_loop ||
                               mode == async_read_cache_get_singlethreaded_loop ||
                               mode == async_read_large_cache_get_loop ||
                               mode == async_read_large_cache_get_singlethreaded_loop;
  const bool large_read = mode == async_read_large_get ||
                          mode == async_read_large_get_ex ||
                          mode == async_read_large_lowerbound ||
                          mode == async_read_large_cursor_batch ||
                          mode == async_read_large_cache_get ||
                          mode == async_read_large_cache_get_singlethreaded;
  const bool cache_hit_read = mode == async_read_get_cache_hit;
  MDBX_val *read_key_value = notfound_read ? &missing_key_value : &key_value;

  memset(&abort_probe, 0, sizeof(abort_probe));
  memset(abort_ops, 0, sizeof(abort_ops));
  memset(many_ops, 0, sizeof(many_ops));
  memset(abort_results, 0, sizeof(abort_results));
  memset(many_operation_results, 0, sizeof(many_operation_results));
  memset(missing_batch_key_bytes, 0xff, sizeof(missing_batch_key_bytes));
  loop_read_probe.expected_result = inject_fault ? MDBX_EIO : MDBX_SUCCESS;
  if (cursor_from_exclusive_read)
    loop_read_probe.hits = 1;
  for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
    abort_get_batch_data[i] = val(NULL, 0);
    abort_get_batch_results[i] = MDBX_PROBLEM;
    abort_get_ex_batch_keys[i] = val(NULL, 0);
    abort_get_ex_batch_data[i] = val(NULL, 0);
    abort_get_ex_batch_counts[i] = SIZE_MAX;
    abort_get_ex_batch_results[i] = MDBX_PROBLEM;
    abort_lower_batch_keys[i] = val(NULL, 0);
    abort_lower_batch_data[i] = val(NULL, 0);
    abort_lower_batch_results[i] = MDBX_PROBLEM;
    mdbx_cache_init(&abort_cache_entries[i]);
    abort_cache_results[i].errcode = MDBX_PROBLEM;
    abort_cache_results[i].status = MDBX_CACHE_ERROR;
    abort_cache_data[i] = val(NULL, 0);
    mdbx_cache_init(&abort_loop_cache_entries[i]);
  }

  if (!env_enabled("MDBX_FORCE_NO_DATA_MMAP")) {
    rc = fail_msg("async read fault smoke requires MDBX_FORCE_NO_DATA_MMAP=1", __FILE__, __LINE__);
    goto bailout;
  }

  if (batch_read || batch_dataset_read) {
    const size_t setup_count = large_indexed_read ? indexed_read_count
                                                  : ASYNC_READ_BATCH_COUNT;
    for (unsigned i = 0; i < setup_count; ++i) {
      batch_keys[i] = i;
      if (large_indexed_read) {
        fill_large_value(large_batch_values[i], sizeof(large_batch_values[i]), batch_keys[i]);
        batch_put_values[i] = val(large_batch_values[i], sizeof(large_batch_values[i]));
      } else {
        fill_large_value(batch_values[i], sizeof(batch_values[i]), batch_keys[i]);
        batch_put_values[i] = val(batch_values[i], sizeof(batch_values[i]));
      }
      batch_key_values[i] = val(&batch_keys[i], sizeof(batch_keys[i]));
      batch_data[i] = val(NULL, 0);
      batch_values_counts[i] = SIZE_MAX;
      batch_results[i] = MDBX_PROBLEM;
      mdbx_cache_init(&batch_cache_entries[i]);
      batch_cache_results[i].errcode = MDBX_PROBLEM;
      batch_cache_results[i].status = MDBX_CACHE_ERROR;
    }
  } else if (large_cursor_stream_read) {
    for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
      batch_keys[i] = i;
      fill_large_value(large_batch_values[i], sizeof(large_batch_values[i]), batch_keys[i]);
      batch_key_values[i] = val(&batch_keys[i], sizeof(batch_keys[i]));
      batch_put_values[i] = val(large_batch_values[i], sizeof(large_batch_values[i]));
    }
  } else if (large_read) {
    fill_large_value(large_value, sizeof(large_value), key);
  }
  if (to_key_equal_miss_read)
    read_key_value = &zero_key_value;
  else if (upperbound_read || to_key_equal_read || to_key_gte_read || to_key_gt_read)
    read_key_value = &batch_key_values[1];

  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
    rc = fail_rc("mdbx_env_delete", rc, __FILE__, __LINE__);
    goto bailout;
  }

  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_env_create(async, &env, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_begin_ex(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, NULL, MDBX_DB_DEFAULTS, &dbi, &op));
  CHECK_OP(op);
  if (large_cursor_stream_read) {
    for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
      CHECK(mdbx_async_put(async, txn, dbi, &batch_key_values[i],
                           &batch_put_values[i], 0, &op));
      CHECK_OP(op);
    }
  } else if (batch_read || batch_dataset_read) {
    const size_t populate_count = large_indexed_read ? indexed_read_count
                                                     : ASYNC_READ_BATCH_COUNT;
    for (unsigned i = 0; i < populate_count; ++i) {
      CHECK(mdbx_async_put(async, txn, dbi, &batch_key_values[i], &batch_put_values[i], 0, &op));
      CHECK_OP(op);
    }
  } else {
    CHECK(mdbx_async_put(async, txn, dbi, &key_value, large_read ? &large_put_value : &put_value, 0, &op));
    CHECK_OP(op);
  }
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_env_close_ex(async, false, &op));
  CHECK(wait_result("mdbx_async_env_close_ex populate", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "unexpected async env close result after populate");
  env = NULL;
  CHECK(mdbx_async_destroy(async, true));
  async = NULL;

  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_env_create(async, &env, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  if (sync_read) {
    CHECK(mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn));
  } else {
    CHECK(mdbx_async_txn_begin_ex(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
    CHECK_OP(op);
  }
  if (mode == async_read_sync_get) {
    read_name = "mdbx_get cold sync read";
  } else if (mode == async_read_sync_get_ex) {
    read_name = "mdbx_get_ex cold sync read";
  } else if (mode == async_read_sync_lowerbound) {
    read_name = "mdbx_get_equal_or_great cold sync read";
  } else if (mode == async_read_sync_cursor_get ||
             mode == async_read_sync_cursor_get_miss) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = mode == async_read_sync_cursor_get_miss
                    ? "mdbx_cursor_get cold sync missing read"
                    : "mdbx_cursor_get cold sync read";
  } else if (mode == async_read_sync_cursor_get_set) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get SET cold sync read";
  } else if (mode == async_read_sync_cursor_get_range) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get SET_RANGE cold sync read";
  } else if (mode == async_read_sync_cursor_get_upperbound) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get SET_UPPERBOUND cold sync read";
  } else if (mode == async_read_sync_cursor_get_to_key_equal) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get TO_KEY_EQUAL cold sync read";
  } else if (mode == async_read_sync_cursor_get_to_key_equal_miss) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get TO_KEY_EQUAL cold sync miss read";
  } else if (mode == async_read_sync_cursor_get_to_key_gte) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get TO_KEY_GREATER_OR_EQUAL cold sync read";
  } else if (mode == async_read_sync_cursor_get_to_key_gt) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get TO_KEY_GREATER_THAN cold sync read";
  } else if (mode == async_read_sync_cursor_get_last) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get LAST cold sync read";
  } else if (mode == async_read_sync_cursor_get_prev) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get PREV cold sync read";
  } else if (mode == async_read_sync_cursor_get_prev_positioned) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get positioned PREV cold sync read";
  } else if (mode == async_read_sync_cursor_get_nodup) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get NEXT_NODUP cold sync read";
  } else if (mode == async_read_sync_cursor_get_batch) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get_batch cold sync read";
  } else if (mode == async_read_sync_cursor_get_batch_nodup) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_get_batch NEXT_NODUP cold sync read";
  } else if (mode == async_read_sync_cursor_scan) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan cold sync read";
  } else if (mode == async_read_sync_cursor_scan_current) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    CHECK(mdbx_cursor_get(cursor, &cursor_from_key, &cursor_from_data,
                          MDBX_SET_KEY));
    read_name = "mdbx_cursor_scan GET_CURRENT cold sync read";
  } else if (mode == async_read_sync_cursor_scan_current_prev) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
    cursor_from_data = val(NULL, 0);
    CHECK(mdbx_cursor_get(cursor, &cursor_from_key, &cursor_from_data,
                          MDBX_SET_KEY));
    read_name = "mdbx_cursor_scan GET_CURRENT PREV cold sync read";
  } else if (mode == async_read_sync_cursor_scan_prev) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan PREV cold sync read";
  } else if (mode == async_read_sync_cursor_scan_nodup) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan NEXT_NODUP cold sync read";
  } else if (mode == async_read_sync_cursor_scan_from) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan_from cold sync read";
  } else if (mode == async_read_sync_cursor_scan_from_prev) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan_from PREV cold sync read";
  } else if (mode == async_read_sync_cursor_scan_from_nodup) {
    CHECK(mdbx_cursor_open(txn, dbi, &cursor));
    read_name = "mdbx_cursor_scan_from NEXT_NODUP cold sync read";
  } else if (cache_hit_read) {
    read_name = "mdbx_async_get warm page-cache hit";
  } else if (mode == async_read_get_ex) {
    read_name = "mdbx_async_get_ex cold single read";
  } else if (mode == async_read_lowerbound) {
    read_name = "mdbx_async_get_equal_or_great cold exact read";
  } else if (mode == async_read_get_many ||
             mode == async_read_get_many_mixed) {
    read_name = "mdbx_async_get_many cold read";
  } else if (mode == async_read_get_ex_many ||
             mode == async_read_get_ex_many_mixed) {
    read_name = "mdbx_async_get_ex_many cold read";
  } else if (mode == async_read_lowerbound_many ||
             mode == async_read_lowerbound_many_mixed) {
    read_name = "mdbx_async_get_equal_or_great_many cold read";
  } else if (mode == async_read_cursor_get ||
             mode == async_read_cursor_get_miss ||
             mode == async_read_cursor_get_set ||
             mode == async_read_cursor_get_range ||
             mode == async_read_cursor_get_upperbound ||
             mode == async_read_cursor_get_to_key_equal ||
             mode == async_read_cursor_get_to_key_equal_miss ||
             mode == async_read_cursor_get_to_key_gte ||
             mode == async_read_cursor_get_to_key_gt ||
             mode == async_read_cursor_get_last ||
             mode == async_read_cursor_get_prev ||
             mode == async_read_cursor_get_prev_positioned) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    if (mode == async_read_cursor_get_miss)
      read_name = "mdbx_async_cursor_get cold missing read";
    else if (mode == async_read_cursor_get_range)
      read_name = "mdbx_async_cursor_get SET_RANGE cold single read";
    else if (mode == async_read_cursor_get_upperbound)
      read_name = "mdbx_async_cursor_get SET_UPPERBOUND cold single read";
    else if (mode == async_read_cursor_get_to_key_equal)
      read_name = "mdbx_async_cursor_get TO_KEY_EQUAL cold single read";
    else if (mode == async_read_cursor_get_to_key_equal_miss)
      read_name = "mdbx_async_cursor_get TO_KEY_EQUAL cold missing read";
    else if (mode == async_read_cursor_get_to_key_gte)
      read_name = "mdbx_async_cursor_get TO_KEY_GREATER_OR_EQUAL cold single read";
    else if (mode == async_read_cursor_get_to_key_gt)
      read_name = "mdbx_async_cursor_get TO_KEY_GREATER_THAN cold single read";
    else if (mode == async_read_cursor_get_last)
      read_name = "mdbx_async_cursor_get LAST cold single read";
    else if (mode == async_read_cursor_get_prev)
      read_name = "mdbx_async_cursor_get PREV cold single read";
    else if (mode == async_read_cursor_get_prev_positioned)
      read_name = "mdbx_async_cursor_get positioned PREV cold single read";
    else if (mode == async_read_cursor_get_set)
      read_name = "mdbx_async_cursor_get SET cold single read";
    else
      read_name = "mdbx_async_cursor_get cold single read";
  } else if (cursor_stream_read) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    if (mode == async_read_cursor_get_batch)
      read_name = "mdbx_async_cursor_get_batch cold read";
    else if (mode == async_read_cursor_get_batch_nodup)
      read_name = "mdbx_async_cursor_get_batch NEXT_NODUP cold read";
    else if (mode == async_read_cursor_get_loop)
      read_name = "mdbx_async_cursor_get_loop cold read";
    else if (mode == async_read_cursor_get_loop_prev)
      read_name = "mdbx_async_cursor_get_loop PREV cold read";
    else if (mode == async_read_cursor_get_loop_current) {
      MDBX_async_op *prime_op = NULL;
      cursor_from_key = batch_key_values[0];
      cursor_from_data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &cursor_from_key,
                                  &cursor_from_data, MDBX_SET_KEY, &prime_op));
      CHECK(wait_result("mdbx_async_cursor_get_loop GET_CURRENT prime",
                        &prime_op, &operation_result, __FILE__, __LINE__));
      REQUIRE(operation_result == MDBX_SUCCESS,
              "unexpected async cursor get loop GET_CURRENT prime result");
      operation_result = MDBX_SUCCESS;
      read_name = "mdbx_async_cursor_get_loop GET_CURRENT cold read";
    } else if (mode == async_read_cursor_get_loop_current_prev) {
      MDBX_async_op *prime_op = NULL;
      cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
      cursor_from_data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &cursor_from_key,
                                  &cursor_from_data, MDBX_SET_KEY, &prime_op));
      CHECK(wait_result("mdbx_async_cursor_get_loop GET_CURRENT PREV prime",
                        &prime_op, &operation_result, __FILE__, __LINE__));
      REQUIRE(operation_result == MDBX_SUCCESS,
              "unexpected async cursor get loop GET_CURRENT PREV prime result");
      operation_result = MDBX_SUCCESS;
      read_name = "mdbx_async_cursor_get_loop GET_CURRENT PREV cold read";
    } else if (mode == async_read_cursor_get_loop_nodup)
      read_name = "mdbx_async_cursor_get_loop NEXT_NODUP cold read";
    else if (mode == async_read_cursor_get_loop_from)
      read_name = "mdbx_async_cursor_get_loop_from cold read";
    else if (mode == async_read_cursor_get_loop_from_prev)
      read_name = "mdbx_async_cursor_get_loop_from PREV cold read";
    else if (mode == async_read_cursor_get_loop_from_setkey)
      read_name = "mdbx_async_cursor_get_loop_from SET_KEY cold read";
    else if (mode == async_read_cursor_get_loop_from_setkey_miss)
      read_name = "mdbx_async_cursor_get_loop_from SET_KEY cold missing read";
    else if (mode == async_read_cursor_get_loop_from_equal)
      read_name = "mdbx_async_cursor_get_loop_from TO_KEY_EQUAL cold read";
    else if (mode == async_read_cursor_get_loop_from_equal_miss)
      read_name = "mdbx_async_cursor_get_loop_from TO_KEY_EQUAL cold missing read";
    else if (mode == async_read_cursor_get_loop_from_gte)
      read_name = "mdbx_async_cursor_get_loop_from TO_KEY_GREATER_OR_EQUAL cold read";
    else if (mode == async_read_cursor_get_loop_from_gt)
      read_name = "mdbx_async_cursor_get_loop_from TO_KEY_GREATER_THAN cold read";
    else if (mode == async_read_cursor_get_loop_from_upperbound)
      read_name = "mdbx_async_cursor_get_loop_from SET_UPPERBOUND cold read";
    else if (mode == async_read_cursor_get_loop_from_nodup)
      read_name = "mdbx_async_cursor_get_loop_from NEXT_NODUP cold read";
    else if (mode == async_read_cursor_get_batches)
      read_name = "mdbx_async_cursor_get_batches cold read";
    else if (mode == async_read_cursor_get_batches_current) {
      MDBX_async_op *prime_op = NULL;
      cursor_from_key = batch_key_values[0];
      cursor_from_data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &cursor_from_key,
                                  &cursor_from_data, MDBX_SET_KEY, &prime_op));
      CHECK(wait_result("mdbx_async_cursor_get_batches GET_CURRENT prime",
                        &prime_op, &operation_result, __FILE__, __LINE__));
      REQUIRE(operation_result == MDBX_SUCCESS,
              "unexpected async cursor batches GET_CURRENT prime result");
      operation_result = MDBX_SUCCESS;
      read_name = "mdbx_async_cursor_get_batches_from GET_CURRENT cold read";
    }
    else if (mode == async_read_cursor_get_batches_from)
      read_name = "mdbx_async_cursor_get_batches_from cold read";
    else if (mode == async_read_cursor_get_batches_from_setkey)
      read_name = "mdbx_async_cursor_get_batches_from SET_KEY cold read";
    else if (mode == async_read_cursor_get_batches_from_setkey_miss)
      read_name = "mdbx_async_cursor_get_batches_from SET_KEY cold missing read";
    else if (mode == async_read_cursor_get_batches_from_equal)
      read_name = "mdbx_async_cursor_get_batches_from TO_KEY_EQUAL cold read";
    else if (mode == async_read_cursor_get_batches_from_equal_miss)
      read_name = "mdbx_async_cursor_get_batches_from TO_KEY_EQUAL cold missing read";
    else if (mode == async_read_cursor_get_batches_from_gte)
      read_name = "mdbx_async_cursor_get_batches_from TO_KEY_GREATER_OR_EQUAL cold read";
    else if (mode == async_read_cursor_get_batches_from_gt)
      read_name = "mdbx_async_cursor_get_batches_from TO_KEY_GREATER_THAN cold read";
    else if (mode == async_read_cursor_get_batches_from_upperbound)
      read_name = "mdbx_async_cursor_get_batches_from SET_UPPERBOUND cold read";
    else if (mode == async_read_cursor_scan)
      read_name = "mdbx_async_cursor_scan cold read";
    else if (mode == async_read_cursor_scan_current) {
      MDBX_async_op *prime_op = NULL;
      cursor_from_key = batch_key_values[0];
      cursor_from_data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &cursor_from_key,
                                  &cursor_from_data, MDBX_SET_KEY, &prime_op));
      CHECK(wait_result("mdbx_async_cursor_scan GET_CURRENT prime",
                        &prime_op, &operation_result, __FILE__, __LINE__));
      REQUIRE(operation_result == MDBX_SUCCESS,
              "unexpected async cursor scan GET_CURRENT prime result");
      operation_result = MDBX_SUCCESS;
      read_name = "mdbx_async_cursor_scan GET_CURRENT cold read";
    } else if (mode == async_read_cursor_scan_current_prev) {
      MDBX_async_op *prime_op = NULL;
      cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
      cursor_from_data = val(NULL, 0);
      CHECK(mdbx_async_cursor_get(async, cursor, &cursor_from_key,
                                  &cursor_from_data, MDBX_SET_KEY, &prime_op));
      CHECK(wait_result("mdbx_async_cursor_scan GET_CURRENT PREV prime",
                        &prime_op, &operation_result, __FILE__, __LINE__));
      REQUIRE(operation_result == MDBX_SUCCESS,
              "unexpected async cursor scan GET_CURRENT PREV prime result");
      operation_result = MDBX_SUCCESS;
      read_name = "mdbx_async_cursor_scan GET_CURRENT PREV cold read";
    }
    else if (mode == async_read_cursor_scan_prev)
      read_name = "mdbx_async_cursor_scan PREV cold read";
    else if (mode == async_read_cursor_scan_nodup)
      read_name = "mdbx_async_cursor_scan NEXT_NODUP cold read";
    else if (mode == async_read_cursor_scan_from_prev)
      read_name = "mdbx_async_cursor_scan_from PREV cold read";
    else if (mode == async_read_cursor_scan_from_nodup)
      read_name = "mdbx_async_cursor_scan_from NEXT_NODUP cold read";
    else if (mode == async_read_cursor_scan_from_setkey)
      read_name = "mdbx_async_cursor_scan_from SET_KEY cold read";
    else if (mode == async_read_cursor_scan_from_setkey_miss)
      read_name = "mdbx_async_cursor_scan_from SET_KEY cold missing read";
    else if (mode == async_read_cursor_scan_from_equal)
      read_name = "mdbx_async_cursor_scan_from TO_KEY_EQUAL cold read";
    else if (mode == async_read_cursor_scan_from_equal_miss)
      read_name = "mdbx_async_cursor_scan_from TO_KEY_EQUAL cold missing read";
    else if (mode == async_read_cursor_scan_from_gte)
      read_name = "mdbx_async_cursor_scan_from TO_KEY_GREATER_OR_EQUAL cold read";
    else if (mode == async_read_cursor_scan_from_gt)
      read_name = "mdbx_async_cursor_scan_from TO_KEY_GREATER_THAN cold read";
    else if (mode == async_read_cursor_scan_from_upperbound)
      read_name = "mdbx_async_cursor_scan_from SET_UPPERBOUND cold read";
    else
      read_name = "mdbx_async_cursor_scan_from cold read";
  } else if (mode == async_read_get_notfound) {
    read_name = "mdbx_async_get cold missing read";
  } else if (cache_single_read || mode == async_read_cache_get_notfound) {
    mdbx_cache_init(&cache_entry);
    if (mode == async_read_cache_get_notfound)
      read_name = "mdbx_async_cache_get cold missing read";
    else if (mode == async_read_cache_get_singlethreaded)
      read_name = "mdbx_async_cache_get_SingleThreaded cold single read";
    else
      read_name = "mdbx_async_cache_get cold single read";
  } else if (mode == async_read_cache_get_many ||
             mode == async_read_cache_get_many_mixed) {
    read_name = "mdbx_async_cache_get_many cold read";
  } else if (mode == async_read_cache_get_singlethreaded_many ||
             mode == async_read_cache_get_singlethreaded_many_mixed) {
    read_name = "mdbx_async_cache_get_SingleThreaded_many cold read";
  } else if (mode == async_read_large_get) {
    read_name = "mdbx_async_get cold large read";
  } else if (mode == async_read_large_get_ex) {
    read_name = "mdbx_async_get_ex cold large read";
  } else if (mode == async_read_large_get_many) {
    read_name = "mdbx_async_get_many cold large read";
  } else if (mode == async_read_large_get_ex_many) {
    read_name = "mdbx_async_get_ex_many cold large read";
  } else if (mode == async_read_large_get_batch) {
    read_name = "mdbx_async_get_batch cold large read";
  } else if (mode == async_read_large_get_ex_batch) {
    read_name = "mdbx_async_get_ex_batch cold large read";
  } else if (mode == async_read_large_get_loop) {
    read_name = "mdbx_async_get_loop cold large read";
  } else if (mode == async_read_large_get_ex_loop) {
    read_name = "mdbx_async_get_ex_loop cold large read";
  } else if (mode == async_read_large_lowerbound) {
    read_name = "mdbx_async_get_equal_or_great cold large read";
  } else if (mode == async_read_large_lowerbound_many) {
    read_name = "mdbx_async_get_equal_or_great_many cold large read";
  } else if (mode == async_read_large_lowerbound_batch) {
    read_name = "mdbx_async_get_equal_or_great_batch cold large read";
  } else if (mode == async_read_large_lowerbound_loop) {
    read_name = "mdbx_async_get_equal_or_great_loop cold large read";
  } else if (mode == async_read_large_cursor_batch) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    read_name = "mdbx_async_cursor_get_batch cold large read";
  } else if (large_cursor_stream_read) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    if (mode == async_read_large_cursor_loop)
      read_name = "mdbx_async_cursor_get_loop cold large read";
    else if (mode == async_read_large_cursor_batches)
      read_name = "mdbx_async_cursor_get_batches cold large read";
    else
      read_name = "mdbx_async_cursor_scan cold large read";
  } else if (mode == async_read_large_cache_get) {
    mdbx_cache_init(&cache_entry);
    read_name = "mdbx_async_cache_get cold large read";
  } else if (mode == async_read_large_cache_get_singlethreaded) {
    mdbx_cache_init(&cache_entry);
    read_name = "mdbx_async_cache_get_SingleThreaded cold large read";
  } else if (mode == async_read_large_cache_get_many) {
    read_name = "mdbx_async_cache_get_many cold large read";
  } else if (mode == async_read_large_cache_get_singlethreaded_many) {
    read_name = "mdbx_async_cache_get_SingleThreaded_many cold large read";
  } else if (mode == async_read_large_cache_get_batch) {
    read_name = "mdbx_async_cache_get_batch cold large read";
  } else if (mode == async_read_large_cache_get_singlethreaded_batch) {
    read_name = "mdbx_async_cache_get_SingleThreaded_batch cold large read";
  } else if (mode == async_read_large_cache_get_loop) {
    read_name = "mdbx_async_cache_get_loop cold large read";
  } else if (mode == async_read_large_cache_get_singlethreaded_loop) {
    read_name = "mdbx_async_cache_get_SingleThreaded_loop cold large read";
  } else if (mode == async_read_abort_order) {
    read_name = "mdbx_async_get cold read before txn abort";
  } else if (mode == async_read_abort_order_batch) {
    read_name = "async read batches before txn abort";
  } else if (mode == async_read_abort_order_loop) {
    read_name = "async read loops before txn abort";
  } else if (mode == async_read_get_batch ||
             mode == async_read_get_batch_mixed) {
    read_name = "mdbx_async_get_batch cold read";
  } else if (mode == async_read_get_ex_batch ||
             mode == async_read_get_ex_batch_mixed) {
    read_name = "mdbx_async_get_ex_batch cold read";
  } else if (mode == async_read_lowerbound_batch ||
             mode == async_read_lowerbound_batch_mixed) {
    read_name = "mdbx_async_get_equal_or_great_batch cold read";
  } else if (mode == async_read_cache_get_batch ||
             mode == async_read_cache_get_batch_mixed) {
    read_name = "mdbx_async_cache_get_batch cold read";
  } else if (mode == async_read_cache_get_singlethreaded_batch ||
             mode == async_read_cache_get_singlethreaded_batch_mixed) {
    read_name = "mdbx_async_cache_get_SingleThreaded_batch cold read";
  } else if (mode == async_read_get_loop) {
    read_name = "mdbx_async_get_loop cold read";
  } else if (mode == async_read_cache_get_loop) {
    read_name = "mdbx_async_cache_get_loop cold read";
  } else if (mode == async_read_cache_get_singlethreaded_loop) {
    read_name = "mdbx_async_cache_get_SingleThreaded_loop cold read";
  } else if (mode == async_read_get_ex_loop) {
    read_name = "mdbx_async_get_ex_loop cold read";
  } else if (mode == async_read_lowerbound_loop) {
    read_name = "mdbx_async_get_equal_or_great_loop cold read";
  }
  memset(&read_stats, 0, sizeof(read_stats));
  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), true));

  CHECK(unset_env_var("MDBX_TEST_DXB_FAULT"));
  if (inject_fault)
    CHECK(set_env_var("MDBX_TEST_DXB_FAULT", "read-complete:EIO"));
  if (cache_hit_read) {
    CHECK(mdbx_async_get(async, txn, dbi, read_key_value, &data, &op));
    CHECK(wait_result("mdbx_async_get warmup cold read", &op, &operation_result, __FILE__, __LINE__));
    REQUIRE(operation_result == MDBX_SUCCESS, "warmup cold async get returned wrong result");
    CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), true));
    REQUIRE(read_stats.storage_read_items > 0, "warmup cold async read did not submit storage reads");
    REQUIRE(read_stats.page_cache_misses > 0, "warmup cold async read did not report page-cache misses");
    REQUIRE(read_stats.page_cache_fills > 0, "warmup cold async read did not fill page-cache entries");
    data = val(NULL, 0);
    operation_result = MDBX_SUCCESS;
  }
  if (mixed_batch_read)
    batch_key_values[1] = missing_batch_key_value;
  if (mode == async_read_abort_order) {
    CHECK(async_block_probe_prepare(&abort_probe));
    abort_probe_prepared = true;
    CHECK(mdbx_async_submit(async, async_block_probe_func, &abort_probe, &abort_ops[0]));
    CHECK(mdbx_async_get(async, txn, dbi, read_key_value, &data, &abort_ops[1]));
    CHECK(mdbx_async_txn_abort(async, txn, NULL, &abort_ops[2]));
    txn = NULL;
    const int early_release_rc = mdbx_async_op_release(abort_ops[1]);
    const int early_destroy_rc = mdbx_async_destroy(async, false);
    CHECK(async_block_probe_release(&abort_probe));
    CHECK(wait_many_success(read_name, abort_ops, 3, abort_results, __FILE__, __LINE__));
    async_block_probe_close(&abort_probe);
    abort_probe_prepared = false;
    REQUIRE(abort_probe.calls == 1, "abort-order async blocker did not run exactly once");
    REQUIRE(early_release_rc == MDBX_BUSY, "pending cold async read operation was released early");
    REQUIRE(early_destroy_rc == MDBX_BUSY, "async executor destroyed with pending cold read operation");
  } else if (mode == async_read_abort_order_batch) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    CHECK(async_block_probe_prepare(&abort_probe));
    abort_probe_prepared = true;
    CHECK(mdbx_async_submit(async, async_block_probe_func, &abort_probe, &abort_ops[0]));
    for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
      abort_get_ex_batch_keys[i] = batch_key_values[i];
      abort_lower_batch_keys[i] = batch_key_values[i];
    }
    CHECK(mdbx_async_get_batch(async, txn, dbi, batch_key_values,
                               abort_get_batch_data, abort_get_batch_results,
                               ASYNC_READ_BATCH_COUNT, &abort_ops[1]));
    CHECK(mdbx_async_get_ex_batch(async, txn, dbi, abort_get_ex_batch_keys,
                                  abort_get_ex_batch_data,
                                  abort_get_ex_batch_counts,
                                  abort_get_ex_batch_results,
                                  ASYNC_READ_BATCH_COUNT, &abort_ops[2]));
    CHECK(mdbx_async_get_equal_or_great_batch(async, txn, dbi,
                                              abort_lower_batch_keys,
                                              abort_lower_batch_data,
                                              abort_lower_batch_results,
                                              ASYNC_READ_BATCH_COUNT,
                                              &abort_ops[3]));
    CHECK(mdbx_async_cache_get_batch(async, txn, dbi, batch_key_values,
                                     abort_cache_data, abort_cache_entries,
                                     abort_cache_results,
                                     ASYNC_READ_BATCH_COUNT, &abort_ops[4]));
    abort_cursor_count = 0;
    memset(abort_cursor_pairs, 0, sizeof(abort_cursor_pairs));
    CHECK(mdbx_async_cursor_get_batch(async, cursor, &abort_cursor_count,
                                      abort_cursor_pairs, 4, MDBX_FIRST,
                                      &abort_ops[5]));
    CHECK(mdbx_async_txn_abort(async, txn, NULL, &abort_ops[6]));
    txn = NULL;
    cursor = NULL;
    const int early_release_rc = mdbx_async_op_release(abort_ops[1]);
    const int early_destroy_rc = mdbx_async_destroy(async, false);
    CHECK(async_block_probe_release(&abort_probe));
    CHECK(wait_many_success(read_name, abort_ops, 7, abort_results, __FILE__, __LINE__));
    async_block_probe_close(&abort_probe);
    abort_probe_prepared = false;
    REQUIRE(abort_probe.calls == 1, "batch abort-order async blocker did not run exactly once");
    REQUIRE(early_release_rc == MDBX_BUSY, "pending batch async read operation was released early");
    REQUIRE(early_destroy_rc == MDBX_BUSY, "async executor destroyed with pending batch read operation");
  } else if (mode == async_read_abort_order_loop) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
    CHECK_OP(op);
    cursor_is_async = true;
    CHECK(async_block_probe_prepare(&abort_probe));
    abort_probe_prepared = true;
    CHECK(mdbx_async_submit(async, async_block_probe_func, &abort_probe, &abort_ops[0]));
    CHECK(mdbx_async_get_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                              async_read_loop_key_func,
                              async_read_loop_result_func,
                              &abort_get_loop_probe,
                              &abort_get_loop_completed, &abort_ops[1]));
    CHECK(mdbx_async_get_ex_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                 async_read_loop_key_func,
                                 async_read_get_ex_loop_result_func,
                                 &abort_get_ex_loop_probe,
                                 &abort_get_ex_loop_completed, &abort_ops[2]));
    CHECK(mdbx_async_get_equal_or_great_loop(async, txn, dbi,
                                             ASYNC_READ_BATCH_COUNT,
                                             async_read_loop_key_func,
                                             async_read_lowerbound_loop_data_func,
                                             async_read_lowerbound_loop_result_func,
                                             &abort_lower_loop_probe,
                                             &abort_lower_loop_completed,
                                             &abort_ops[3]));
    CHECK(mdbx_async_cache_get_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                    async_read_loop_key_func,
                                    abort_loop_cache_entries,
                                    async_read_cache_loop_result_func,
                                    &abort_cache_loop_probe,
                                    &abort_cache_loop_completed, &abort_ops[4]));
    CHECK(mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT,
                                     MDBX_FIRST, MDBX_NEXT,
                                     async_read_cursor_loop_result_func,
                                     &abort_cursor_loop_probe,
                                     &abort_cursor_loop_completed, &abort_ops[5]));
    CHECK(mdbx_async_txn_abort(async, txn, NULL, &abort_ops[6]));
    txn = NULL;
    cursor = NULL;
    const int early_release_rc = mdbx_async_op_release(abort_ops[1]);
    const int early_destroy_rc = mdbx_async_destroy(async, false);
    CHECK(async_block_probe_release(&abort_probe));
    CHECK(wait_many_success(read_name, abort_ops, 7, abort_results, __FILE__, __LINE__));
    async_block_probe_close(&abort_probe);
    abort_probe_prepared = false;
    REQUIRE(abort_probe.calls == 1, "loop abort-order async blocker did not run exactly once");
    REQUIRE(early_release_rc == MDBX_BUSY, "pending loop async read operation was released early");
    REQUIRE(early_destroy_rc == MDBX_BUSY, "async executor destroyed with pending loop read operation");
  } else if (mode == async_read_sync_get)
    rc = mdbx_get(txn, dbi, read_key_value, &data);
  else if (mode == async_read_sync_get_ex)
    rc = mdbx_get_ex(txn, dbi, read_key_value, &data, &values_count);
  else if (mode == async_read_sync_lowerbound)
    rc = mdbx_get_equal_or_great(txn, dbi, read_key_value, &data);
  else if (mode == async_read_sync_cursor_get ||
           mode == async_read_sync_cursor_get_miss)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_SET_KEY);
  else if (mode == async_read_sync_cursor_get_set)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_SET);
  else if (mode == async_read_sync_cursor_get_range)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_SET_RANGE);
  else if (mode == async_read_sync_cursor_get_upperbound)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_SET_UPPERBOUND);
  else if (mode == async_read_sync_cursor_get_to_key_equal ||
           mode == async_read_sync_cursor_get_to_key_equal_miss)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_TO_KEY_EQUAL);
  else if (mode == async_read_sync_cursor_get_to_key_gte)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_TO_KEY_GREATER_OR_EQUAL);
  else if (mode == async_read_sync_cursor_get_to_key_gt)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_TO_KEY_GREATER_THAN);
  else if (mode == async_read_sync_cursor_get_last)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_LAST);
  else if (mode == async_read_sync_cursor_get_prev)
    rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_PREV);
  else if (mode == async_read_sync_cursor_get_prev_positioned) {
    MDBX_val last_key = val(NULL, 0);
    MDBX_val last_data = val(NULL, 0);
    rc = mdbx_cursor_get(cursor, &last_key, &last_data, MDBX_LAST);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_cursor_get(cursor, read_key_value, &data, MDBX_PREV);
  }
  else if (mode == async_read_sync_cursor_get_nodup) {
    MDBX_val first_key = val(NULL, 0);
    MDBX_val first_data = val(NULL, 0);
    rc = mdbx_cursor_get(cursor, &first_key, &first_data, MDBX_FIRST);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_cursor_get(cursor, &cursor_from_key, &data, MDBX_NEXT_NODUP);
  } else if (mode == async_read_sync_cursor_get_batch) {
    cursor_batch_count = 0;
    memset(cursor_batch_pairs, 0, sizeof(cursor_batch_pairs));
    rc = mdbx_cursor_get_batch(cursor, &cursor_batch_count, cursor_batch_pairs,
                               ASYNC_READ_BATCH_COUNT * 2, MDBX_FIRST);
  } else if (mode == async_read_sync_cursor_get_batch_nodup) {
    cursor_prime_count = 0;
    cursor_batch_count = 0;
    cursor_batch_start_index = 2;
    memset(cursor_prime_pairs, 0, sizeof(cursor_prime_pairs));
    memset(cursor_batch_pairs, 0, sizeof(cursor_batch_pairs));
    rc = mdbx_cursor_get_batch(cursor, &cursor_prime_count, cursor_prime_pairs,
                               4, MDBX_FIRST);
    cursor_prime_result = rc;
    if (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE)
      rc = mdbx_cursor_get_batch(cursor, &cursor_batch_count, cursor_batch_pairs,
                                 ASYNC_READ_BATCH_COUNT * 2, MDBX_NEXT_NODUP);
  } else if (mode == async_read_sync_cursor_scan) {
    rc = mdbx_cursor_scan(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                          MDBX_FIRST, MDBX_NEXT, NULL);
  } else if (mode == async_read_sync_cursor_scan_current) {
    rc = mdbx_cursor_scan(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                          MDBX_GET_CURRENT, MDBX_NEXT, NULL);
  } else if (mode == async_read_sync_cursor_scan_current_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_cursor_scan(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                          MDBX_GET_CURRENT, MDBX_PREV, NULL);
  } else if (mode == async_read_sync_cursor_scan_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_cursor_scan(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                          MDBX_LAST, MDBX_PREV, NULL);
  } else if (mode == async_read_sync_cursor_scan_nodup) {
    rc = mdbx_cursor_scan(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                          MDBX_FIRST, MDBX_NEXT_NODUP, NULL);
  } else if (mode == async_read_sync_cursor_scan_from) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_cursor_scan_from(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                               MDBX_SET_LOWERBOUND, &cursor_from_key, &cursor_from_data,
                               MDBX_NEXT, NULL);
  } else if (mode == async_read_sync_cursor_scan_from_prev) {
    cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
    cursor_from_data = val(NULL, 0);
    loop_read_probe.reverse = true;
    rc = mdbx_cursor_scan_from(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                               MDBX_SET_KEY, &cursor_from_key, &cursor_from_data,
                               MDBX_PREV, NULL);
  } else if (mode == async_read_sync_cursor_scan_from_nodup) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_cursor_scan_from(cursor, async_read_cursor_scan_result_func, &loop_read_probe,
                               MDBX_SET_LOWERBOUND, &cursor_from_key, &cursor_from_data,
                               MDBX_NEXT_NODUP, NULL);
  } else if (mode == async_read_cursor_get ||
             mode == async_read_cursor_get_miss)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_SET_KEY, &op);
  else if (mode == async_read_cursor_get_set)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_SET, &op);
  else if (mode == async_read_cursor_get_range)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_SET_RANGE, &op);
  else if (mode == async_read_cursor_get_upperbound)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_SET_UPPERBOUND, &op);
  else if (mode == async_read_cursor_get_to_key_equal ||
           mode == async_read_cursor_get_to_key_equal_miss)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_TO_KEY_EQUAL, &op);
  else if (mode == async_read_cursor_get_to_key_gte)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_TO_KEY_GREATER_OR_EQUAL, &op);
  else if (mode == async_read_cursor_get_to_key_gt)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_TO_KEY_GREATER_THAN, &op);
  else if (mode == async_read_cursor_get_last)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_LAST, &op);
  else if (mode == async_read_cursor_get_prev)
    rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_PREV, &op);
  else if (mode == async_read_cursor_get_prev_positioned) {
    MDBX_val last_key = val(NULL, 0);
    MDBX_val last_data = val(NULL, 0);
    MDBX_async_op *prime_op = NULL;
    rc = mdbx_async_cursor_get(async, cursor, &last_key, &last_data, MDBX_LAST, &prime_op);
    if (rc == MDBX_SUCCESS)
      rc = wait_result("mdbx_async_cursor_get LAST before PREV", &prime_op,
                       &cursor_prime_result, __FILE__, __LINE__);
    if (rc == MDBX_SUCCESS && cursor_prime_result == MDBX_SUCCESS)
      rc = mdbx_async_cursor_get(async, cursor, read_key_value, &data, MDBX_PREV, &op);
  }
  else if (mode == async_read_get_ex ||
           mode == async_read_large_get_ex)
    rc = mdbx_async_get_ex(async, txn, dbi, read_key_value, &data, &values_count, &op);
  else if (mode == async_read_lowerbound)
    rc = mdbx_async_get_equal_or_great(async, txn, dbi, read_key_value, &data, &op);
  else if (mode == async_read_get_many ||
           mode == async_read_get_many_mixed)
    rc = mdbx_async_get_many(async, txn, dbi, batch_key_values, batch_data,
                             ASYNC_READ_BATCH_COUNT, many_ops);
  else if (mode == async_read_large_get_many)
    rc = mdbx_async_get_many(async, txn, dbi, batch_key_values, batch_data,
                             indexed_read_count, many_ops);
  else if (mode == async_read_get_ex_many ||
           mode == async_read_get_ex_many_mixed)
    rc = mdbx_async_get_ex_many(async, txn, dbi, batch_key_values, batch_data,
                                batch_values_counts, ASYNC_READ_BATCH_COUNT, many_ops);
  else if (mode == async_read_large_get_ex_many)
    rc = mdbx_async_get_ex_many(async, txn, dbi, batch_key_values, batch_data,
                                batch_values_counts, indexed_read_count, many_ops);
  else if (mode == async_read_lowerbound_many ||
           mode == async_read_lowerbound_many_mixed)
    rc = mdbx_async_get_equal_or_great_many(async, txn, dbi, batch_key_values,
                                            batch_data, ASYNC_READ_BATCH_COUNT, many_ops);
  else if (mode == async_read_large_lowerbound_many)
    rc = mdbx_async_get_equal_or_great_many(async, txn, dbi, batch_key_values,
                                            batch_data, indexed_read_count, many_ops);
  else if (mode == async_read_cursor_get_batch) {
    cursor_batch_count = 0;
    memset(cursor_batch_pairs, 0, sizeof(cursor_batch_pairs));
    rc = mdbx_async_cursor_get_batch(async, cursor, &cursor_batch_count, cursor_batch_pairs,
                                     ASYNC_READ_BATCH_COUNT * 2, MDBX_FIRST, &op);
  } else if (mode == async_read_large_cursor_batch) {
    cursor_batch_count = 0;
    memset(cursor_batch_pairs, 0, sizeof(cursor_batch_pairs));
    rc = mdbx_async_cursor_get_batch(async, cursor, &cursor_batch_count,
                                     cursor_batch_pairs, 4, MDBX_FIRST, &op);
  } else if (mode == async_read_large_cursor_loop) {
    rc = mdbx_async_cursor_get_loop(async, cursor, LARGE_ITEM_COUNT,
                                    MDBX_FIRST, MDBX_NEXT,
                                    async_read_large_cursor_loop_result_func,
                                    &large_cursor_probe,
                                    &cursor_stream_completed, &op);
  } else if (mode == async_read_large_cursor_batches) {
    rc = mdbx_async_cursor_get_batches(async, cursor, LARGE_ITEM_COUNT, 2,
                                       async_read_large_cursor_batches_result_func,
                                       &large_cursor_probe,
                                       &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batch_nodup) {
    MDBX_async_op *prime_op = NULL;
    cursor_prime_count = 0;
    cursor_batch_count = 0;
    cursor_batch_start_index = 2;
    memset(cursor_prime_pairs, 0, sizeof(cursor_prime_pairs));
    memset(cursor_batch_pairs, 0, sizeof(cursor_batch_pairs));
    rc = mdbx_async_cursor_get_batch(async, cursor, &cursor_prime_count, cursor_prime_pairs,
                                     4, MDBX_FIRST, &prime_op);
    if (rc == MDBX_SUCCESS) {
      rc = wait_result("mdbx_async_cursor_get_batch NEXT_NODUP prime", &prime_op,
                       &cursor_prime_result, __FILE__, __LINE__);
      if (rc == MDBX_SUCCESS &&
          (cursor_prime_result == MDBX_SUCCESS || cursor_prime_result == MDBX_RESULT_TRUE))
        rc = mdbx_async_cursor_get_batch(async, cursor, &cursor_batch_count,
                                         cursor_batch_pairs, ASYNC_READ_BATCH_COUNT * 2,
                                         MDBX_NEXT_NODUP, &op);
    }
  } else if (mode == async_read_cursor_get_loop) {
    rc = mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT, MDBX_FIRST,
                                    MDBX_NEXT, async_read_cursor_loop_result_func,
                                    &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT, MDBX_LAST,
                                    MDBX_PREV, async_read_cursor_loop_result_func,
                                    &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_current) {
    rc = mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT,
                                    MDBX_GET_CURRENT, MDBX_NEXT,
                                    async_read_cursor_loop_result_func,
                                    &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_current_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT,
                                    MDBX_GET_CURRENT, MDBX_PREV,
                                    async_read_cursor_loop_result_func,
                                    &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_nodup) {
    rc = mdbx_async_cursor_get_loop(async, cursor, ASYNC_READ_BATCH_COUNT, MDBX_FIRST,
                                    MDBX_NEXT_NODUP, async_read_cursor_loop_result_func,
                                    &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, ASYNC_READ_BATCH_COUNT,
                                         MDBX_SET_LOWERBOUND, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_prev) {
    cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
    cursor_from_data = val(NULL, 0);
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_get_loop_from(async, cursor, ASYNC_READ_BATCH_COUNT,
                                         MDBX_SET_KEY, &cursor_from_key,
                                         &cursor_from_data, MDBX_PREV,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_setkey) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_SET_KEY, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_setkey_miss) {
    cursor_from_key = missing_batch_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_SET_KEY, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_equal) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_TO_KEY_EQUAL, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_equal_miss) {
    cursor_from_key = zero_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_TO_KEY_EQUAL, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_gte) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_TO_KEY_GREATER_OR_EQUAL, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_gt) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_TO_KEY_GREATER_THAN, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_upperbound) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, cursor_stream_target,
                                         MDBX_SET_UPPERBOUND, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_loop_from_nodup) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_loop_from(async, cursor, ASYNC_READ_BATCH_COUNT,
                                         MDBX_SET_LOWERBOUND, &cursor_from_key,
                                         &cursor_from_data, MDBX_NEXT_NODUP,
                                         async_read_cursor_loop_result_func,
                                         &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches) {
    rc = mdbx_async_cursor_get_batches(async, cursor, ASYNC_READ_BATCH_COUNT, 8,
                                       async_read_cursor_batches_result_func,
                                       &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_current) {
    rc = mdbx_async_cursor_get_batches_from(async, cursor, ASYNC_READ_BATCH_COUNT, 8,
                                            MDBX_GET_CURRENT, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, ASYNC_READ_BATCH_COUNT, 8,
                                            MDBX_SET_LOWERBOUND, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_setkey) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_SET_KEY, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_setkey_miss) {
    cursor_from_key = missing_batch_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_SET_KEY, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_equal) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_TO_KEY_EQUAL, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_equal_miss) {
    cursor_from_key = zero_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_TO_KEY_EQUAL, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_gte) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_TO_KEY_GREATER_OR_EQUAL, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_gt) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_TO_KEY_GREATER_THAN, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_get_batches_from_upperbound) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_get_batches_from(async, cursor, cursor_stream_target, 8,
                                            MDBX_SET_UPPERBOUND, &cursor_from_key,
                                            &cursor_from_data,
                                            async_read_cursor_batches_result_func,
                                            &loop_read_probe, &cursor_stream_completed, &op);
  } else if (mode == async_read_cursor_scan) {
    rc = mdbx_async_cursor_scan(async, cursor, async_read_cursor_scan_result_func,
                                &loop_read_probe, MDBX_FIRST, MDBX_NEXT, NULL, &op);
  } else if (mode == async_read_cursor_scan_current) {
    rc = mdbx_async_cursor_scan(async, cursor, async_read_cursor_scan_result_func,
                                &loop_read_probe, MDBX_GET_CURRENT, MDBX_NEXT, NULL,
                                &op);
  } else if (mode == async_read_cursor_scan_current_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_scan(async, cursor, async_read_cursor_scan_result_func,
                                &loop_read_probe, MDBX_GET_CURRENT, MDBX_PREV, NULL,
                                &op);
  } else if (mode == async_read_cursor_scan_prev) {
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_scan(async, cursor, async_read_cursor_scan_result_func,
                                &loop_read_probe, MDBX_LAST, MDBX_PREV, NULL, &op);
  } else if (mode == async_read_cursor_scan_nodup) {
    rc = mdbx_async_cursor_scan(async, cursor, async_read_cursor_scan_result_func,
                                &loop_read_probe, MDBX_FIRST, MDBX_NEXT_NODUP, NULL, &op);
  } else if (mode == async_read_cursor_scan_from) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_LOWERBOUND,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_prev) {
    cursor_from_key = batch_key_values[ASYNC_READ_BATCH_COUNT - 1];
    cursor_from_data = val(NULL, 0);
    loop_read_probe.reverse = true;
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_KEY,
                                     &cursor_from_key, &cursor_from_data, MDBX_PREV,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_setkey) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_KEY,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_setkey_miss) {
    cursor_from_key = missing_batch_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_KEY,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_equal) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_TO_KEY_EQUAL,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_equal_miss) {
    cursor_from_key = zero_key_value;
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_TO_KEY_EQUAL,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_gte) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_TO_KEY_GREATER_OR_EQUAL,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_gt) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_TO_KEY_GREATER_THAN,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_upperbound) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_UPPERBOUND,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT,
                                     NULL, &op);
  } else if (mode == async_read_cursor_scan_from_nodup) {
    cursor_from_key = batch_key_values[0];
    cursor_from_data = val(NULL, 0);
    rc = mdbx_async_cursor_scan_from(async, cursor, async_read_cursor_scan_result_func,
                                     &loop_read_probe, MDBX_SET_LOWERBOUND,
                                     &cursor_from_key, &cursor_from_data, MDBX_NEXT_NODUP,
                                     NULL, &op);
  } else if (mode == async_read_large_cursor_scan) {
    rc = mdbx_async_cursor_scan(async, cursor,
                                async_read_large_cursor_scan_result_func,
                                &large_cursor_probe, MDBX_FIRST, MDBX_NEXT,
                                NULL, &op);
  } else if (mode == async_read_cache_get || mode == async_read_cache_get_notfound ||
           mode == async_read_large_cache_get)
    rc = mdbx_async_cache_get(async, txn, dbi, read_key_value, &data, &cache_entry, &cache_result, &op);
  else if (mode == async_read_cache_get_singlethreaded ||
           mode == async_read_large_cache_get_singlethreaded)
    rc = mdbx_async_cache_get_SingleThreaded(async, txn, dbi, read_key_value, &data, &cache_entry,
                                             &cache_result, &op);
  else if (mode == async_read_cache_get_many ||
           mode == async_read_cache_get_many_mixed)
    rc = mdbx_async_cache_get_many(async, txn, dbi, batch_key_values, batch_data,
                                   batch_cache_entries, batch_cache_results,
                                   ASYNC_READ_BATCH_COUNT, many_ops);
  else if (mode == async_read_large_cache_get_many)
    rc = mdbx_async_cache_get_many(async, txn, dbi, batch_key_values, batch_data,
                                   batch_cache_entries, batch_cache_results,
                                   indexed_read_count, many_ops);
  else if (mode == async_read_large_cache_get_singlethreaded_many)
    rc = mdbx_async_cache_get_SingleThreaded_many(async, txn, dbi, batch_key_values,
                                                  batch_data, batch_cache_entries,
                                                  batch_cache_results,
                                                  indexed_read_count, many_ops);
  else if (mode == async_read_cache_get_singlethreaded_many ||
           mode == async_read_cache_get_singlethreaded_many_mixed)
    rc = mdbx_async_cache_get_SingleThreaded_many(async, txn, dbi, batch_key_values,
                                                  batch_data, batch_cache_entries,
                                                  batch_cache_results,
                                                  ASYNC_READ_BATCH_COUNT, many_ops);
  else if (mode == async_read_get_batch ||
           mode == async_read_get_batch_mixed)
    rc = mdbx_async_get_batch(async, txn, dbi, batch_key_values, batch_data, batch_results,
                              ASYNC_READ_BATCH_COUNT, &op);
  else if (mode == async_read_large_get_batch)
    rc = mdbx_async_get_batch(async, txn, dbi, batch_key_values, batch_data,
                              batch_results, indexed_read_count, &op);
  else if (mode == async_read_get_ex_batch ||
           mode == async_read_get_ex_batch_mixed)
    rc = mdbx_async_get_ex_batch(async, txn, dbi, batch_key_values, batch_data,
                                 batch_values_counts, batch_results,
                                 ASYNC_READ_BATCH_COUNT, &op);
  else if (mode == async_read_large_get_ex_batch)
    rc = mdbx_async_get_ex_batch(async, txn, dbi, batch_key_values, batch_data,
                                 batch_values_counts, batch_results,
                                 indexed_read_count, &op);
  else if (mode == async_read_lowerbound_batch ||
           mode == async_read_lowerbound_batch_mixed)
    rc = mdbx_async_get_equal_or_great_batch(async, txn, dbi, batch_key_values,
                                             batch_data, batch_results,
                                             ASYNC_READ_BATCH_COUNT, &op);
  else if (mode == async_read_large_lowerbound_batch)
    rc = mdbx_async_get_equal_or_great_batch(async, txn, dbi, batch_key_values,
                                             batch_data, batch_results,
                                             indexed_read_count, &op);
  else if (mode == async_read_cache_get_batch ||
           mode == async_read_cache_get_batch_mixed)
    rc = mdbx_async_cache_get_batch(async, txn, dbi, batch_key_values, batch_data,
                                    batch_cache_entries, batch_cache_results,
                                    ASYNC_READ_BATCH_COUNT, &op);
  else if (mode == async_read_large_cache_get_batch)
    rc = mdbx_async_cache_get_batch(async, txn, dbi, batch_key_values, batch_data,
                                    batch_cache_entries, batch_cache_results,
                                    indexed_read_count, &op);
  else if (mode == async_read_large_cache_get_singlethreaded_batch)
    rc = mdbx_async_cache_get_SingleThreaded_batch(async, txn, dbi, batch_key_values,
                                                   batch_data, batch_cache_entries,
                                                   batch_cache_results,
                                                   indexed_read_count, &op);
  else if (mode == async_read_cache_get_singlethreaded_batch ||
           mode == async_read_cache_get_singlethreaded_batch_mixed)
    rc = mdbx_async_cache_get_SingleThreaded_batch(async, txn, dbi, batch_key_values,
                                                   batch_data, batch_cache_entries,
                                                   batch_cache_results,
                                                   ASYNC_READ_BATCH_COUNT, &op);
  else if (mode == async_read_get_loop)
    rc = mdbx_async_get_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT, async_read_loop_key_func,
                             async_read_loop_result_func, &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_large_get_loop)
    rc = mdbx_async_get_loop(async, txn, dbi, indexed_read_count,
                             async_read_loop_key_func,
                             async_read_large_loop_result_func,
                             &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_cache_get_loop)
    rc = mdbx_async_cache_get_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                   async_read_loop_key_func, batch_cache_entries,
                                   async_read_cache_loop_result_func, &loop_read_probe,
                                   &loop_completed, &op);
  else if (mode == async_read_large_cache_get_loop)
    rc = mdbx_async_cache_get_loop(async, txn, dbi, indexed_read_count,
                                   async_read_loop_key_func, batch_cache_entries,
                                   async_read_large_cache_loop_result_func, &loop_read_probe,
                                   &loop_completed, &op);
  else if (mode == async_read_large_cache_get_singlethreaded_loop)
    rc = mdbx_async_cache_get_SingleThreaded_loop(
        async, txn, dbi, indexed_read_count, async_read_loop_key_func,
        batch_cache_entries, async_read_large_cache_loop_result_func,
        &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_cache_get_singlethreaded_loop)
    rc = mdbx_async_cache_get_SingleThreaded_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                                  async_read_loop_key_func, batch_cache_entries,
                                                  async_read_cache_loop_result_func,
                                                  &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_get_ex_loop)
    rc = mdbx_async_get_ex_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                async_read_loop_key_func, async_read_get_ex_loop_result_func,
                                &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_large_get_ex_loop)
    rc = mdbx_async_get_ex_loop(async, txn, dbi, indexed_read_count,
                                async_read_loop_key_func,
                                async_read_large_get_ex_loop_result_func,
                                &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_lowerbound_loop)
    rc = mdbx_async_get_equal_or_great_loop(async, txn, dbi, ASYNC_READ_BATCH_COUNT,
                                            async_read_loop_key_func,
                                            async_read_lowerbound_loop_data_func,
                                            async_read_lowerbound_loop_result_func,
                                            &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_large_lowerbound_loop)
    rc = mdbx_async_get_equal_or_great_loop(async, txn, dbi, indexed_read_count,
                                            async_read_loop_key_func,
                                            async_read_lowerbound_loop_data_func,
                                            async_read_large_lowerbound_loop_result_func,
                                            &loop_read_probe, &loop_completed, &op);
  else if (mode == async_read_large_lowerbound)
    rc = mdbx_async_get_equal_or_great(async, txn, dbi, read_key_value, &data, &op);
  else
    rc = mdbx_async_get(async, txn, dbi, read_key_value, &data, &op);
  if (abort_order_read)
    operation_result = MDBX_SUCCESS;
  else if (many_read && rc == MDBX_SUCCESS) {
    rc = wait_many_result(read_name, many_ops, indexed_read_count,
                          many_operation_results, __FILE__, __LINE__);
    operation_result = MDBX_SUCCESS;
  } else if (sync_read) {
    operation_result = rc;
    if (mode == async_read_sync_cursor_get_batch ||
        mode == async_read_sync_cursor_get_batch_nodup) {
      cursor_batch_result = operation_result;
      if (operation_result == MDBX_RESULT_TRUE)
        operation_result = MDBX_SUCCESS;
    } else if (cursor_scan_read && operation_result == MDBX_RESULT_TRUE) {
      operation_result = MDBX_SUCCESS;
    }
    rc = MDBX_SUCCESS;
  } else if (rc == MDBX_SUCCESS) {
    rc = wait_result(read_name, &op, &operation_result, __FILE__, __LINE__);
    if (mode == async_read_cursor_get_batch ||
        mode == async_read_large_cursor_batch ||
        mode == async_read_cursor_get_batch_nodup) {
      cursor_batch_result = operation_result;
      if (operation_result == MDBX_RESULT_TRUE)
        operation_result = MDBX_SUCCESS;
    } else if (cursor_scan_read) {
      if (operation_result == MDBX_RESULT_TRUE)
        operation_result = MDBX_SUCCESS;
    } else if (mode == async_read_large_cursor_scan) {
      if (operation_result == MDBX_RESULT_TRUE)
        operation_result = MDBX_SUCCESS;
    }
  } else
    operation_result = rc;
  CHECK(unset_env_var("MDBX_TEST_DXB_FAULT"));
  if (rc != MDBX_SUCCESS)
    goto bailout;
  if (inject_fault) {
    if (mode == async_read_get || mode == async_read_large_get ||
        mode == async_read_large_lowerbound) {
      REQUIRE(operation_result == MDBX_EIO, "async get did not propagate injected read-completion failure");
      REQUIRE(data.iov_base == NULL && data.iov_len == 0, "failed async get returned data");
    } else if (mode == async_read_large_get_ex) {
      REQUIRE(operation_result == MDBX_EIO,
              "async get_ex did not propagate injected read-completion failure");
      REQUIRE(values_count == 0, "failed async get_ex returned value count");
      REQUIRE(data.iov_base == NULL && data.iov_len == 0,
              "failed async get_ex returned data");
    } else if (mode == async_read_large_get_many) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async get many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted large async get many returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted large async get many did not observe injected read failure");
    } else if (mode == async_read_large_get_ex_many) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async get_ex many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted large async get_ex many returned wrong item result");
        REQUIRE(many_operation_results[i] == MDBX_BAD_TXN ||
                    batch_values_counts[i] == 0,
                "faulted large async get_ex many returned value count");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted large async get_ex many did not observe injected read failure");
    } else if (mode == async_read_large_get_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async get batch operation returned wrong result");
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted large async get batch returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_get_ex_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async get_ex batch operation returned wrong result");
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted large async get_ex batch returned wrong item result");
        REQUIRE(batch_values_counts[i] == 0,
                "faulted large async get_ex batch returned value count");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_get_many) {
      REQUIRE(operation_result == MDBX_SUCCESS, "faulted async get many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted async get many returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted async get many did not observe injected read failure");
    } else if (mode == async_read_get_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS, "faulted async get batch operation returned wrong result");
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted async get batch returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_get_ex_many) {
      REQUIRE(operation_result == MDBX_SUCCESS, "faulted async get_ex many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted async get_ex many returned wrong item result");
        REQUIRE(many_operation_results[i] == MDBX_BAD_TXN ||
                    batch_values_counts[i] == 0,
                "faulted async get_ex many returned value count");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted async get_ex many did not observe injected read failure");
    } else if (mode == async_read_get_ex_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS, "faulted async get_ex batch operation returned wrong result");
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted async get_ex batch returned wrong item result");
        REQUIRE(batch_values_counts[i] == 0,
                "faulted async get_ex batch returned value count");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_lowerbound_many) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async lowerbound many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted async lowerbound many returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted async lowerbound many did not observe injected read failure");
    } else if (mode == async_read_large_lowerbound_many) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async lowerbound many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted large async lowerbound many returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio,
              "faulted large async lowerbound many did not observe injected read failure");
    } else if (mode == async_read_lowerbound_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async lowerbound batch operation returned wrong result");
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted async lowerbound batch returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_lowerbound_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async lowerbound batch operation returned wrong result");
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_EIO,
                "faulted large async lowerbound batch returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_cache_get_singlethreaded ||
               mode == async_read_large_cache_get_singlethreaded) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async single-threaded cache get did not propagate read-completion failure");
      REQUIRE(cache_result.errcode == MDBX_EIO &&
                  cache_result.status == MDBX_CACHE_ERROR,
              "faulted async single-threaded cache get returned wrong cache result");
      CHECK(expect_empty_value(&data, __FILE__, __LINE__));
    } else if (mode == async_read_cache_get_many ||
               mode == async_read_cache_get_singlethreaded_many ||
               mode == async_read_large_cache_get_many ||
               mode == async_read_large_cache_get_singlethreaded_many) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async cache get many operation returned wrong result");
      bool saw_eio = false;
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        saw_eio = saw_eio || many_operation_results[i] == MDBX_EIO ||
                  batch_cache_results[i].errcode == MDBX_EIO;
        REQUIRE(async_read_fault_result(many_operation_results[i]),
                "faulted async cache get many returned wrong operation result");
        REQUIRE(async_read_fault_result(batch_cache_results[i].errcode) &&
                    batch_cache_results[i].status == MDBX_CACHE_ERROR,
                "faulted async cache get many returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
      REQUIRE(saw_eio, "faulted async cache get many did not observe injected read failure");
    } else if (mode == async_read_cache_get_batch ||
               mode == async_read_cache_get_singlethreaded_batch ||
               mode == async_read_large_cache_get_batch ||
               mode == async_read_large_cache_get_singlethreaded_batch) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async cache get batch operation returned wrong result");
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_cache_results[i].errcode == MDBX_EIO &&
                    batch_cache_results[i].status == MDBX_CACHE_ERROR,
                "faulted async cache get batch returned wrong item result");
        CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_get_loop ||
               mode == async_read_large_get_loop) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async get loop operation returned wrong result");
      REQUIRE(loop_completed == indexed_read_count,
              "faulted async get loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "faulted async get loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "faulted async get loop saw wrong result count");
    } else if (mode == async_read_get_ex_loop ||
               mode == async_read_large_get_ex_loop) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async get_ex loop operation returned wrong result");
      REQUIRE(loop_completed == indexed_read_count,
              "faulted async get_ex loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "faulted async get_ex loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "faulted async get_ex loop saw wrong result count");
    } else if (mode == async_read_lowerbound_loop) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async lowerbound loop operation returned wrong result");
      REQUIRE(loop_completed == ASYNC_READ_BATCH_COUNT,
              "faulted async lowerbound loop completed wrong count");
      REQUIRE(loop_read_probe.keys == ASYNC_READ_BATCH_COUNT,
              "faulted async lowerbound loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == ASYNC_READ_BATCH_COUNT,
              "faulted async lowerbound loop saw wrong result count");
    } else if (mode == async_read_large_lowerbound_loop) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted large async lowerbound loop operation returned wrong result");
      REQUIRE(loop_completed == indexed_read_count,
              "faulted large async lowerbound loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "faulted large async lowerbound loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "faulted large async lowerbound loop saw wrong result count");
    } else if (mode == async_read_cache_get_loop ||
               mode == async_read_cache_get_singlethreaded_loop ||
               mode == async_read_large_cache_get_loop ||
               mode == async_read_large_cache_get_singlethreaded_loop) {
      REQUIRE(operation_result == MDBX_SUCCESS,
              "faulted async cache get loop operation returned wrong result");
      REQUIRE(loop_completed == indexed_read_count,
              "faulted async cache get loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "faulted async cache get loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "faulted async cache get loop saw wrong result count");
    } else if (mode == async_read_cursor_get ||
               mode == async_read_cursor_get_miss ||
               mode == async_read_cursor_get_last ||
               mode == async_read_cursor_get_prev) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async cursor get did not propagate read-completion failure");
      CHECK(expect_empty_value(&data, __FILE__, __LINE__));
    } else if (mode == async_read_cursor_get_batch ||
               mode == async_read_large_cursor_batch) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async cursor get batch did not propagate read-completion failure");
      REQUIRE(cursor_batch_result == MDBX_EIO,
              "faulted async cursor get batch recorded wrong cursor result");
      REQUIRE(cursor_batch_count == 0,
              "faulted async cursor get batch returned items");
    } else if (large_cursor_stream_read) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted large async cursor stream did not propagate read-completion failure");
      REQUIRE(cursor_stream_completed == 0,
              "faulted large async cursor stream completed items");
      REQUIRE(large_cursor_probe.results == 0,
              "faulted large async cursor stream invoked callback");
    } else if (mode == async_read_cursor_get_loop ||
               mode == async_read_cursor_get_loop_prev ||
               mode == async_read_cursor_get_loop_current ||
               mode == async_read_cursor_get_loop_current_prev ||
               mode == async_read_cursor_get_loop_nodup ||
               mode == async_read_cursor_get_loop_from ||
               mode == async_read_cursor_get_loop_from_prev ||
               mode == async_read_cursor_get_loop_from_setkey ||
               mode == async_read_cursor_get_loop_from_setkey_miss ||
               mode == async_read_cursor_get_loop_from_equal ||
               mode == async_read_cursor_get_loop_from_equal_miss ||
               mode == async_read_cursor_get_loop_from_gte ||
               mode == async_read_cursor_get_loop_from_gt ||
               mode == async_read_cursor_get_loop_from_upperbound ||
               mode == async_read_cursor_get_loop_from_nodup) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async cursor get loop did not propagate read-completion failure");
      REQUIRE(cursor_stream_completed == 0,
              "faulted async cursor get loop completed items");
      REQUIRE(loop_read_probe.results == 0,
              "faulted async cursor get loop invoked result callback");
    } else if (mode == async_read_cursor_get_batches ||
               mode == async_read_cursor_get_batches_current ||
               mode == async_read_cursor_get_batches_from ||
               mode == async_read_cursor_get_batches_from_setkey ||
               mode == async_read_cursor_get_batches_from_setkey_miss ||
               mode == async_read_cursor_get_batches_from_equal ||
               mode == async_read_cursor_get_batches_from_equal_miss ||
               mode == async_read_cursor_get_batches_from_gte ||
               mode == async_read_cursor_get_batches_from_gt ||
               mode == async_read_cursor_get_batches_from_upperbound) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async cursor batches did not propagate read-completion failure");
      REQUIRE(cursor_stream_completed == 0,
              "faulted async cursor batches completed items");
      REQUIRE(loop_read_probe.results == 0,
              "faulted async cursor batches invoked result callback");
    } else if (mode == async_read_cursor_scan ||
               mode == async_read_cursor_scan_current ||
               mode == async_read_cursor_scan_current_prev ||
               mode == async_read_cursor_scan_prev ||
               mode == async_read_cursor_scan_nodup ||
               mode == async_read_cursor_scan_from ||
               mode == async_read_cursor_scan_from_prev ||
               mode == async_read_cursor_scan_from_setkey ||
               mode == async_read_cursor_scan_from_setkey_miss ||
               mode == async_read_cursor_scan_from_equal ||
               mode == async_read_cursor_scan_from_equal_miss ||
               mode == async_read_cursor_scan_from_gte ||
               mode == async_read_cursor_scan_from_gt ||
               mode == async_read_cursor_scan_from_upperbound ||
               mode == async_read_cursor_scan_from_nodup) {
      REQUIRE(operation_result == MDBX_EIO,
              "faulted async cursor scan did not propagate read-completion failure");
      REQUIRE(loop_read_probe.results == 0,
              "faulted async cursor scan invoked predicate");
    } else {
      REQUIRE(false, "unhandled async read fault mode");
    }
  } else if (notfound_read) {
    REQUIRE(operation_result == MDBX_NOTFOUND, "cold async missing read returned wrong result");
    if (mode == async_read_cache_get_notfound)
      REQUIRE(cache_result.errcode == MDBX_NOTFOUND && cache_result.status == MDBX_CACHE_UNABLE,
              "cold async cache missing read returned wrong cache result");
    CHECK(expect_empty_value(&data, __FILE__, __LINE__));
  } else if (to_key_equal_miss_read) {
    REQUIRE(operation_result == MDBX_NOTFOUND,
            "cold cursor get TO_KEY_EQUAL miss returned wrong result");
    REQUIRE(read_key_value->iov_len == sizeof(batch_keys[0]),
            "cold cursor get TO_KEY_EQUAL miss returned wrong key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
    REQUIRE(actual_key == batch_keys[0],
            "cold cursor get TO_KEY_EQUAL miss returned wrong key");
    REQUIRE(data.iov_len == sizeof(batch_values[0]),
            "cold cursor get TO_KEY_EQUAL miss returned wrong value size");
    REQUIRE(memcmp(data.iov_base, batch_values[0], sizeof(batch_values[0])) == 0,
            "cold cursor get TO_KEY_EQUAL miss returned wrong value");
  } else if (cursor_from_setkey_miss_read) {
    const int expected_result =
        mode == async_read_cursor_scan_from_setkey_miss ? MDBX_NOTFOUND : MDBX_RESULT_TRUE;
    REQUIRE(operation_result == expected_result,
            "cold cursor stream SET_KEY miss returned wrong result");
    REQUIRE(cursor_stream_completed == 0,
            "cold cursor stream SET_KEY miss completed items");
    REQUIRE(loop_read_probe.results == 0,
            "cold cursor stream SET_KEY miss invoked callback");
    REQUIRE(cursor_from_key.iov_len == sizeof(missing_batch_key_bytes),
            "cold cursor stream SET_KEY miss returned wrong key size");
    REQUIRE(memcmp(cursor_from_key.iov_base, missing_batch_key_bytes,
                   sizeof(missing_batch_key_bytes)) == 0,
            "cold cursor stream SET_KEY miss changed key");
    CHECK(expect_empty_value(&cursor_from_data, __FILE__, __LINE__));
  } else if (cursor_from_equal_miss_read) {
    const int expected_result =
        mode == async_read_cursor_scan_from_equal_miss ? MDBX_NOTFOUND : MDBX_RESULT_TRUE;
    REQUIRE(operation_result == expected_result,
            "cold cursor stream TO_KEY_EQUAL miss returned wrong result");
    REQUIRE(cursor_stream_completed == 0,
            "cold cursor stream TO_KEY_EQUAL miss completed items");
    REQUIRE(loop_read_probe.results == 0,
            "cold cursor stream TO_KEY_EQUAL miss invoked callback");
    REQUIRE(cursor_from_key.iov_len == sizeof(batch_keys[0]),
            "cold cursor stream TO_KEY_EQUAL miss returned wrong key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, cursor_from_key.iov_base, sizeof(actual_key));
    REQUIRE(actual_key == batch_keys[0],
            "cold cursor stream TO_KEY_EQUAL miss returned wrong key");
    REQUIRE(cursor_from_data.iov_len == sizeof(batch_values[0]),
            "cold cursor stream TO_KEY_EQUAL miss returned wrong value size");
    REQUIRE(memcmp(cursor_from_data.iov_base, batch_values[0], sizeof(batch_values[0])) == 0,
            "cold cursor stream TO_KEY_EQUAL miss returned wrong value");
  } else {
    REQUIRE(operation_result == MDBX_SUCCESS ||
                (cursor_scan_read && operation_result == MDBX_RESULT_TRUE),
            "cold async read returned wrong result");
    if (cache_single_read) {
      REQUIRE(cache_result.errcode == MDBX_SUCCESS && cache_result.status == MDBX_CACHE_REFRESHED,
              "cold single async cache read returned wrong cache result");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_sync_get) {
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_sync_get_ex) {
      REQUIRE(values_count == 1, "cold sync get_ex returned wrong value count");
      REQUIRE(read_key_value->iov_len == sizeof(key), "cold sync get_ex returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold sync get_ex returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_sync_lowerbound) {
      REQUIRE(read_key_value->iov_len == sizeof(key), "cold sync lowerbound returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold sync lowerbound returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_sync_cursor_get ||
               mode == async_read_sync_cursor_get_set ||
               mode == async_read_sync_cursor_get_range) {
      REQUIRE(read_key_value->iov_len == sizeof(key),
              "cold sync cursor get returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold sync cursor get returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_sync_cursor_get_upperbound ||
               mode == async_read_sync_cursor_get_to_key_equal ||
               mode == async_read_sync_cursor_get_to_key_gte ||
               mode == async_read_sync_cursor_get_to_key_gt) {
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[forward_seek_expected_index]),
              "cold sync cursor get forward seek returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[forward_seek_expected_index],
              "cold sync cursor get forward seek returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[forward_seek_expected_index]),
              "cold sync cursor get forward seek returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[forward_seek_expected_index],
                     sizeof(batch_values[forward_seek_expected_index])) == 0,
              "cold sync cursor get forward seek returned wrong value");
    } else if (mode == async_read_sync_cursor_get_prev_positioned) {
      const unsigned expected_index = ASYNC_READ_BATCH_COUNT - 2;
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[expected_index]),
              "cold sync cursor get positioned PREV returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[expected_index],
              "cold sync cursor get positioned PREV returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[expected_index]),
              "cold sync cursor get positioned PREV returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[expected_index],
                     sizeof(batch_values[expected_index])) == 0,
              "cold sync cursor get positioned PREV returned wrong value");
    } else if (mode == async_read_sync_cursor_get_last ||
               mode == async_read_sync_cursor_get_prev) {
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[ASYNC_READ_BATCH_COUNT - 1]),
              "cold sync cursor get LAST/PREV returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[ASYNC_READ_BATCH_COUNT - 1],
              "cold sync cursor get LAST/PREV returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1]),
              "cold sync cursor get LAST/PREV returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[ASYNC_READ_BATCH_COUNT - 1],
                     sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1])) == 0,
              "cold sync cursor get LAST/PREV returned wrong value");
    } else if (mode == async_read_sync_cursor_get_nodup) {
      REQUIRE(cursor_from_key.iov_len == sizeof(batch_keys[1]),
              "cold sync cursor get NEXT_NODUP returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, cursor_from_key.iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[1],
              "cold sync cursor get NEXT_NODUP returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[1]),
              "cold sync cursor get NEXT_NODUP returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[1], sizeof(batch_values[1])) == 0,
              "cold sync cursor get NEXT_NODUP returned wrong value");
    } else if (mode == async_read_large_get ||
               mode == async_read_large_lowerbound) {
      CHECK(expect_large_value(&data, key, __FILE__, __LINE__));
    } else if (mode == async_read_large_get_ex) {
      REQUIRE(values_count == 1,
              "cold large async get_ex returned wrong value count");
      REQUIRE(read_key_value->iov_len == sizeof(key),
              "cold large async get_ex returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold large async get_ex returned wrong key");
      CHECK(expect_large_value(&data, key, __FILE__, __LINE__));
    } else if (mode == async_read_large_get_many) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(many_operation_results[i] == MDBX_SUCCESS,
                "cold large async get many returned wrong item result");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_get_batch) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_SUCCESS,
                "cold large async get batch returned wrong item result");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_get_ex_many) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(many_operation_results[i] == MDBX_SUCCESS,
                "cold large async get_ex many returned wrong item result");
        REQUIRE(batch_values_counts[i] == 1,
                "cold large async get_ex many returned wrong value count");
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold large async get_ex many returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i],
                "cold large async get_ex many returned wrong key");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_get_ex_batch) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_SUCCESS,
                "cold large async get_ex batch returned wrong item result");
        REQUIRE(batch_values_counts[i] == 1,
                "cold large async get_ex batch returned wrong value count");
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold large async get_ex batch returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i],
                "cold large async get_ex batch returned wrong key");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_cursor_batch) {
      REQUIRE(cursor_batch_result == MDBX_SUCCESS ||
                  cursor_batch_result == MDBX_RESULT_TRUE,
              "cold large async cursor batch returned wrong result");
      REQUIRE(cursor_batch_count == 2,
              "cold large async cursor batch returned wrong pair count");
      REQUIRE(cursor_batch_pairs[0].iov_len == sizeof(key),
              "cold large async cursor batch returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, cursor_batch_pairs[0].iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key,
              "cold large async cursor batch returned wrong key");
      CHECK(expect_large_value(&cursor_batch_pairs[1], key, __FILE__, __LINE__));
    } else if (large_cursor_stream_read) {
      REQUIRE(large_cursor_probe.results == LARGE_ITEM_COUNT,
              "cold large async cursor stream saw wrong result count");
      if (mode == async_read_large_cursor_loop ||
          mode == async_read_large_cursor_batches) {
        REQUIRE(cursor_stream_completed == LARGE_ITEM_COUNT,
                "cold large async cursor stream completed wrong count");
      }
      if (mode == async_read_large_cursor_batches)
        REQUIRE(large_cursor_probe.batches > 1,
                "cold large async cursor batches did not split callbacks");
    } else if (mode == async_read_large_cache_get ||
               mode == async_read_large_cache_get_singlethreaded) {
      REQUIRE(cache_result.errcode == MDBX_SUCCESS && cache_result.status != MDBX_CACHE_ERROR,
              "cold large async cache read returned wrong cache result");
      CHECK(expect_large_value(&data, key, __FILE__, __LINE__));
    } else if (mode == async_read_abort_order) {
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_abort_order_batch) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        REQUIRE(abort_get_batch_results[i] == MDBX_SUCCESS,
                "queued get batch before abort returned wrong item result");
        REQUIRE(abort_get_batch_data[i].iov_len == sizeof(batch_values[i]),
                "queued get batch before abort returned wrong value size");
        REQUIRE(memcmp(abort_get_batch_data[i].iov_base, batch_values[i],
                       sizeof(batch_values[i])) == 0,
                "queued get batch before abort returned wrong value");
        REQUIRE(abort_get_ex_batch_results[i] == MDBX_SUCCESS,
                "queued get_ex batch before abort returned wrong item result");
        REQUIRE(abort_get_ex_batch_counts[i] == 1,
                "queued get_ex batch before abort returned wrong value count");
        REQUIRE(abort_get_ex_batch_data[i].iov_len == sizeof(batch_values[i]),
                "queued get_ex batch before abort returned wrong value size");
        REQUIRE(memcmp(abort_get_ex_batch_data[i].iov_base, batch_values[i],
                       sizeof(batch_values[i])) == 0,
                "queued get_ex batch before abort returned wrong value");
        REQUIRE(abort_lower_batch_results[i] == MDBX_SUCCESS,
                "queued lowerbound batch before abort returned wrong item result");
        REQUIRE(abort_lower_batch_data[i].iov_len == sizeof(batch_values[i]),
                "queued lowerbound batch before abort returned wrong value size");
        REQUIRE(memcmp(abort_lower_batch_data[i].iov_base, batch_values[i],
                       sizeof(batch_values[i])) == 0,
                "queued lowerbound batch before abort returned wrong value");
        REQUIRE(abort_cache_results[i].errcode == MDBX_SUCCESS &&
                    abort_cache_results[i].status == MDBX_CACHE_REFRESHED,
                "queued cache batch before abort returned wrong item result");
        REQUIRE(abort_cache_data[i].iov_len == sizeof(batch_values[i]),
                "queued cache batch before abort returned wrong value size");
        REQUIRE(memcmp(abort_cache_data[i].iov_base, batch_values[i],
                       sizeof(batch_values[i])) == 0,
                "queued cache batch before abort returned wrong value");
      }
      REQUIRE(abort_cursor_count == 4,
              "queued cursor batch before abort returned wrong item count");
      for (unsigned i = 0; i < 2; ++i) {
        MDBX_val *const batch_key = &abort_cursor_pairs[i * 2];
        MDBX_val *const batch_data_value = &abort_cursor_pairs[i * 2 + 1];
        REQUIRE(batch_key->iov_len == sizeof(batch_keys[i]),
                "queued cursor batch before abort returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key->iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i],
                "queued cursor batch before abort returned wrong key");
        REQUIRE(batch_data_value->iov_len == sizeof(batch_values[i]),
                "queued cursor batch before abort returned wrong value size");
        REQUIRE(memcmp(batch_data_value->iov_base, batch_values[i],
                       sizeof(batch_values[i])) == 0,
                "queued cursor batch before abort returned wrong value");
      }
    } else if (mode == async_read_abort_order_loop) {
      REQUIRE(abort_get_loop_completed == ASYNC_READ_BATCH_COUNT &&
                  abort_get_loop_probe.keys == ASYNC_READ_BATCH_COUNT &&
                  abort_get_loop_probe.results == ASYNC_READ_BATCH_COUNT,
              "queued get loop before abort did not complete");
      REQUIRE(abort_get_ex_loop_completed == ASYNC_READ_BATCH_COUNT &&
                  abort_get_ex_loop_probe.keys == ASYNC_READ_BATCH_COUNT &&
                  abort_get_ex_loop_probe.results == ASYNC_READ_BATCH_COUNT,
              "queued get_ex loop before abort did not complete");
      REQUIRE(abort_lower_loop_completed == ASYNC_READ_BATCH_COUNT &&
                  abort_lower_loop_probe.keys == ASYNC_READ_BATCH_COUNT &&
                  abort_lower_loop_probe.results == ASYNC_READ_BATCH_COUNT,
              "queued lowerbound loop before abort did not complete");
      REQUIRE(abort_cache_loop_completed == ASYNC_READ_BATCH_COUNT &&
                  abort_cache_loop_probe.keys == ASYNC_READ_BATCH_COUNT &&
                  abort_cache_loop_probe.results == ASYNC_READ_BATCH_COUNT,
              "queued cache loop before abort did not complete");
      REQUIRE(abort_cursor_loop_completed == ASYNC_READ_BATCH_COUNT &&
                  abort_cursor_loop_probe.results == ASYNC_READ_BATCH_COUNT,
              "queued cursor loop before abort did not complete");
    } else if (mode == async_read_get_ex) {
      REQUIRE(values_count == 1, "cold async get_ex returned wrong value count");
      REQUIRE(read_key_value->iov_len == sizeof(key), "cold async get_ex returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold async get_ex returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_lowerbound) {
      REQUIRE(read_key_value->iov_len == sizeof(key), "cold async lowerbound returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold async lowerbound returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_cursor_get ||
               mode == async_read_cursor_get_set ||
               mode == async_read_cursor_get_range) {
      REQUIRE(read_key_value->iov_len == sizeof(key),
              "cold async cursor get returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == key, "cold async cursor get returned wrong key");
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    } else if (mode == async_read_cursor_get_upperbound ||
               mode == async_read_cursor_get_to_key_equal ||
               mode == async_read_cursor_get_to_key_gte ||
               mode == async_read_cursor_get_to_key_gt) {
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[forward_seek_expected_index]),
              "cold async cursor get forward seek returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[forward_seek_expected_index],
              "cold async cursor get forward seek returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[forward_seek_expected_index]),
              "cold async cursor get forward seek returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[forward_seek_expected_index],
                     sizeof(batch_values[forward_seek_expected_index])) == 0,
              "cold async cursor get forward seek returned wrong value");
    } else if (mode == async_read_cursor_get_prev_positioned) {
      const unsigned expected_index = ASYNC_READ_BATCH_COUNT - 2;
      REQUIRE(cursor_prime_result == MDBX_SUCCESS,
              "cold async cursor get positioned PREV prime returned wrong result");
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[expected_index]),
              "cold async cursor get positioned PREV returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[expected_index],
              "cold async cursor get positioned PREV returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[expected_index]),
              "cold async cursor get positioned PREV returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[expected_index],
                     sizeof(batch_values[expected_index])) == 0,
              "cold async cursor get positioned PREV returned wrong value");
    } else if (mode == async_read_cursor_get_last ||
               mode == async_read_cursor_get_prev) {
      REQUIRE(read_key_value->iov_len == sizeof(batch_keys[ASYNC_READ_BATCH_COUNT - 1]),
              "cold async cursor get LAST/PREV returned wrong key size");
      uint64_t actual_key = UINT64_MAX;
      memcpy(&actual_key, read_key_value->iov_base, sizeof(actual_key));
      REQUIRE(actual_key == batch_keys[ASYNC_READ_BATCH_COUNT - 1],
              "cold async cursor get LAST/PREV returned wrong key");
      REQUIRE(data.iov_len == sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1]),
              "cold async cursor get LAST/PREV returned wrong value size");
      REQUIRE(memcmp(data.iov_base, batch_values[ASYNC_READ_BATCH_COUNT - 1],
                     sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1])) == 0,
              "cold async cursor get LAST/PREV returned wrong value");
    } else if (mode == async_read_cursor_get_batch ||
               mode == async_read_cursor_get_batch_nodup ||
               mode == async_read_sync_cursor_get_batch ||
               mode == async_read_sync_cursor_get_batch_nodup) {
      REQUIRE(cursor_batch_result == MDBX_SUCCESS || cursor_batch_result == MDBX_RESULT_TRUE,
              "cold cursor get batch returned wrong result");
      if (mode == async_read_cursor_get_batch_nodup ||
          mode == async_read_sync_cursor_get_batch_nodup) {
        REQUIRE(cursor_prime_result == MDBX_SUCCESS || cursor_prime_result == MDBX_RESULT_TRUE,
                "cold cursor get batch NEXT_NODUP prime returned wrong result");
        REQUIRE(cursor_prime_count == 4,
                "cold cursor get batch NEXT_NODUP prime returned wrong item count");
      }
      REQUIRE(cursor_batch_count == (ASYNC_READ_BATCH_COUNT - cursor_batch_start_index) * 2,
              "cold cursor get batch returned wrong item count");
      for (unsigned i = cursor_batch_start_index; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const unsigned pair_index = i - cursor_batch_start_index;
        MDBX_val *const batch_key = &cursor_batch_pairs[pair_index * 2];
        MDBX_val *const batch_data_value = &cursor_batch_pairs[pair_index * 2 + 1];
        REQUIRE(batch_key->iov_len == sizeof(batch_keys[i]),
                "cold cursor get batch returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key->iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i], "cold cursor get batch returned wrong key");
        REQUIRE(batch_data_value->iov_len == sizeof(batch_values[i]),
                "cold cursor get batch returned wrong value size");
        REQUIRE(memcmp(batch_data_value->iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold cursor get batch returned wrong value");
      }
    } else if (mode == async_read_cursor_get_loop ||
               mode == async_read_cursor_get_loop_prev ||
               mode == async_read_cursor_get_loop_current ||
               mode == async_read_cursor_get_loop_current_prev ||
               mode == async_read_cursor_get_loop_nodup ||
               mode == async_read_cursor_get_loop_from ||
               mode == async_read_cursor_get_loop_from_prev ||
               mode == async_read_cursor_get_loop_from_setkey ||
               mode == async_read_cursor_get_loop_from_equal ||
               mode == async_read_cursor_get_loop_from_gte ||
               mode == async_read_cursor_get_loop_from_gt ||
               mode == async_read_cursor_get_loop_from_upperbound ||
               mode == async_read_cursor_get_loop_from_nodup) {
      REQUIRE(cursor_stream_completed == cursor_stream_target,
              "cold async cursor get loop completed wrong count");
      REQUIRE(loop_read_probe.results == cursor_stream_target,
              "cold async cursor get loop saw wrong result count");
      if (mode == async_read_cursor_get_loop_from ||
          mode == async_read_cursor_get_loop_from_prev ||
          mode == async_read_cursor_get_loop_from_setkey ||
          mode == async_read_cursor_get_loop_from_equal ||
          mode == async_read_cursor_get_loop_from_gte ||
          mode == async_read_cursor_get_loop_from_gt ||
          mode == async_read_cursor_get_loop_from_upperbound ||
          mode == async_read_cursor_get_loop_from_nodup) {
        const size_t expected_final_index =
            mode == async_read_cursor_get_loop_from_prev ? 0 : ASYNC_READ_BATCH_COUNT - 1;
        REQUIRE(cursor_from_key.iov_len == sizeof(batch_keys[expected_final_index]),
                "cold async cursor get loop-from returned wrong key size");
        REQUIRE(cursor_from_data.iov_len == sizeof(batch_values[expected_final_index]),
                "cold async cursor get loop-from returned wrong value size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, cursor_from_key.iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[expected_final_index],
                "cold async cursor get loop-from returned wrong key");
        REQUIRE(memcmp(cursor_from_data.iov_base, batch_values[expected_final_index],
                       sizeof(batch_values[expected_final_index])) == 0,
                "cold async cursor get loop-from returned wrong value");
      }
    } else if (mode == async_read_cursor_get_batches ||
               mode == async_read_cursor_get_batches_current ||
               mode == async_read_cursor_get_batches_from ||
               mode == async_read_cursor_get_batches_from_setkey ||
               mode == async_read_cursor_get_batches_from_equal ||
               mode == async_read_cursor_get_batches_from_gte ||
               mode == async_read_cursor_get_batches_from_gt ||
               mode == async_read_cursor_get_batches_from_upperbound) {
      REQUIRE(cursor_stream_completed == cursor_stream_target,
              "cold async cursor batches completed wrong count");
      REQUIRE(loop_read_probe.results == cursor_stream_target,
              "cold async cursor batches saw wrong result count");
      REQUIRE(loop_read_probe.keys > 1, "cold async cursor batches did not invoke multiple callbacks");
      if (mode == async_read_cursor_get_batches_current ||
          mode == async_read_cursor_get_batches_from ||
          mode == async_read_cursor_get_batches_from_setkey ||
          mode == async_read_cursor_get_batches_from_equal ||
          mode == async_read_cursor_get_batches_from_gte ||
          mode == async_read_cursor_get_batches_from_gt ||
          mode == async_read_cursor_get_batches_from_upperbound) {
        REQUIRE(cursor_from_key.iov_len == sizeof(batch_keys[ASYNC_READ_BATCH_COUNT - 1]),
                "cold async cursor batches-from returned wrong key size");
        REQUIRE(cursor_from_data.iov_len == sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1]),
                "cold async cursor batches-from returned wrong value size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, cursor_from_key.iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[ASYNC_READ_BATCH_COUNT - 1],
                "cold async cursor batches-from returned wrong key");
        REQUIRE(memcmp(cursor_from_data.iov_base, batch_values[ASYNC_READ_BATCH_COUNT - 1],
                       sizeof(batch_values[ASYNC_READ_BATCH_COUNT - 1])) == 0,
                "cold async cursor batches-from returned wrong value");
      }
    } else if (mode == async_read_cursor_scan ||
               mode == async_read_cursor_scan_current ||
               mode == async_read_cursor_scan_current_prev ||
               mode == async_read_cursor_scan_prev ||
               mode == async_read_cursor_scan_nodup ||
               mode == async_read_cursor_scan_from ||
               mode == async_read_cursor_scan_from_prev ||
               mode == async_read_cursor_scan_from_setkey ||
               mode == async_read_cursor_scan_from_equal ||
               mode == async_read_cursor_scan_from_gte ||
               mode == async_read_cursor_scan_from_gt ||
               mode == async_read_cursor_scan_from_upperbound ||
               mode == async_read_cursor_scan_from_nodup ||
               mode == async_read_sync_cursor_scan ||
               mode == async_read_sync_cursor_scan_current ||
               mode == async_read_sync_cursor_scan_current_prev ||
               mode == async_read_sync_cursor_scan_prev ||
               mode == async_read_sync_cursor_scan_nodup ||
               mode == async_read_sync_cursor_scan_from ||
               mode == async_read_sync_cursor_scan_from_prev ||
               mode == async_read_sync_cursor_scan_from_nodup) {
      REQUIRE(loop_read_probe.results == cursor_stream_target,
              "cold cursor scan saw wrong result count");
      if (mode == async_read_cursor_scan_from ||
          mode == async_read_cursor_scan_from_prev ||
          mode == async_read_cursor_scan_from_setkey ||
          mode == async_read_cursor_scan_from_equal ||
          mode == async_read_cursor_scan_from_gte ||
          mode == async_read_cursor_scan_from_gt ||
          mode == async_read_cursor_scan_from_upperbound ||
          mode == async_read_cursor_scan_from_nodup ||
          mode == async_read_sync_cursor_scan_from ||
          mode == async_read_sync_cursor_scan_from_prev ||
          mode == async_read_sync_cursor_scan_from_nodup) {
        const size_t expected_final_index =
            mode == async_read_cursor_scan_from_prev ||
                    mode == async_read_sync_cursor_scan_from_prev
                ? 0
                : ASYNC_READ_BATCH_COUNT - 1;
        REQUIRE(cursor_from_key.iov_len == sizeof(batch_keys[expected_final_index]),
                "cold cursor scan-from returned wrong key size");
        REQUIRE(cursor_from_data.iov_len == sizeof(batch_values[expected_final_index]),
                "cold cursor scan-from returned wrong value size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, cursor_from_key.iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[expected_final_index],
                "cold cursor scan-from returned wrong key");
        REQUIRE(memcmp(cursor_from_data.iov_base, batch_values[expected_final_index],
                       sizeof(batch_values[expected_final_index])) == 0,
                "cold cursor scan-from returned wrong value");
      }
    } else if (mode == async_read_get_many ||
               mode == async_read_get_many_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(many_operation_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async get many returned wrong item result");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async get many returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async get many returned wrong value");
      }
    } else if (mode == async_read_get_batch ||
               mode == async_read_get_batch_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(batch_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async get batch returned wrong item result");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]), "cold async get batch returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async get batch returned wrong value");
      }
    } else if (mode == async_read_get_ex_many ||
               mode == async_read_get_ex_many_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(many_operation_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async get_ex many returned wrong item result");
        REQUIRE(batch_values_counts[i] == (missing ? 0 : 1),
                "cold async get_ex many returned wrong value count");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold async get_ex many returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i], "cold async get_ex many returned wrong key");
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async get_ex many returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async get_ex many returned wrong value");
      }
    } else if (mode == async_read_get_ex_batch ||
               mode == async_read_get_ex_batch_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(batch_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async get_ex batch returned wrong item result");
        REQUIRE(batch_values_counts[i] == (missing ? 0 : 1),
                "cold async get_ex batch returned wrong value count");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold async get_ex batch returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i], "cold async get_ex batch returned wrong key");
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async get_ex batch returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async get_ex batch returned wrong value");
      }
    } else if (mode == async_read_lowerbound_many ||
               mode == async_read_lowerbound_many_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(many_operation_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async lowerbound many returned wrong item result");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold async lowerbound many returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i], "cold async lowerbound many returned wrong key");
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async lowerbound many returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async lowerbound many returned wrong value");
      }
    } else if (mode == async_read_large_lowerbound_many) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(many_operation_results[i] == MDBX_SUCCESS,
                "cold large async lowerbound many returned wrong item result");
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold large async lowerbound many returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i],
                "cold large async lowerbound many returned wrong key");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_lowerbound_batch ||
               mode == async_read_lowerbound_batch_mixed) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        REQUIRE(batch_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async lowerbound batch returned wrong item result");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold async lowerbound batch returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i], "cold async lowerbound batch returned wrong key");
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async lowerbound batch returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async lowerbound batch returned wrong value");
      }
    } else if (mode == async_read_large_lowerbound_batch) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        REQUIRE(batch_results[i] == MDBX_SUCCESS,
                "cold large async lowerbound batch returned wrong item result");
        REQUIRE(batch_key_values[i].iov_len == sizeof(batch_keys[i]),
                "cold large async lowerbound batch returned wrong key size");
        uint64_t actual_key = UINT64_MAX;
        memcpy(&actual_key, batch_key_values[i].iov_base, sizeof(actual_key));
        REQUIRE(actual_key == batch_keys[i],
                "cold large async lowerbound batch returned wrong key");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (mode == async_read_large_cache_get_many ||
               mode == async_read_large_cache_get_singlethreaded_many ||
               mode == async_read_large_cache_get_batch ||
               mode == async_read_large_cache_get_singlethreaded_batch) {
      for (unsigned i = 0; i < indexed_read_count; ++i) {
        if (mode == async_read_large_cache_get_many ||
            mode == async_read_large_cache_get_singlethreaded_many)
          REQUIRE(many_operation_results[i] == MDBX_SUCCESS,
                  "cold large async cache get many returned wrong operation result");
        if (batch_cache_results[i].errcode != MDBX_SUCCESS) {
          rc = fail_rc("cold large async cache get batch item",
                       batch_cache_results[i].errcode, __FILE__, __LINE__);
          goto bailout;
        }
        REQUIRE(batch_cache_results[i].status == MDBX_CACHE_REFRESHED,
                "cold large async cache get batch returned wrong cache status");
        CHECK(expect_large_value(&batch_data[i], batch_keys[i], __FILE__, __LINE__));
      }
    } else if (cache_batch_read) {
      for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
        const bool missing = mixed_batch_read && i == 1;
        if (mode == async_read_cache_get_many ||
            mode == async_read_cache_get_singlethreaded_many ||
            mode == async_read_cache_get_many_mixed ||
            mode == async_read_cache_get_singlethreaded_many_mixed)
          REQUIRE(many_operation_results[i] == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                  "cold async cache get many returned wrong operation result");
        REQUIRE(batch_cache_results[i].errcode == (missing ? MDBX_NOTFOUND : MDBX_SUCCESS),
                "cold async cache get batch returned wrong item result");
        REQUIRE((missing && batch_cache_results[i].status != MDBX_CACHE_ERROR) ||
                    (!missing && batch_cache_results[i].status == MDBX_CACHE_REFRESHED),
                "cold async cache get batch returned wrong cache status");
        if (missing) {
          CHECK(expect_empty_value(&batch_data[i], __FILE__, __LINE__));
          continue;
        }
        REQUIRE(batch_data[i].iov_len == sizeof(batch_values[i]),
                "cold async cache get batch returned wrong value size");
        REQUIRE(memcmp(batch_data[i].iov_base, batch_values[i], sizeof(batch_values[i])) == 0,
                "cold async cache get batch returned wrong value");
      }
    } else if (mode == async_read_get_loop ||
               mode == async_read_large_get_loop) {
      REQUIRE(loop_completed == indexed_read_count, "cold async get loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count, "cold async get loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count, "cold async get loop saw wrong result count");
    } else if (cache_loop_read) {
      REQUIRE(loop_completed == indexed_read_count, "cold async cache get loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "cold async cache get loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "cold async cache get loop saw wrong result count");
    } else if (mode == async_read_get_ex_loop ||
               mode == async_read_large_get_ex_loop) {
      REQUIRE(loop_completed == indexed_read_count, "cold async get_ex loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "cold async get_ex loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "cold async get_ex loop saw wrong result count");
    } else if (mode == async_read_lowerbound_loop) {
      REQUIRE(loop_completed == ASYNC_READ_BATCH_COUNT, "cold async lowerbound loop completed wrong count");
      REQUIRE(loop_read_probe.keys == ASYNC_READ_BATCH_COUNT,
              "cold async lowerbound loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == ASYNC_READ_BATCH_COUNT,
              "cold async lowerbound loop saw wrong result count");
    } else if (mode == async_read_large_lowerbound_loop) {
      REQUIRE(loop_completed == indexed_read_count,
              "cold large async lowerbound loop completed wrong count");
      REQUIRE(loop_read_probe.keys == indexed_read_count,
              "cold large async lowerbound loop prepared wrong key count");
      REQUIRE(loop_read_probe.results == indexed_read_count,
              "cold large async lowerbound loop saw wrong result count");
    } else {
      CHECK(expect_payload(&data, payload, __FILE__, __LINE__));
    }
  }

  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), false));
  if (cache_hit_read) {
    REQUIRE(read_stats.storage_read_items == 0, "warm async cache-hit read submitted storage reads");
    REQUIRE(read_stats.storage_read_batches == 0, "warm async cache-hit read started storage read batches");
    REQUIRE(read_stats.storage_read_completed == 0, "warm async cache-hit read completed unexpected reads");
    REQUIRE(read_stats.storage_read_errors == 0, "warm async cache-hit read reported read errors");
    REQUIRE(read_stats.iouring_read_items == 0, "warm async cache-hit read used io_uring reads");
    REQUIRE(read_stats.iouring_read_batches == 0, "warm async cache-hit read reported io_uring batches");
    REQUIRE(read_stats.pending_polls == 0, "warm async cache-hit read left reads pending");
    REQUIRE(read_stats.page_cache_hits > 0, "warm async cache-hit read did not report page-cache hits");
    REQUIRE(read_stats.page_cache_misses == 0, "warm async cache-hit read reported page-cache misses");
    REQUIRE(read_stats.page_cache_fills == 0, "warm async cache-hit read filled page-cache entries");
  } else {
    REQUIRE(read_stats.storage_read_items > 0, "cold single async read did not submit storage reads");
    REQUIRE(read_stats.storage_read_batches > 0, "cold single async read did not start storage read batches");
    REQUIRE(read_stats.storage_read_completed >= read_stats.storage_read_items,
            "cold single async read did not complete submitted reads");
    if (inject_fault)
      REQUIRE(read_stats.storage_read_errors > 0, "faulted async get did not report read errors");
    else
      REQUIRE(read_stats.storage_read_errors == 0, "successful cold single async read reported read errors");
    REQUIRE(read_stats.page_cache_misses > 0, "cold single async read did not report page-cache misses");
    if (batch_read && !inject_fault) {
      REQUIRE(read_stats.storage_read_items > 1, "cold async batch read did not submit multiple storage reads");
      if (indexed_batch_read)
        REQUIRE(read_stats.storage_read_max_batch > 1, "cold async batch read did not report multi-read batches");
    }
    if (env_enabled("MDBX_ASYNC_SMOKE_EXPECT_IOURING")) {
      REQUIRE(read_stats.iouring_read_items > 0, "cold single async read did not use io_uring reads");
      REQUIRE(read_stats.iouring_read_batches > 0, "cold single async read did not report io_uring read batches");
      if (batch_read && !inject_fault) {
        REQUIRE(read_stats.iouring_read_items > 1, "cold async batch read did not submit multiple io_uring reads");
        if (indexed_batch_read) {
          REQUIRE(read_stats.iouring_read_max_batch > 1, "cold async batch read did not report io_uring batch depth");
          REQUIRE(read_stats.iouring_read_max_inflight > 1, "cold async batch read did not report overlapped reads");
        }
      }
    }
    if (env_enabled("MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING")) {
      REQUIRE(read_stats.iouring_read_items == 0, "fallback cold single async read unexpectedly used io_uring reads");
      REQUIRE(read_stats.iouring_read_batches == 0, "fallback cold single async read reported io_uring batches");
      REQUIRE(read_stats.pending_polls == 0, "fallback cold single async read left reads pending");
    }
  }

  if (cursor) {
    if (cursor_is_async) {
      CHECK(mdbx_async_cursor_close(async, cursor, &op));
      CHECK_OP(op);
    } else {
      mdbx_cursor_close(cursor);
    }
    cursor = NULL;
  }
  if (txn) {
    if (sync_read) {
      CHECK(mdbx_txn_abort(txn));
    } else {
      CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
      CHECK_OP(op);
    }
    txn = NULL;
  }
  CHECK(mdbx_async_env_close_ex(async, false, &op));
  CHECK(wait_result("mdbx_async_env_close_ex read fault", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "unexpected async env close result after read fault");
  env = NULL;
  CHECK(mdbx_async_env_delete(async, path, MDBX_ENV_JUST_DELETE, &op));
  CHECK(wait_result("mdbx_async_env_delete read fault", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS || operation_result == MDBX_RESULT_TRUE,
          "unexpected async env delete result after read fault");
  CHECK(mdbx_async_destroy(async, true));
  return MDBX_SUCCESS;

bailout:
  (void)unset_env_var("MDBX_TEST_DXB_FAULT");
  if (abort_probe_prepared) {
    (void)async_block_probe_release(&abort_probe);
    async_block_probe_close(&abort_probe);
  }
  for (unsigned i = 0; i < 7; ++i) {
    if (abort_ops[i]) {
      int ignored = MDBX_SUCCESS;
      (void)mdbx_async_wait(abort_ops[i], &ignored);
      (void)mdbx_async_op_release(abort_ops[i]);
      abort_ops[i] = NULL;
    }
  }
  for (unsigned i = 0; i < ASYNC_READ_BATCH_COUNT; ++i) {
    if (many_ops[i]) {
      int ignored = MDBX_SUCCESS;
      (void)mdbx_async_wait(many_ops[i], &ignored);
      (void)mdbx_async_op_release(many_ops[i]);
      many_ops[i] = NULL;
    }
  }
  if (cursor) {
    if (cursor_is_async && async) {
      MDBX_async_op *cleanup_op = NULL;
      int ignored = MDBX_SUCCESS;
      if (mdbx_async_cursor_close(async, cursor, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
        (void)mdbx_async_wait(cleanup_op, &ignored);
        (void)mdbx_async_op_release(cleanup_op);
      }
    } else {
      (void)mdbx_cursor_close(cursor);
    }
    cursor = NULL;
  }
  if (txn) {
    if (sync_read) {
      (void)mdbx_txn_abort(txn);
    } else if (async) {
      MDBX_async_op *cleanup_op = NULL;
      int ignored = MDBX_SUCCESS;
      if (mdbx_async_txn_abort(async, txn, NULL, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
        (void)mdbx_async_wait(cleanup_op, &ignored);
        (void)mdbx_async_op_release(cleanup_op);
      }
    } else {
      (void)mdbx_txn_abort(txn);
    }
    txn = NULL;
  }
  if (async) {
    if (env) {
      MDBX_async_op *cleanup_op = NULL;
      int ignored = MDBX_SUCCESS;
      if (mdbx_async_env_close_ex(async, true, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
        (void)mdbx_async_wait(cleanup_op, &ignored);
        (void)mdbx_async_op_release(cleanup_op);
      }
      env = NULL;
    }
    (void)mdbx_async_destroy(async, true);
  }
  (void)mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  return rc ? rc : MDBX_PROBLEM;
}

static int exercise_async_dupsort_read_fallback(const char *path) {
  MDBX_async *async = NULL;
  MDBX_env *env = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  MDBX_dbi dbi = 0;
  int rc = MDBX_SUCCESS;
  int operation_result = MDBX_SUCCESS;
  MDBX_async_read_stats read_stats;

  const uint64_t dup_key = 7;
  const uint64_t key0 = 9;
  const uint64_t key1 = 11;
  const uint64_t dup0 = expected_value(dup_key);
  const uint64_t dup1 = expected_value(dup_key) + 1;
  const uint64_t value0 = expected_value(key0);
  const uint64_t value1 = expected_value(key1);
  MDBX_val dup_key_value = val((void *)&dup_key, sizeof(dup_key));
  MDBX_val key0_value = val((void *)&key0, sizeof(key0));
  MDBX_val key1_value = val((void *)&key1, sizeof(key1));
  MDBX_val dup0_value = val((void *)&dup0, sizeof(dup0));
  MDBX_val dup1_value = val((void *)&dup1, sizeof(dup1));
  MDBX_val value0_value = val((void *)&value0, sizeof(value0));
  MDBX_val value1_value = val((void *)&value1, sizeof(value1));
  uint8_t missing_key_bytes[sizeof(uint64_t)];
  MDBX_val missing_key = val(missing_key_bytes, sizeof(missing_key_bytes));
  MDBX_val data = val(NULL, 0);
  size_t values_count = SIZE_MAX;
  MDBX_async_op *ops[3] = {NULL, NULL, NULL};
  int op_results[3] = {MDBX_PROBLEM, MDBX_PROBLEM, MDBX_PROBLEM};
  MDBX_val keys[3] = {dup_key_value, missing_key, key1_value};
  MDBX_val values[3] = {val(NULL, 0), val(NULL, 0), val(NULL, 0)};
  size_t counts[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  MDBX_cache_entry_t cache_entry;
  MDBX_cache_entry_t cache_st_entry;
  MDBX_cache_result_t cache_result = {MDBX_PROBLEM, MDBX_CACHE_ERROR};
  MDBX_cache_result_t cache_st_result = {MDBX_PROBLEM, MDBX_CACHE_ERROR};
  MDBX_async_op *cache_ops[2] = {NULL, NULL};
  int cache_op_results[2] = {MDBX_PROBLEM, MDBX_PROBLEM};
  MDBX_val cache_keys[2] = {dup_key_value, missing_key};
  MDBX_val cache_data[2] = {val(NULL, 0), val(NULL, 0)};
  MDBX_cache_entry_t cache_entries[2];
  MDBX_cache_result_t cache_results[2];
  MDBX_val cursor_pairs[8];
  size_t completed = 0;
  struct async_dupsort_loop_probe loop_probe = {{dup_key, UINT64_MAX, key1},
                                                {dup0, 0, value1},
                                                {MDBX_SUCCESS, MDBX_NOTFOUND, MDBX_SUCCESS},
                                                0,
                                                0};
  struct async_dupsort_cache_loop_probe cache_loop_probe = {{dup_key, UINT64_MAX},
                                                            {MDBX_EMULTIVAL, MDBX_NOTFOUND},
                                                            {MDBX_CACHE_ERROR, MDBX_CACHE_UNABLE},
                                                            0,
                                                            0};
  struct async_dupsort_cursor_probe cursor_probe = {{dup_key, dup_key, key0, key1},
                                                    {dup0, dup1, value0, value1},
                                                    0};

  memset(missing_key_bytes, 0xff, sizeof(missing_key_bytes));
  memset(&read_stats, 0, sizeof(read_stats));
  mdbx_cache_init(&cache_entry);
  mdbx_cache_init(&cache_st_entry);
  for (size_t i = 0; i < 2; ++i) {
    mdbx_cache_init(&cache_entries[i]);
    cache_results[i].errcode = MDBX_PROBLEM;
    cache_results[i].status = MDBX_CACHE_ERROR;
  }

  if (!env_enabled("MDBX_FORCE_NO_DATA_MMAP")) {
    rc = fail_msg("async dupsort fallback smoke requires MDBX_FORCE_NO_DATA_MMAP=1", __FILE__, __LINE__);
    goto bailout;
  }

  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
    rc = fail_rc("mdbx_env_delete", rc, __FILE__, __LINE__);
    goto bailout;
  }

  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_env_create(async, &env, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_set_maxdbs(async, 4, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_begin_ex(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, "async-dupsort-target", MDBX_CREATE | MDBX_DUPSORT, &dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, dbi, &dup_key_value, &dup0_value, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, dbi, &dup_key_value, &dup1_value, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, dbi, &key0_value, &value0_value, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, dbi, &key1_value, &value1_value, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_env_close_ex(async, false, &op));
  CHECK(wait_result("mdbx_async_env_close_ex dupsort populate", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "unexpected async env close after dupsort populate");
  env = NULL;
  CHECK(mdbx_async_destroy(async, true));
  async = NULL;

  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &async));
  CHECK(mdbx_async_env_create(async, &env, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_set_maxdbs(async, 4, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_begin_ex(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, "async-dupsort-target", MDBX_DUPSORT, &dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), true));

  CHECK(mdbx_async_get(async, txn, dbi, &dup_key_value, &data, &op));
  CHECK(wait_result("mdbx_async_get dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "dupsort async get fallback returned wrong result");
  CHECK(expect_payload(&data, dup0, __FILE__, __LINE__));

  data = val(NULL, 0);
  values_count = SIZE_MAX;
  MDBX_val get_ex_key = dup_key_value;
  CHECK(mdbx_async_get_ex(async, txn, dbi, &get_ex_key, &data, &values_count, &op));
  CHECK(wait_result("mdbx_async_get_ex dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "dupsort async get_ex fallback returned wrong result");
  REQUIRE(values_count == 2, "dupsort async get_ex fallback returned wrong duplicate count");
  CHECK(expect_payload(&data, dup0, __FILE__, __LINE__));

  data = val(NULL, 0);
  cache_result.errcode = MDBX_PROBLEM;
  cache_result.status = MDBX_CACHE_ERROR;
  CHECK(mdbx_async_cache_get(async, txn, dbi, &dup_key_value, &data, &cache_entry, &cache_result, &op));
  CHECK(wait_result("mdbx_async_cache_get dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_EMULTIVAL && cache_result.errcode == MDBX_EMULTIVAL &&
              cache_result.status == MDBX_CACHE_ERROR,
          "dupsort async cache_get fallback did not preserve multivalue error");
  CHECK(expect_empty_value(&data, __FILE__, __LINE__));

  data = val(NULL, 0);
  cache_st_result.errcode = MDBX_PROBLEM;
  cache_st_result.status = MDBX_CACHE_ERROR;
  CHECK(mdbx_async_cache_get_SingleThreaded(async, txn, dbi, &dup_key_value, &data, &cache_st_entry,
                                            &cache_st_result, &op));
  CHECK(wait_result("mdbx_async_cache_get_SingleThreaded dupsort fallback", &op, &operation_result,
                    __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_EMULTIVAL && cache_st_result.errcode == MDBX_EMULTIVAL &&
              cache_st_result.status == MDBX_CACHE_ERROR,
          "dupsort async single-threaded cache_get fallback did not preserve multivalue error");
  CHECK(expect_empty_value(&data, __FILE__, __LINE__));

  for (size_t i = 0; i < 2; ++i) {
    mdbx_cache_init(&cache_entries[i]);
    cache_results[i].errcode = MDBX_PROBLEM;
    cache_results[i].status = MDBX_CACHE_ERROR;
    cache_data[i] = val(NULL, 0);
    cache_op_results[i] = MDBX_PROBLEM;
  }
  CHECK(mdbx_async_cache_get_many(async, txn, dbi, cache_keys, cache_data, cache_entries,
                                  cache_results, 2, cache_ops));
  CHECK(wait_many_result("mdbx_async_cache_get_many dupsort fallback", cache_ops, 2,
                         cache_op_results, __FILE__, __LINE__));
  REQUIRE(cache_op_results[0] == MDBX_EMULTIVAL && cache_op_results[1] == MDBX_NOTFOUND,
          "dupsort async cache_get_many fallback returned wrong results");
  REQUIRE(cache_results[0].errcode == MDBX_EMULTIVAL &&
              cache_results[0].status == MDBX_CACHE_ERROR &&
              cache_results[1].errcode == MDBX_NOTFOUND &&
              cache_results[1].status == MDBX_CACHE_UNABLE,
          "dupsort async cache_get_many fallback returned wrong cache results");
  CHECK(expect_empty_value(&cache_data[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&cache_data[1], __FILE__, __LINE__));

  for (size_t i = 0; i < 2; ++i) {
    mdbx_cache_init(&cache_entries[i]);
    cache_results[i].errcode = MDBX_PROBLEM;
    cache_results[i].status = MDBX_CACHE_ERROR;
    cache_data[i] = val(NULL, 0);
    cache_op_results[i] = MDBX_PROBLEM;
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_many(async, txn, dbi, cache_keys, cache_data,
                                                 cache_entries, cache_results, 2, cache_ops));
  CHECK(wait_many_result("mdbx_async_cache_get_SingleThreaded_many dupsort fallback", cache_ops, 2,
                         cache_op_results, __FILE__, __LINE__));
  REQUIRE(cache_op_results[0] == MDBX_EMULTIVAL && cache_op_results[1] == MDBX_NOTFOUND,
          "dupsort async single-threaded cache_get_many fallback returned wrong results");
  REQUIRE(cache_results[0].errcode == MDBX_EMULTIVAL &&
              cache_results[0].status == MDBX_CACHE_ERROR &&
              cache_results[1].errcode == MDBX_NOTFOUND &&
              cache_results[1].status == MDBX_CACHE_UNABLE,
          "dupsort async single-threaded cache_get_many fallback returned wrong cache results");
  CHECK(expect_empty_value(&cache_data[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&cache_data[1], __FILE__, __LINE__));

  for (size_t i = 0; i < 2; ++i) {
    mdbx_cache_init(&cache_entries[i]);
    cache_results[i].errcode = MDBX_PROBLEM;
    cache_results[i].status = MDBX_CACHE_ERROR;
    cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_batch(async, txn, dbi, cache_keys, cache_data, cache_entries,
                                   cache_results, 2, &op));
  CHECK_OP(op);
  REQUIRE(cache_results[0].errcode == MDBX_EMULTIVAL &&
              cache_results[0].status == MDBX_CACHE_ERROR &&
              cache_results[1].errcode == MDBX_NOTFOUND &&
              cache_results[1].status == MDBX_CACHE_UNABLE,
          "dupsort async cache_get_batch fallback returned wrong cache results");
  CHECK(expect_empty_value(&cache_data[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&cache_data[1], __FILE__, __LINE__));

  for (size_t i = 0; i < 2; ++i) {
    mdbx_cache_init(&cache_entries[i]);
    cache_results[i].errcode = MDBX_PROBLEM;
    cache_results[i].status = MDBX_CACHE_ERROR;
    cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_batch(async, txn, dbi, cache_keys, cache_data,
                                                  cache_entries, cache_results, 2, &op));
  CHECK_OP(op);
  REQUIRE(cache_results[0].errcode == MDBX_EMULTIVAL &&
              cache_results[0].status == MDBX_CACHE_ERROR &&
              cache_results[1].errcode == MDBX_NOTFOUND &&
              cache_results[1].status == MDBX_CACHE_UNABLE,
          "dupsort async single-threaded cache_get_batch fallback returned wrong cache results");
  CHECK(expect_empty_value(&cache_data[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&cache_data[1], __FILE__, __LINE__));

  for (size_t i = 0; i < 2; ++i)
    mdbx_cache_init(&cache_entries[i]);
  completed = 0;
  cache_loop_probe.keys_seen = 0;
  cache_loop_probe.results_seen = 0;
  CHECK(mdbx_async_cache_get_loop(async, txn, dbi, 2, async_dupsort_cache_loop_key_func,
                                  cache_entries, async_dupsort_cache_loop_result_func,
                                  &cache_loop_probe, &completed, &op));
  CHECK_OP(op);
  REQUIRE(completed == 2 && cache_loop_probe.keys_seen == 2 &&
              cache_loop_probe.results_seen == 2,
          "dupsort async cache_get_loop fallback did not visit every item");

  for (size_t i = 0; i < 2; ++i)
    mdbx_cache_init(&cache_entries[i]);
  completed = 0;
  cache_loop_probe.keys_seen = 0;
  cache_loop_probe.results_seen = 0;
  CHECK(mdbx_async_cache_get_SingleThreaded_loop(async, txn, dbi, 2,
                                                 async_dupsort_cache_loop_key_func,
                                                 cache_entries,
                                                 async_dupsort_cache_loop_result_func,
                                                 &cache_loop_probe, &completed, &op));
  CHECK_OP(op);
  REQUIRE(completed == 2 && cache_loop_probe.keys_seen == 2 &&
              cache_loop_probe.results_seen == 2,
          "dupsort async single-threaded cache_get_loop fallback did not visit every item");

  CHECK(mdbx_async_get_loop(async, txn, dbi, 3, async_dupsort_loop_key_func,
                            async_dupsort_loop_result_func, &loop_probe, &completed, &op));
  CHECK(wait_result("mdbx_async_get_loop dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "dupsort async get_loop fallback returned wrong result");
  REQUIRE(completed == 3 && loop_probe.keys_seen == 3 && loop_probe.results_seen == 3,
          "dupsort async get_loop fallback did not visit every item");

  completed = 0;
  loop_probe.keys_seen = 0;
  loop_probe.results_seen = 0;
  CHECK(mdbx_async_get_ex_loop(async, txn, dbi, 3, async_dupsort_loop_key_func,
                               async_dupsort_get_ex_loop_result_func, &loop_probe, &completed, &op));
  CHECK(wait_result("mdbx_async_get_ex_loop dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "dupsort async get_ex_loop fallback returned wrong result");
  REQUIRE(completed == 3 && loop_probe.keys_seen == 3 && loop_probe.results_seen == 3,
          "dupsort async get_ex_loop fallback did not visit every item");

  CHECK(mdbx_async_get_many(async, txn, dbi, keys, values, 3, ops));
  CHECK(wait_many_result("mdbx_async_get_many dupsort fallback", ops, 3, op_results, __FILE__, __LINE__));
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "dupsort async get_many fallback returned wrong results");
  CHECK(expect_payload(&values[0], dup0, __FILE__, __LINE__));
  CHECK(expect_empty_value(&values[1], __FILE__, __LINE__));
  CHECK(expect_payload(&values[2], value1, __FILE__, __LINE__));

  keys[0] = dup_key_value;
  keys[1] = missing_key;
  keys[2] = key0_value;
  values[0] = val(NULL, 0);
  values[1] = val(NULL, 0);
  values[2] = val(NULL, 0);
  counts[0] = SIZE_MAX;
  counts[1] = SIZE_MAX;
  counts[2] = SIZE_MAX;
  CHECK(mdbx_async_get_ex_batch(async, txn, dbi, keys, values, counts, op_results, 3, &op));
  CHECK_OP(op);
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "dupsort async get_ex batch fallback returned wrong results");
  REQUIRE(counts[0] == 2 && counts[1] == 0 && counts[2] == 1,
          "dupsort async get_ex batch fallback returned wrong duplicate counts");
  CHECK(expect_payload(&values[0], dup0, __FILE__, __LINE__));
  CHECK(expect_empty_value(&values[1], __FILE__, __LINE__));
  CHECK(expect_payload(&values[2], value0, __FILE__, __LINE__));

  keys[0] = key0_value;
  keys[1] = missing_key;
  keys[2] = key1_value;
  values[0] = val(NULL, 0);
  values[1] = val(NULL, 0);
  values[2] = val(NULL, 0);
  CHECK(mdbx_async_get_equal_or_great_batch(async, txn, dbi, keys, values, op_results, 3, &op));
  CHECK_OP(op);
  REQUIRE((op_results[0] == MDBX_SUCCESS || op_results[0] == MDBX_RESULT_TRUE) &&
              op_results[1] == MDBX_NOTFOUND &&
              (op_results[2] == MDBX_SUCCESS || op_results[2] == MDBX_RESULT_TRUE),
          "dupsort async lowerbound batch fallback returned wrong results");
  CHECK(expect_payload(&values[0], value0, __FILE__, __LINE__));
  CHECK(expect_empty_value(&values[1], __FILE__, __LINE__));
  CHECK(expect_payload(&values[2], value1, __FILE__, __LINE__));

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);

  MDBX_val cursor_key = dup_key_value;
  MDBX_val cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_SET_KEY, &op));
  CHECK(wait_result("mdbx_async_cursor_get dupsort fallback", &op, &operation_result,
                    __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS,
          "dupsort async cursor_get fallback returned wrong result");
  REQUIRE(cursor_key.iov_len == sizeof(uint64_t),
          "dupsort async cursor_get fallback returned wrong key size");
  uint64_t cursor_actual_key = UINT64_MAX;
  memcpy(&cursor_actual_key, cursor_key.iov_base, sizeof(cursor_actual_key));
  REQUIRE(cursor_actual_key == dup_key, "dupsort async cursor_get fallback returned wrong key");
  CHECK(expect_payload(&cursor_data, dup0, __FILE__, __LINE__));

  completed = 0;
  cursor_probe.items_seen = 0;
  CHECK(mdbx_async_cursor_get_loop(async, cursor, 4, MDBX_FIRST, MDBX_NEXT,
                                   async_dupsort_cursor_loop_func, &cursor_probe,
                                   &completed, &op));
  CHECK(wait_result("mdbx_async_cursor_get_loop dupsort fallback", &op, &operation_result,
                    __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS,
          "dupsort async cursor_get_loop fallback returned wrong result");
  REQUIRE(completed == 4 && cursor_probe.items_seen == 4,
          "dupsort async cursor_get_loop fallback did not visit every item");

  cursor_probe.items_seen = 0;
  CHECK(mdbx_async_cursor_scan(async, cursor, async_dupsort_cursor_scan_func,
                               &cursor_probe, MDBX_FIRST, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan dupsort fallback", &op, &operation_result,
                    __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS,
          "dupsort async cursor_scan fallback returned wrong result");
  REQUIRE(cursor_probe.items_seen == 4,
          "dupsort async cursor_scan fallback did not visit every item");

  size_t cursor_pair_count = 0;
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &cursor_pair_count, cursor_pairs,
                                    sizeof(cursor_pairs) / sizeof(cursor_pairs[0]),
                                    MDBX_FIRST, &op));
  CHECK(wait_result("mdbx_async_cursor_get_batch dupsort fallback", &op,
                    &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_INCOMPATIBLE,
          "dupsort async cursor_get_batch fallback returned wrong result");
  REQUIRE(cursor_pair_count == 0,
          "dupsort async cursor_get_batch fallback unexpectedly returned pairs");

  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), false));
  REQUIRE(read_stats.storage_read_items > 0, "dupsort fallback async reads did not submit storage reads");
  REQUIRE(read_stats.storage_read_completed >= read_stats.storage_read_items,
          "dupsort fallback async reads did not complete submitted reads");
  REQUIRE(read_stats.storage_read_errors == 0, "dupsort fallback async reads reported read errors");
  if (env_enabled("MDBX_ASYNC_SMOKE_EXPECT_IOURING")) {
    REQUIRE(read_stats.iouring_read_items > 0, "dupsort fallback async reads did not use io_uring");
    REQUIRE(read_stats.iouring_read_batches > 0, "dupsort fallback async reads reported no io_uring batches");
  }
  if (env_enabled("MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING")) {
    REQUIRE(read_stats.iouring_read_items == 0, "dupsort fallback unexpectedly used io_uring");
    REQUIRE(read_stats.iouring_read_batches == 0, "dupsort fallback unexpectedly reported io_uring batches");
  }

  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_env_close_ex(async, false, &op));
  CHECK(wait_result("mdbx_async_env_close_ex dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS, "unexpected async env close after dupsort fallback");
  env = NULL;
  CHECK(mdbx_async_env_delete(async, path, MDBX_ENV_JUST_DELETE, &op));
  CHECK(wait_result("mdbx_async_env_delete dupsort fallback", &op, &operation_result, __FILE__, __LINE__));
  REQUIRE(operation_result == MDBX_SUCCESS || operation_result == MDBX_RESULT_TRUE,
          "unexpected async env delete after dupsort fallback");
  CHECK(mdbx_async_destroy(async, true));
  return MDBX_SUCCESS;

bailout:
  for (unsigned i = 0; i < 3; ++i) {
    if (ops[i]) {
      int ignored = MDBX_SUCCESS;
      (void)mdbx_async_wait(ops[i], &ignored);
      (void)mdbx_async_op_release(ops[i]);
      ops[i] = NULL;
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (cache_ops[i]) {
      int ignored = MDBX_SUCCESS;
      (void)mdbx_async_wait(cache_ops[i], &ignored);
      (void)mdbx_async_op_release(cache_ops[i]);
      cache_ops[i] = NULL;
    }
  }
  if (cursor && async) {
    MDBX_async_op *cleanup_op = NULL;
    int ignored = MDBX_SUCCESS;
    if (mdbx_async_cursor_close(async, cursor, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
      (void)mdbx_async_wait(cleanup_op, &ignored);
      (void)mdbx_async_op_release(cleanup_op);
    }
    cursor = NULL;
  }
  if (txn && async) {
    MDBX_async_op *cleanup_op = NULL;
    int ignored = MDBX_SUCCESS;
    if (mdbx_async_txn_abort(async, txn, NULL, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
      (void)mdbx_async_wait(cleanup_op, &ignored);
      (void)mdbx_async_op_release(cleanup_op);
    }
    txn = NULL;
  }
  if (async) {
    if (env) {
      MDBX_async_op *cleanup_op = NULL;
      int ignored = MDBX_SUCCESS;
      if (mdbx_async_env_close_ex(async, true, &cleanup_op) == MDBX_SUCCESS && cleanup_op) {
        (void)mdbx_async_wait(cleanup_op, &ignored);
        (void)mdbx_async_op_release(cleanup_op);
      }
      env = NULL;
    }
    (void)mdbx_async_destroy(async, true);
  }
  (void)mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  return rc ? rc : MDBX_PROBLEM;
}

int main(void) {
  char path[96];
  char copy_env_path[128];
  char copy_txn_path[128];
  MDBX_env *env = NULL;
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_txn *origin_txn = NULL;
  MDBX_txn *clone_txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_cursor *cursor2 = NULL;
  MDBX_async_op *op = NULL;
  MDBX_dbi dbi = 0;
  MDBX_dbi enum_dbi = 0;
  MDBX_dbi close_dbi = 0;
  MDBX_dbi drop_dbi = 0;
  MDBX_dbi rename_dbi = 0;
  MDBX_dbi range_dbi = 0;
  MDBX_dbi bunch_dbi = 0;
  MDBX_dbi del_loop_dbi = 0;
  MDBX_dbi put_loop_dbi = 0;
  MDBX_dbi large_dbi = 0;
  MDBX_dbi key_del_loop_dbi = 0;
  MDBX_dbi replace_loop_dbi = 0;
  MDBX_dbi replace_delete_loop_dbi = 0;
  MDBX_dbi replace_ex_delete_loop_dbi = 0;
  MDBX_dbi custom_cstr_dbi = 0;
  MDBX_dbi custom_val_dbi = 0;
  uint64_t keys[ITEM_COUNT];
  uint64_t values[ITEM_COUNT];
  uint64_t large_keys[LARGE_ITEM_COUNT];
  uint8_t large_values[LARGE_ITEM_COUNT][LARGE_VALUE_BYTES];
  uint64_t cursor_extra_key = ITEM_COUNT;
  uint64_t cursor_extra_value = expected_value(ITEM_COUNT);
  uint64_t checkpoint_key = ITEM_COUNT + 100;
  uint64_t checkpoint_value = expected_value(checkpoint_key);
  uint64_t rollback_key = ITEM_COUNT + 101;
  uint64_t rollback_value = expected_value(rollback_key);
  uint64_t amend_key = ITEM_COUNT + 102;
  uint64_t amend_value = expected_value(amend_key);
  uint64_t replacement_value = expected_value(1) + UINT64_C(1000);
  uint64_t replace_ex_dirty_value = expected_value(2) + UINT64_C(2000);
  MDBX_val key_values[ITEM_COUNT];
  MDBX_val delete_keys[ITEM_COUNT];
  MDBX_val put_values[ITEM_COUNT];
  MDBX_val large_key_values[LARGE_ITEM_COUNT];
  MDBX_val large_put_values[LARGE_ITEM_COUNT];
  uint8_t open2_name_bytes[] = {'a', 's', 'y', 'n', 'c', '-', 'o', 'p', 'e', 'n', '2', 0, 'o', 'l', 'd'};
  uint8_t rename2_name_bytes[] = {'a', 's', 'y', 'n', 'c', '-', 'o', 'p', 'e', 'n', '2', 0, 'n', 'e', 'w'};
  uint8_t custom_val_name_bytes[] = {'a', 's', 'y', 'n', 'c', '-', 'c', 'm', 'p', 0, 'v', 'a', 'l'};
  uint8_t custom_key_a[] = {'a'};
  uint8_t custom_key_b[] = {'b'};
  uint8_t custom_value_a[] = {'A'};
  uint8_t custom_value_b[] = {'B'};
  MDBX_val open2_name = val(open2_name_bytes, sizeof(open2_name_bytes));
  MDBX_val rename2_name = val(rename2_name_bytes, sizeof(rename2_name_bytes));
  MDBX_val custom_val_name = val(custom_val_name_bytes, sizeof(custom_val_name_bytes));
  MDBX_val custom_key_a_value = val(custom_key_a, sizeof(custom_key_a));
  MDBX_val custom_key_b_value = val(custom_key_b, sizeof(custom_key_b));
  MDBX_val custom_put_value_a = val(custom_value_a, sizeof(custom_value_a));
  MDBX_val custom_put_value_b = val(custom_value_b, sizeof(custom_value_b));
  MDBX_val cursor_extra_key_value = val(&cursor_extra_key, sizeof(cursor_extra_key));
  MDBX_val cursor_extra_put_value = val(&cursor_extra_value, sizeof(cursor_extra_value));
  MDBX_val checkpoint_key_value = val(&checkpoint_key, sizeof(checkpoint_key));
  MDBX_val checkpoint_put_value = val(&checkpoint_value, sizeof(checkpoint_value));
  MDBX_val rollback_key_value = val(&rollback_key, sizeof(rollback_key));
  MDBX_val rollback_put_value = val(&rollback_value, sizeof(rollback_value));
  MDBX_val amend_key_value = val(&amend_key, sizeof(amend_key));
  MDBX_val amend_put_value = val(&amend_value, sizeof(amend_value));
  MDBX_val replacement_put_value = val(&replacement_value, sizeof(replacement_value));
  MDBX_val replace_ex_dirty_put_value = val(&replace_ex_dirty_value, sizeof(replace_ex_dirty_value));
  MDBX_val get_values[ITEM_COUNT];
  MDBX_async_op *ops[ITEM_COUNT];
  int op_results[ITEM_COUNT];
  int close_result = MDBX_SUCCESS;
  int rc = MDBX_SUCCESS;
  const bool expect_explicit_reads = env_enabled("MDBX_FORCE_NO_DATA_MMAP");
  MDBX_async_read_stats read_stats;

  memset(ops, 0, sizeof(ops));
  memset(&read_stats, 0, sizeof(read_stats));
  snprintf(path, sizeof(path), "./async-api-smoke-%llx", smoke_run_id());
  snprintf(copy_env_path, sizeof(copy_env_path), "./async-api-smoke-copy-env-%llx", smoke_run_id());
  snprintf(copy_txn_path, sizeof(copy_txn_path), "./async-api-smoke-copy-txn-%llx", smoke_run_id());
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_GET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_GET_EX_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_get_ex);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_LOWERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_lowerbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_SET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_set);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_RANGE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_range);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_UPPERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_upperbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_to_key_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_to_key_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GTE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_to_key_gte);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_to_key_gt);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_LAST_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_last);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_POSITIONED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_prev_positioned);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_get_batch_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_current_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_from_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_sync_cursor_scan_from_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_SINGLE_GET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_SINGLE_GET_NOTFOUND_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_notfound);
  if (env_enabled("MDBX_ASYNC_SMOKE_SINGLE_GET_CACHE_HIT_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_cache_hit);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_MANY_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_many_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_MANY_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex_many_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound_many_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_set);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_RANGE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_range);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_UPPERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_upperbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_to_key_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_to_key_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GTE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_to_key_gte);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_to_key_gt);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LAST_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_last);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_PREV_POSITIONED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_prev_positioned);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCH_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batch_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_MANY_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_many_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_MANY_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded_many_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_GET_NOTFOUND_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_notfound);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_EX_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_ex);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_EX_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_ex_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_EX_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_ex_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_GET_EX_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_get_ex_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_lowerbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_lowerbound_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_lowerbound_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_lowerbound_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cursor_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CURSOR_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cursor_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCHES_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cursor_batches);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CURSOR_SCAN_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cursor_scan);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cache_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_READ_ONLY"))
    return exercise_async_read_path(path, false,
                                    async_read_large_cache_get_singlethreaded);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cache_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_MANY_READ_ONLY"))
    return exercise_async_read_path(path, false,
                                    async_read_large_cache_get_singlethreaded_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cache_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false,
                                    async_read_large_cache_get_singlethreaded_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_large_cache_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false,
                                    async_read_large_cache_get_singlethreaded_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_ABORT_ORDER_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_abort_order);
  if (env_enabled("MDBX_ASYNC_SMOKE_ABORT_ORDER_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_abort_order_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_ABORT_ORDER_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_abort_order_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_BATCH_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_batch_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_BATCH_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex_batch_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound_batch_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_BATCH_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_batch_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_MIXED_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded_batch_mixed);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_CACHE_ST_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cache_get_singlethreaded_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_GET_EX_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_get_ex_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_LOWERBOUND_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_lowerbound_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_current_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GTE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_gte);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_gt);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_UPPERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_upperbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_loop_from_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_CURRENT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GTE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_gte);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_gt);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_UPPERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_get_batches_from_upperbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_current_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_PREV_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_MISS_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GTE_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_gte);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GT_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_gt);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_UPPERBOUND_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_upperbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_NODUP_READ_ONLY"))
    return exercise_async_read_path(path, false, async_read_cursor_scan_from_nodup);
  if (env_enabled("MDBX_ASYNC_SMOKE_DUPSORT_FALLBACK_READ_ONLY"))
    return exercise_async_dupsort_read_fallback(path);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_ONLY"))
    return exercise_async_read_path(path, true, async_read_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_ex_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_lowerbound_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_ex_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_lowerbound_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_singlethreaded);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_singlethreaded_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_singlethreaded_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_ex);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_ex_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_ex_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_get_ex_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_lowerbound);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_lowerbound_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_lowerbound_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_lowerbound_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cursor_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cursor_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_BATCHES_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cursor_batches);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_SCAN_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cursor_scan);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_ONLY"))
    return exercise_async_read_path(path, true,
                                    async_read_large_cache_get_singlethreaded);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_MANY_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cache_get_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_MANY_ONLY"))
    return exercise_async_read_path(path, true,
                                    async_read_large_cache_get_singlethreaded_many);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cache_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_BATCH_ONLY"))
    return exercise_async_read_path(path, true,
                                    async_read_large_cache_get_singlethreaded_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_large_cache_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_LOOP_ONLY"))
    return exercise_async_read_path(path, true,
                                    async_read_large_cache_get_singlethreaded_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_get_ex_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_lowerbound_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_cache_get_singlethreaded_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LAST_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_last);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_PREV_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCH_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batch);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_CURRENT_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_PREV_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_PREV_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_SETKEY_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_SETKEY_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_EQUAL_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_EQUAL_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_loop_from_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_CURRENT_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_SETKEY_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_SETKEY_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_EQUAL_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_EQUAL_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_get_batches_from_equal_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_CURRENT_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_current);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_PREV_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_PREV_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from_prev);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_SETKEY_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from_setkey);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_SETKEY_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from_setkey_miss);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_EQUAL_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from_equal);
  if (env_enabled("MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_EQUAL_MISS_ONLY"))
    return exercise_async_read_path(path, true, async_read_cursor_scan_from_equal_miss);

  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
    rc = fail_rc("mdbx_env_delete", rc, __FILE__, __LINE__);
    goto bailout;
  }
  (void)mdbx_env_delete(copy_env_path, MDBX_ENV_JUST_DELETE);
  (void)mdbx_env_delete(copy_txn_path, MDBX_ENV_JUST_DELETE);

  CHECK(mdbx_async_create(NULL, MDBX_ASYNC_DEFAULTS, &async));
  REQUIRE(mdbx_async_env(async) == NULL, "new unbound async executor returned an environment");
  CHECK(mdbx_async_env_create(async, &env, &op));
  CHECK_OP(op);
  REQUIRE(mdbx_async_env(async) == env, "async env create did not bind the executor");
  CHECK(mdbx_async_env_set_maxdbs(async, 12, &op));
  CHECK_OP(op);
  MDBX_dbi maxdbs = 0;
  CHECK(mdbx_async_env_get_maxdbs(async, &maxdbs, &op));
  CHECK_OP(op);
  REQUIRE(maxdbs == 12, "unexpected async environment maxdbs option");
  CHECK(mdbx_async_env_set_maxreaders(async, 32, &op));
  CHECK_OP(op);
  unsigned maxreaders = 0;
  CHECK(mdbx_async_env_get_maxreaders(async, &maxreaders, &op));
  CHECK_OP(op);
  REQUIRE(maxreaders == 32, "unexpected async environment maxreaders option");
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  REQUIRE(mdbx_async_env(async) == env, "async executor returned wrong environment");
  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), true));

  struct async_probe probe = {0};
  CHECK(mdbx_async_submit(async, async_probe_func, &probe, &op));
  CHECK_OP(op);
  REQUIRE(probe.calls == 1, "generic async callback was not executed exactly once");

  int env_userctx_a = 41;
  int env_userctx_b = 42;
  void *env_context = NULL;
  CHECK(mdbx_async_env_set_userctx(async, &env_userctx_a, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_userctx(async, &env_context, &op));
  CHECK_OP(op);
  REQUIRE(env_context == &env_userctx_a, "unexpected async environment context");
  CHECK(mdbx_async_env_set_userctx(async, &env_userctx_b, &op));
  CHECK_OP(op);
  env_context = NULL;
  CHECK(mdbx_async_env_get_userctx(async, &env_context, &op));
  CHECK_OP(op);
  REQUIRE(env_context == &env_userctx_b, "unexpected updated async environment context");

  MDBX_hsr_func hsr_callback = NULL;
  CHECK(mdbx_async_env_set_hsr(async, hsr_probe_func, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_hsr(async, &hsr_callback, &op));
  CHECK_OP(op);
  REQUIRE(hsr_callback == hsr_probe_func, "unexpected async HSR callback");
  CHECK(mdbx_async_env_set_hsr(async, NULL, &op));
  CHECK_OP(op);
  hsr_callback = hsr_probe_func;
  CHECK(mdbx_async_env_get_hsr(async, &hsr_callback, &op));
  CHECK_OP(op);
  REQUIRE(hsr_callback == NULL, "async HSR callback was not cleared");

  const char *env_path = NULL;
  CHECK(mdbx_async_env_get_path(async, &env_path, &op));
  CHECK_OP(op);
  REQUIRE(env_path && strcmp(env_path, path) == 0, "unexpected async environment path");

  mdbx_filehandle_t env_fd;
  CHECK(mdbx_async_env_get_fd(async, &env_fd, &op));
  CHECK_OP(op);
#if defined(_WIN32) || defined(_WIN64)
  REQUIRE(env_fd != INVALID_HANDLE_VALUE && env_fd != NULL, "unexpected async environment file descriptor");
#else
  REQUIRE(env_fd >= 0, "unexpected async environment file descriptor");
#endif

  size_t syncbytes = 0;
  CHECK(mdbx_async_env_set_syncbytes(async, 131072, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_syncbytes(async, &syncbytes, &op));
  CHECK_OP(op);
  REQUIRE(syncbytes == 131072, "unexpected async environment sync-bytes option");

  CHECK(mdbx_async_env_set_syncperiod(async, 65536, &op));
  CHECK_OP(op);
  uint64_t option_value = 0;
  CHECK(mdbx_async_env_get_option(async, MDBX_opt_sync_period, &option_value, &op));
  CHECK_OP(op);
  REQUIRE(option_value == 65536, "unexpected async generic sync-period option");
  unsigned syncperiod = 0;
  CHECK(mdbx_async_env_get_syncperiod(async, &syncperiod, &op));
  CHECK_OP(op);
  REQUIRE(syncperiod == 65536, "unexpected async environment sync-period option");

  unsigned env_flags = 0;
  CHECK(mdbx_async_env_get_flags(async, &env_flags, &op));
  CHECK_OP(op);
  REQUIRE((env_flags & MDBX_NOSUBDIR) != 0, "async environment flags missed NOSUBDIR");
  CHECK(mdbx_async_env_set_flags(async, MDBX_NOMETASYNC, true, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_flags(async, &env_flags, &op));
  CHECK_OP(op);
  REQUIRE((env_flags & MDBX_NOMETASYNC) != 0, "async environment flag set did not stick");
  CHECK(mdbx_async_env_set_flags(async, MDBX_NOMETASYNC, false, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_flags(async, &env_flags, &op));
  CHECK_OP(op);
  REQUIRE((env_flags & MDBX_NOMETASYNC) == 0, "async environment flag clear did not stick");

  int env_limit = 0;
  CHECK(mdbx_async_env_get_maxkeysize_ex(async, MDBX_DB_DEFAULTS, &env_limit, &op));
  CHECK_OP(op);
  REQUIRE(env_limit > 0, "unexpected async max key size");
  CHECK(mdbx_async_env_get_maxvalsize_ex(async, MDBX_DB_DEFAULTS, &env_limit, &op));
  CHECK_OP(op);
  REQUIRE(env_limit > 0, "unexpected async max value size");
  CHECK(mdbx_async_env_get_pairsize4page_max(async, MDBX_DB_DEFAULTS, &env_limit, &op));
  CHECK_OP(op);
  REQUIRE(env_limit > 0, "unexpected async max pair size");
  CHECK(mdbx_async_env_get_valsize4page_max(async, MDBX_DB_DEFAULTS, &env_limit, &op));
  CHECK_OP(op);
  REQUIRE(env_limit > 0, "unexpected async max page value size");

  CHECK(mdbx_async_env_set_geometry(async, -1, -1, -1, -1, -1, -1, &op));
  CHECK_OP(op);

  int env_operation_result = MDBX_SUCCESS;
  CHECK(mdbx_async_env_sync_poll(async, &op));
  CHECK(wait_result("mdbx_async_env_sync_poll", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async environment sync-poll result");
  CHECK(mdbx_async_env_sync(async, &op));
  CHECK(wait_result("mdbx_async_env_sync", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async environment sync result");
  CHECK(mdbx_async_env_warmup(async, NULL, MDBX_warmup_default, 0, &op));
  CHECK(wait_result("mdbx_async_env_warmup", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_ENOSYS,
          "unexpected async environment warmup result");

  CHECK(mdbx_async_thread_register(async, &op));
  CHECK(wait_result("mdbx_async_thread_register", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async thread register result");
  struct reader_probe reader_probe = {0};
  CHECK(mdbx_async_reader_list(async, reader_probe_func, &reader_probe, &op));
  CHECK(wait_result("mdbx_async_reader_list", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS, "unexpected async reader list result");
  REQUIRE(reader_probe.calls > 0, "async reader list did not see registered worker");
  int dead_readers = -1;
  CHECK(mdbx_async_reader_check(async, &dead_readers, &op));
  CHECK(wait_result("mdbx_async_reader_check", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async reader check result");
  REQUIRE(dead_readers >= 0, "async reader check did not update dead count");
  CHECK(mdbx_async_thread_unregister(async, &op));
  CHECK(wait_result("mdbx_async_thread_unregister", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async thread unregister result");

  CHECK(mdbx_async_txn_lock(async, true, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_unlock(async, &op));
  CHECK_OP(op);

  MDBX_stat env_stat;
  memset(&env_stat, 0, sizeof(env_stat));
  CHECK(mdbx_async_env_stat_ex(async, NULL, &env_stat, sizeof(env_stat), &op));
  CHECK_OP(op);
  REQUIRE(env_stat.ms_psize != 0, "async environment stat returned empty page size");

  MDBX_envinfo env_info;
  memset(&env_info, 0, sizeof(env_info));
  CHECK(mdbx_async_env_info_ex(async, NULL, &env_info, sizeof(env_info), &op));
  CHECK_OP(op);
  REQUIRE(env_info.mi_dxb_pagesize != 0, "async environment info returned empty page size");

  int txn_userctx_a = 51;
  int txn_userctx_b = 52;
  void *txn_context = NULL;
  CHECK(mdbx_async_txn_begin_ex(async, NULL, 0, &txn, &txn_userctx_a, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "write transaction was not returned");
  CHECK(mdbx_async_txn_get_userctx(async, txn, &txn_context, &op));
  CHECK_OP(op);
  REQUIRE(txn_context == &txn_userctx_a, "unexpected initial async transaction context");
  CHECK(mdbx_async_txn_set_userctx(async, txn, &txn_userctx_b, &op));
  CHECK_OP(op);
  txn_context = NULL;
  CHECK(mdbx_async_txn_get_userctx(async, txn, &txn_context, &op));
  CHECK_OP(op);
  REQUIRE(txn_context == &txn_userctx_b, "unexpected updated async transaction context");

  MDBX_env *txn_env = NULL;
  CHECK(mdbx_async_txn_env(async, txn, &txn_env, &op));
  CHECK_OP(op);
  REQUIRE(txn_env == env, "unexpected async transaction environment");

  MDBX_txn_flags_t txn_flags = MDBX_TXN_INVALID;
  CHECK(mdbx_async_txn_flags(async, txn, &txn_flags, &op));
  CHECK_OP(op);
  REQUIRE((txn_flags & MDBX_TXN_RDONLY) == 0, "write transaction reported readonly flag");

  uint64_t txn_id = 0;
  CHECK(mdbx_async_txn_id(async, txn, &txn_id, &op));
  CHECK_OP(op);
  REQUIRE(txn_id != 0, "async transaction id was empty");

  int txn_lag = -1;
  int txn_percent = -1;
  CHECK(mdbx_async_txn_straggler(async, txn, &txn_lag, &txn_percent, &op));
  CHECK_OP(op);
  REQUIRE(txn_lag >= 0 && txn_percent >= 0 && txn_percent <= 100, "unexpected async transaction straggler info");

  CHECK(mdbx_async_dbi_open(async, txn, NULL, MDBX_DB_DEFAULTS, &dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, "async-large-cache-target", MDBX_CREATE, &large_dbi, &op));
  CHECK_OP(op);

  CHECK(mdbx_async_put(async, txn, dbi, &checkpoint_key_value, &checkpoint_put_value, 0, &op));
  CHECK_OP(op);
  MDBX_commit_latency checkpoint_latency;
  memset(&checkpoint_latency, 0, sizeof(checkpoint_latency));
  int checkpoint_result = MDBX_SUCCESS;
  CHECK(mdbx_async_txn_checkpoint(async, txn, MDBX_TXN_NOWEAKING, &checkpoint_latency, &op));
  CHECK(wait_result("mdbx_async_txn_checkpoint", &op, &checkpoint_result, __FILE__, __LINE__));
  REQUIRE(checkpoint_result == MDBX_SUCCESS, "unexpected async transaction checkpoint result");
  CHECK(mdbx_async_put(async, txn, dbi, &rollback_key_value, &rollback_put_value, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_rollback(async, txn, &op));
  CHECK_OP(op);
  MDBX_val checkpoint_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, dbi, &checkpoint_key_value, &checkpoint_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&checkpoint_data, checkpoint_value, __FILE__, __LINE__));
  MDBX_val rollback_data = val(NULL, 0);
  int rollback_get_result = MDBX_SUCCESS;
  CHECK(mdbx_async_get(async, txn, dbi, &rollback_key_value, &rollback_data, &op));
  CHECK(wait_result("mdbx_async_get rollback marker", &op, &rollback_get_result, __FILE__, __LINE__));
  REQUIRE(rollback_get_result == MDBX_NOTFOUND, "async rollback marker survived rollback");
  CHECK(mdbx_async_del(async, txn, dbi, &checkpoint_key_value, NULL, &op));
  CHECK_OP(op);

  uint64_t sequence_value = UINT64_MAX;
  CHECK(mdbx_async_dbi_sequence(async, txn, dbi, &sequence_value, 7, &op));
  CHECK_OP(op);
  REQUIRE(sequence_value == 0, "unexpected initial async dbi sequence");

  MDBX_canary canary = {11, 22, 33, 0};
  CHECK(mdbx_async_canary_put(async, txn, &canary, &op));
  CHECK_OP(op);
  MDBX_canary canary_read;
  memset(&canary_read, 0, sizeof(canary_read));
  CHECK(mdbx_async_canary_get(async, txn, &canary_read, &op));
  CHECK_OP(op);
  REQUIRE(canary_read.x == canary.x && canary_read.y == canary.y && canary_read.z == canary.z,
          "unexpected async canary values in write txn");
  REQUIRE(canary_read.v != 0, "async canary did not receive transaction id");

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    keys[i] = i;
    values[i] = expected_value(keys[i]);
    key_values[i] = val(&keys[i], sizeof(keys[i]));
    put_values[i] = val(&values[i], sizeof(values[i]));
  }
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    large_keys[i] = ITEM_COUNT + 200u + i;
    fill_large_value(large_values[i], sizeof(large_values[i]), large_keys[i]);
    large_key_values[i] = val(&large_keys[i], sizeof(large_keys[i]));
    large_put_values[i] = val(large_values[i], sizeof(large_values[i]));
  }

  CHECK(mdbx_async_dbi_open(async, txn, "async-enum-target", MDBX_CREATE, &enum_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, enum_dbi, &key_values[2], &put_values[2], 0, &op));
  CHECK_OP(op);
  struct enum_probe enum_probe = {0, false};
  CHECK(mdbx_async_enumerate_tables(async, txn, enum_probe_func, &enum_probe, &op));
  CHECK_OP(op);
  REQUIRE(enum_probe.calls > 0 && enum_probe.saw_target, "async enumerate did not see target table");
  CHECK(mdbx_async_drop(async, txn, enum_dbi, true, &op));
  CHECK_OP(op);
  enum_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-drop-target", MDBX_CREATE, &drop_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, drop_dbi, &key_values[0], &put_values[0], 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_drop(async, txn, drop_dbi, false, &op));
  CHECK_OP(op);
  MDBX_stat drop_stat;
  memset(&drop_stat, 0, sizeof(drop_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, drop_dbi, &drop_stat, sizeof(drop_stat), &op));
  CHECK_OP(op);
  REQUIRE(drop_stat.ms_entries == 0, "async drop(false) did not empty table");
  CHECK(mdbx_async_put(async, txn, drop_dbi, &key_values[0], &put_values[0], 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_drop(async, txn, drop_dbi, true, &op));
  CHECK_OP(op);
  drop_dbi = 0;

  CHECK(mdbx_async_dbi_open2(async, txn, &open2_name, MDBX_CREATE, &rename_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, rename_dbi, &key_values[1], &put_values[1], 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_rename(async, txn, rename_dbi, "async-renamed-cstr", &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_rename2(async, txn, rename_dbi, &rename2_name, &op));
  CHECK_OP(op);
  MDBX_stat rename_stat;
  memset(&rename_stat, 0, sizeof(rename_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, rename_dbi, &rename_stat, sizeof(rename_stat), &op));
  CHECK_OP(op);
  REQUIRE(rename_stat.ms_entries == 1, "async renamed table lost payload");
  CHECK(mdbx_async_drop(async, txn, rename_dbi, true, &op));
  CHECK_OP(op);
  rename_dbi = 0;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  CHECK(mdbx_async_dbi_open_ex(async, txn, "async-custom-cstr", MDBX_CREATE, &custom_cstr_dbi,
                               async_custom_cmp, NULL, &op));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, custom_cstr_dbi, &custom_key_b_value, &custom_put_value_b, 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, custom_cstr_dbi, &custom_key_a_value, &custom_put_value_a, 0, &op));
  CHECK_OP(op);
  MDBX_stat custom_stat;
  memset(&custom_stat, 0, sizeof(custom_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, custom_cstr_dbi, &custom_stat, sizeof(custom_stat), &op));
  CHECK_OP(op);
  REQUIRE(custom_stat.ms_entries == 2, "async custom-comparator cstr table lost payload");
  CHECK(mdbx_async_drop(async, txn, custom_cstr_dbi, true, &op));
  CHECK_OP(op);
  custom_cstr_dbi = 0;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  CHECK(mdbx_async_dbi_open_ex2(async, txn, &custom_val_name, MDBX_CREATE, &custom_val_dbi,
                                async_custom_cmp, NULL, &op));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
  CHECK_OP(op);
  CHECK(mdbx_async_put(async, txn, custom_val_dbi, &custom_key_a_value, &custom_put_value_a, 0, &op));
  CHECK_OP(op);
  MDBX_val custom_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, custom_val_dbi, &custom_key_a_value, &custom_data, &op));
  CHECK_OP(op);
  REQUIRE(custom_data.iov_len == sizeof(custom_value_a) &&
              memcmp(custom_data.iov_base, custom_value_a, sizeof(custom_value_a)) == 0,
          "async custom-comparator val table returned wrong payload");
  CHECK(mdbx_async_drop(async, txn, custom_val_dbi, true, &op));
  CHECK_OP(op);
  custom_val_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-range-target", MDBX_CREATE, &range_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, range_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch range", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  CHECK(mdbx_async_cursor_open(async, txn, range_dbi, &cursor, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_open(async, txn, range_dbi, &cursor2, &op));
  CHECK_OP(op);
  MDBX_val range_key = key_values[1];
  MDBX_val range_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &range_key, &range_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  range_key = key_values[3];
  range_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor2, &range_key, &range_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  uint64_t affected = UINT64_MAX;
  CHECK(mdbx_async_cursor_delete_range(async, cursor, cursor2, true, &affected, &op));
  CHECK_OP(op);
  REQUIRE(affected == 3, "unexpected async cursor range deletion count");
  CHECK(mdbx_async_cursor_close(async, cursor2, &op));
  CHECK_OP(op);
  cursor2 = NULL;
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;
  MDBX_stat range_stat;
  memset(&range_stat, 0, sizeof(range_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, range_dbi, &range_stat, sizeof(range_stat), &op));
  CHECK_OP(op);
  REQUIRE(range_stat.ms_entries == 2, "async cursor range deletion left unexpected entries");
  CHECK(mdbx_async_drop(async, txn, range_dbi, true, &op));
  CHECK_OP(op);
  range_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-del-loop-target", MDBX_CREATE, &del_loop_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, del_loop_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch del loop", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  CHECK(mdbx_async_cursor_open(async, txn, del_loop_dbi, &cursor, &op));
  CHECK_OP(op);
  MDBX_val del_loop_key = key_values[1];
  MDBX_val del_loop_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &del_loop_key, &del_loop_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  size_t del_loop_completed = 0;
  CHECK(mdbx_async_cursor_del_loop(async, cursor, 3, MDBX_CURRENT, &del_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(del_loop_completed == 3, "unexpected async cursor delete loop count");
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;
  MDBX_stat del_loop_stat;
  memset(&del_loop_stat, 0, sizeof(del_loop_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, del_loop_dbi, &del_loop_stat, sizeof(del_loop_stat), &op));
  CHECK_OP(op);
  REQUIRE(del_loop_stat.ms_entries == 2, "async cursor delete loop left unexpected entries");
  CHECK(mdbx_async_drop(async, txn, del_loop_dbi, true, &op));
  CHECK_OP(op);
  del_loop_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-bunch-target", MDBX_CREATE, &bunch_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, bunch_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch bunch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  CHECK(mdbx_async_cursor_open(async, txn, bunch_dbi, &cursor, &op));
  CHECK_OP(op);
  MDBX_val bunch_key = key_values[2];
  MDBX_val bunch_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &bunch_key, &bunch_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  affected = UINT64_MAX;
  CHECK(mdbx_async_cursor_bunch_delete(async, cursor, MDBX_DELETE_AFTER_INCLUDING, &affected, &op));
  CHECK_OP(op);
  REQUIRE(affected == 3, "unexpected async cursor bunch deletion count");
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;
  MDBX_stat bunch_stat;
  memset(&bunch_stat, 0, sizeof(bunch_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, bunch_dbi, &bunch_stat, sizeof(bunch_stat), &op));
  CHECK_OP(op);
  REQUIRE(bunch_stat.ms_entries == 2, "async cursor bunch deletion left unexpected entries");
  CHECK(mdbx_async_drop(async, txn, bunch_dbi, true, &op));
  CHECK_OP(op);
  bunch_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-put-loop-target", MDBX_CREATE, &put_loop_dbi, &op));
  CHECK_OP(op);
  struct put_loop_probe put_loop_probe;
  memset(&put_loop_probe, 0, sizeof(put_loop_probe));
  const size_t put_loop_count = sizeof(put_loop_probe.keys) / sizeof(put_loop_probe.keys[0]);
  size_t put_loop_completed = 0;
  CHECK(mdbx_async_put_loop(async, txn, put_loop_dbi, put_loop_count, put_loop_item_func, put_loop_result_func,
                            &put_loop_probe, &put_loop_completed, 0, &op));
  CHECK_OP(op);
  REQUIRE(put_loop_completed == put_loop_count, "unexpected async put loop completion count");
  REQUIRE(put_loop_probe.items == put_loop_count && put_loop_probe.results == put_loop_count,
          "async put loop callbacks did not cover all items");
  MDBX_val put_loop_key = val(&put_loop_probe.keys[2], sizeof(put_loop_probe.keys[2]));
  MDBX_val put_loop_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, put_loop_dbi, &put_loop_key, &put_loop_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&put_loop_data, put_loop_probe.values[2], __FILE__, __LINE__));
  CHECK(mdbx_async_drop(async, txn, put_loop_dbi, true, &op));
  CHECK_OP(op);
  put_loop_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-key-del-loop-target", MDBX_CREATE, &key_del_loop_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, key_del_loop_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch key del loop", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  struct del_loop_probe del_loop_probe;
  memset(&del_loop_probe, 0, sizeof(del_loop_probe));
  const size_t key_del_loop_count = sizeof(del_loop_probe.keys) / sizeof(del_loop_probe.keys[0]);
  for (size_t i = 0; i < key_del_loop_count; ++i)
    del_loop_probe.keys[i] = keys[i + 1];
  size_t key_del_loop_completed = 0;
  CHECK(mdbx_async_del_loop(async, txn, key_del_loop_dbi, key_del_loop_count, del_loop_key_func, NULL,
                            del_loop_result_func, &del_loop_probe, &key_del_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(key_del_loop_completed == key_del_loop_count, "unexpected async delete loop completion count");
  REQUIRE(del_loop_probe.keys_seen == key_del_loop_count && del_loop_probe.results == key_del_loop_count,
          "async delete loop callbacks did not cover all items");
  MDBX_stat key_del_loop_stat;
  memset(&key_del_loop_stat, 0, sizeof(key_del_loop_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, key_del_loop_dbi, &key_del_loop_stat, sizeof(key_del_loop_stat), &op));
  CHECK_OP(op);
  REQUIRE(key_del_loop_stat.ms_entries == 2, "async delete loop left unexpected entries");
  CHECK(mdbx_async_drop(async, txn, key_del_loop_dbi, true, &op));
  CHECK_OP(op);
  key_del_loop_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-replace-loop-target", MDBX_CREATE, &replace_loop_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, replace_loop_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch replace loop", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  struct replace_loop_probe replace_loop_probe;
  memset(&replace_loop_probe, 0, sizeof(replace_loop_probe));
  const size_t replace_loop_count = sizeof(replace_loop_probe.keys) / sizeof(replace_loop_probe.keys[0]);
  for (size_t i = 0; i < replace_loop_count; ++i) {
    replace_loop_probe.keys[i] = keys[i + 1];
    replace_loop_probe.expected_old[i] = expected_value(replace_loop_probe.keys[i]);
  }
  size_t replace_loop_completed = 0;
  CHECK(mdbx_async_replace_loop(async, txn, replace_loop_dbi, replace_loop_count, replace_loop_item_func,
                                replace_loop_result_func, &replace_loop_probe, &replace_loop_completed, 0, &op));
  CHECK_OP(op);
  REQUIRE(replace_loop_completed == replace_loop_count, "unexpected async replace loop completion count");
  REQUIRE(replace_loop_probe.items == replace_loop_count && replace_loop_probe.results == replace_loop_count,
          "async replace loop callbacks did not cover all items");
  MDBX_val replace_loop_key = val(&replace_loop_probe.keys[2], sizeof(replace_loop_probe.keys[2]));
  MDBX_val replace_loop_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, replace_loop_dbi, &replace_loop_key, &replace_loop_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&replace_loop_data, replace_loop_probe.new_values[2], __FILE__, __LINE__));
  CHECK(mdbx_async_drop(async, txn, replace_loop_dbi, true, &op));
  CHECK_OP(op);
  replace_loop_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-replace-delete-loop-target", MDBX_CREATE, &replace_delete_loop_dbi,
                            &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, replace_delete_loop_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch replace delete loop", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  struct replace_delete_loop_probe replace_delete_loop_probe;
  memset(&replace_delete_loop_probe, 0, sizeof(replace_delete_loop_probe));
  const size_t replace_delete_loop_count =
      sizeof(replace_delete_loop_probe.keys) / sizeof(replace_delete_loop_probe.keys[0]);
  for (size_t i = 0; i < replace_delete_loop_count; ++i) {
    replace_delete_loop_probe.keys[i] = keys[i + 1];
    replace_delete_loop_probe.expected_old[i] = expected_value(replace_delete_loop_probe.keys[i]);
  }
  size_t replace_delete_loop_completed = 0;
  CHECK(mdbx_async_replace_delete_loop(async, txn, replace_delete_loop_dbi, replace_delete_loop_count,
                                       replace_delete_loop_item_func, replace_delete_loop_result_func,
                                       &replace_delete_loop_probe, &replace_delete_loop_completed, MDBX_CURRENT, &op));
  CHECK_OP(op);
  REQUIRE(replace_delete_loop_completed == replace_delete_loop_count,
          "unexpected async replace delete loop completion count");
  REQUIRE(replace_delete_loop_probe.items == replace_delete_loop_count &&
              replace_delete_loop_probe.results == replace_delete_loop_count,
          "async replace delete loop callbacks did not cover all items");
  MDBX_val replace_delete_loop_key = val(&replace_delete_loop_probe.keys[2], sizeof(replace_delete_loop_probe.keys[2]));
  MDBX_val replace_delete_loop_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, replace_delete_loop_dbi, &replace_delete_loop_key, &replace_delete_loop_data, &op));
  int replace_delete_loop_get_result = MDBX_SUCCESS;
  CHECK(wait_result("mdbx_async_get replace delete loop", &op, &replace_delete_loop_get_result, __FILE__, __LINE__));
  REQUIRE(replace_delete_loop_get_result == MDBX_NOTFOUND, "async replace delete loop did not remove key");
  CHECK(mdbx_async_drop(async, txn, replace_delete_loop_dbi, true, &op));
  CHECK_OP(op);
  replace_delete_loop_dbi = 0;

  CHECK(mdbx_async_dbi_open(async, txn, "async-replace-ex-delete-loop-target", MDBX_CREATE,
                            &replace_ex_delete_loop_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, replace_ex_delete_loop_dbi, key_values, put_values, op_results, 5, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 5; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch replace_ex delete loop", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  uint64_t replace_ex_delete_loop_dirty_values[] = {expected_value(1) + UINT64_C(11000),
                                                    expected_value(2) + UINT64_C(12000),
                                                    expected_value(3) + UINT64_C(13000)};
  MDBX_val replace_ex_delete_loop_dirty_data[] = {
      val(&replace_ex_delete_loop_dirty_values[0], sizeof(replace_ex_delete_loop_dirty_values[0])),
      val(&replace_ex_delete_loop_dirty_values[1], sizeof(replace_ex_delete_loop_dirty_values[1])),
      val(&replace_ex_delete_loop_dirty_values[2], sizeof(replace_ex_delete_loop_dirty_values[2]))};
  struct replace_delete_loop_probe replace_ex_delete_loop_probe;
  memset(&replace_ex_delete_loop_probe, 0, sizeof(replace_ex_delete_loop_probe));
  const size_t replace_ex_delete_loop_count =
      sizeof(replace_ex_delete_loop_probe.keys) / sizeof(replace_ex_delete_loop_probe.keys[0]);
  for (size_t i = 0; i < replace_ex_delete_loop_count; ++i) {
    replace_ex_delete_loop_probe.keys[i] = keys[i + 1];
    replace_ex_delete_loop_probe.expected_old[i] = replace_ex_delete_loop_dirty_values[i];
    CHECK(mdbx_async_put(async, txn, replace_ex_delete_loop_dbi, &key_values[i + 1],
                         &replace_ex_delete_loop_dirty_data[i], MDBX_CURRENT, &op));
    CHECK_OP(op);
  }
  struct preserve_probe preserve_delete_loop_probe = {0};
  size_t replace_ex_delete_loop_completed = 0;
  CHECK(mdbx_async_replace_ex_delete_loop(async, txn, replace_ex_delete_loop_dbi, replace_ex_delete_loop_count,
                                          replace_delete_loop_item_func, replace_delete_loop_result_func,
                                          &replace_ex_delete_loop_probe, &replace_ex_delete_loop_completed,
                                          MDBX_CURRENT, preserve_probe_func, &preserve_delete_loop_probe, &op));
  CHECK_OP(op);
  REQUIRE(replace_ex_delete_loop_completed == replace_ex_delete_loop_count,
          "unexpected async replace_ex delete loop completion count");
  REQUIRE(replace_ex_delete_loop_probe.items == replace_ex_delete_loop_count &&
              replace_ex_delete_loop_probe.results == replace_ex_delete_loop_count,
          "async replace_ex delete loop callbacks did not cover all items");
  REQUIRE(preserve_delete_loop_probe.calls == replace_ex_delete_loop_count,
          "async replace_ex_delete_loop preserver was not called per item");
  MDBX_val replace_ex_delete_loop_key =
      val(&replace_ex_delete_loop_probe.keys[2], sizeof(replace_ex_delete_loop_probe.keys[2]));
  MDBX_val replace_ex_delete_loop_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, replace_ex_delete_loop_dbi, &replace_ex_delete_loop_key,
                       &replace_ex_delete_loop_data, &op));
  int replace_ex_delete_loop_get_result = MDBX_SUCCESS;
  CHECK(wait_result("mdbx_async_get replace_ex delete loop", &op, &replace_ex_delete_loop_get_result, __FILE__,
                    __LINE__));
  REQUIRE(replace_ex_delete_loop_get_result == MDBX_NOTFOUND, "async replace_ex delete loop did not remove key");
  CHECK(mdbx_async_drop(async, txn, replace_ex_delete_loop_dbi, true, &op));
  CHECK_OP(op);
  replace_ex_delete_loop_dbi = 0;

  CHECK(mdbx_async_put(async, txn, dbi, &key_values[0], &put_values[0], 0, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_put_batch(async, txn, dbi, key_values + 1, put_values + 1, op_results, ITEM_COUNT - 1, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < ITEM_COUNT - 1; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  CHECK(mdbx_async_put_batch(async, txn, large_dbi, large_key_values, large_put_values, op_results,
                             LARGE_ITEM_COUNT, 0, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch large", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  memset(&env_stat, 0, sizeof(env_stat));
  CHECK(mdbx_async_env_stat_ex(async, NULL, &env_stat, sizeof(env_stat), &op));
  CHECK_OP(op);
  REQUIRE(env_stat.ms_entries == ITEM_COUNT + LARGE_ITEM_COUNT + 1,
          "unexpected async environment stat entries after commit");

  CHECK(mdbx_async_env_copy(async, copy_env_path, MDBX_CP_DONT_FLUSH, &op));
  CHECK_OP(op);
  CHECK(verify_copied_value(copy_env_path, keys[7], __FILE__, __LINE__));

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, "async-close-target", MDBX_CREATE, &close_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_dbi_close(async, close_dbi, &op));
  CHECK_OP(op);
  close_dbi = 0;
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_dbi_open(async, txn, "async-close-target", MDBX_DB_DEFAULTS, &close_dbi, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_drop(async, txn, close_dbi, true, &op));
  CHECK_OP(op);
  close_dbi = 0;
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &origin_txn));
  CHECK(mdbx_async_txn_clone(async, origin_txn, &clone_txn, &txn_userctx_a, &op));
  CHECK_OP(op);
  REQUIRE(clone_txn != NULL, "async transaction clone was not returned");
  txn_context = NULL;
  CHECK(mdbx_async_txn_get_userctx(async, clone_txn, &txn_context, &op));
  CHECK_OP(op);
  REQUIRE(txn_context == &txn_userctx_a, "unexpected async cloned transaction context");
  MDBX_val clone_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, clone_txn, dbi, &key_values[0], &clone_data, &op));
  CHECK_OP(op);
  CHECK(expect_value(&clone_data, keys[0], __FILE__, __LINE__));
  CHECK(mdbx_async_txn_abort(async, clone_txn, NULL, &op));
  CHECK_OP(op);
  clone_txn = NULL;
  CHECK(mdbx_txn_abort(origin_txn));
  origin_txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "abort-order read transaction was not returned");
  struct async_block_probe abort_order_probe;
  MDBX_val abort_order_data = val(NULL, 0);
  ops[0] = NULL;
  ops[1] = NULL;
  ops[2] = NULL;
  CHECK(async_block_probe_prepare(&abort_order_probe));
  CHECK(mdbx_async_submit(async, async_block_probe_func, &abort_order_probe, &ops[0]));
  CHECK(mdbx_async_get(async, txn, dbi, &key_values[1], &abort_order_data, &ops[1]));
  CHECK(mdbx_async_txn_abort(async, txn, NULL, &ops[2]));
  txn = NULL;
  CHECK(async_block_probe_release(&abort_order_probe));
  CHECK(wait_many_success("queued async read before txn abort", ops, 3, op_results,
                          __FILE__, __LINE__));
  async_block_probe_close(&abort_order_probe);
  REQUIRE(abort_order_probe.calls == 1, "abort-order async blocker did not run exactly once");
  CHECK(expect_value(&abort_order_data, keys[1], __FILE__, __LINE__));

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "abort-order batch read transaction was not returned");
  struct async_block_probe batch_abort_order_probe;
  MDBX_val abort_get_batch_data[3];
  int abort_get_batch_results[3];
  MDBX_val abort_get_ex_batch_keys[3];
  MDBX_val abort_get_ex_batch_data[3];
  size_t abort_get_ex_batch_counts[3];
  int abort_get_ex_batch_results[3];
  MDBX_val abort_lower_batch_keys[3];
  MDBX_val abort_lower_batch_data[3];
  int abort_lower_batch_results[3];
  MDBX_cache_entry_t abort_cache_entries[3];
  MDBX_cache_result_t abort_cache_results[3];
  MDBX_val abort_cache_data[3];
  MDBX_val abort_cursor_pairs[4];
  size_t abort_cursor_count = 0;
  memset(ops, 0, sizeof(ops));
  CHECK(async_block_probe_prepare(&batch_abort_order_probe));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_submit(async, async_block_probe_func, &batch_abort_order_probe, &ops[0]));
  for (unsigned i = 0; i < 3; ++i) {
    abort_get_batch_data[i] = val(NULL, 0);
    abort_get_batch_results[i] = MDBX_PROBLEM;
    abort_get_ex_batch_keys[i] = key_values[i + 2];
    abort_get_ex_batch_data[i] = val(NULL, 0);
    abort_get_ex_batch_counts[i] = SIZE_MAX;
    abort_get_ex_batch_results[i] = MDBX_PROBLEM;
    abort_lower_batch_keys[i] = key_values[i + 5];
    abort_lower_batch_data[i] = val(NULL, 0);
    abort_lower_batch_results[i] = MDBX_PROBLEM;
    mdbx_cache_init(&abort_cache_entries[i]);
    abort_cache_results[i].errcode = MDBX_PROBLEM;
    abort_cache_results[i].status = MDBX_CACHE_ERROR;
    abort_cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_get_batch(async, txn, dbi, key_values, abort_get_batch_data,
                             abort_get_batch_results, 3, &ops[1]));
  CHECK(mdbx_async_get_ex_batch(async, txn, dbi, abort_get_ex_batch_keys,
                                abort_get_ex_batch_data, abort_get_ex_batch_counts,
                                abort_get_ex_batch_results, 3, &ops[2]));
  CHECK(mdbx_async_get_equal_or_great_batch(async, txn, dbi, abort_lower_batch_keys,
                                            abort_lower_batch_data,
                                            abort_lower_batch_results, 3, &ops[3]));
  CHECK(mdbx_async_cache_get_batch(async, txn, dbi, &key_values[8],
                                   abort_cache_data, abort_cache_entries,
                                   abort_cache_results, 3, &ops[4]));
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &abort_cursor_count,
                                    abort_cursor_pairs, 4, MDBX_FIRST, &ops[5]));
  CHECK(mdbx_async_txn_abort(async, txn, NULL, &ops[6]));
  txn = NULL;
  cursor = NULL;
  CHECK(async_block_probe_release(&batch_abort_order_probe));
  CHECK(wait_many_success("queued async read batches before txn abort", ops, 7,
                          op_results, __FILE__, __LINE__));
  async_block_probe_close(&batch_abort_order_probe);
  REQUIRE(batch_abort_order_probe.calls == 1,
          "batch abort-order async blocker did not run exactly once");
  for (unsigned i = 0; i < 3; ++i) {
    REQUIRE(abort_get_batch_results[i] == MDBX_SUCCESS,
            "queued get batch before abort returned wrong result");
    CHECK(expect_value(&abort_get_batch_data[i], keys[i], __FILE__, __LINE__));
    REQUIRE(abort_get_ex_batch_results[i] == MDBX_SUCCESS,
            "queued get_ex batch before abort returned wrong result");
    REQUIRE(abort_get_ex_batch_counts[i] == 1,
            "queued get_ex batch before abort returned wrong value count");
    CHECK(expect_value(&abort_get_ex_batch_data[i], keys[i + 2], __FILE__, __LINE__));
    REQUIRE(abort_lower_batch_results[i] == MDBX_SUCCESS,
            "queued lowerbound batch before abort returned wrong result");
    CHECK(expect_value(&abort_lower_batch_data[i], keys[i + 5], __FILE__, __LINE__));
    REQUIRE(abort_cache_results[i].errcode == MDBX_SUCCESS &&
                abort_cache_results[i].status == MDBX_CACHE_REFRESHED,
            "queued cache batch before abort returned wrong result");
    CHECK(expect_value(&abort_cache_data[i], keys[i + 8], __FILE__, __LINE__));
  }
  REQUIRE(abort_cursor_count == 4,
          "queued cursor batch before abort returned wrong value count");
  for (unsigned i = 0; i < 2; ++i) {
    REQUIRE(abort_cursor_pairs[i * 2].iov_len == sizeof(uint64_t),
            "queued cursor batch before abort returned wrong key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, abort_cursor_pairs[i * 2].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == keys[i],
            "queued cursor batch before abort returned wrong key");
    CHECK(expect_value(&abort_cursor_pairs[i * 2 + 1], actual_key,
                       __FILE__, __LINE__));
  }

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "abort-order loop read transaction was not returned");
  struct async_block_probe loop_abort_order_probe;
  struct get_loop_probe abort_get_loop_probe = {0, 0, 0};
  struct get_ex_loop_probe abort_get_ex_loop_probe = {0, 0, 0, 0};
  struct get_equal_or_great_loop_probe abort_lower_loop_probe = {0};
  struct cache_loop_probe abort_cache_loop_probe = {0, 16, 0, 0, 0};
  struct cursor_get_loop_probe abort_cursor_loop_probe = {0};
  MDBX_cache_entry_t abort_cache_loop_entries[4];
  size_t abort_get_loop_completed = 0;
  size_t abort_get_ex_loop_completed = 0;
  size_t abort_lower_loop_completed = 0;
  size_t abort_cache_loop_completed = 0;
  size_t abort_cursor_loop_completed = 0;
  memset(ops, 0, sizeof(ops));
  for (unsigned i = 0; i < 4; ++i)
    mdbx_cache_init(&abort_cache_loop_entries[i]);
  CHECK(async_block_probe_prepare(&loop_abort_order_probe));
  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_submit(async, async_block_probe_func, &loop_abort_order_probe, &ops[0]));
  CHECK(mdbx_async_get_loop(async, txn, dbi, 4, get_loop_key_func,
                            get_loop_result_func, &abort_get_loop_probe,
                            &abort_get_loop_completed, &ops[1]));
  CHECK(mdbx_async_get_ex_loop(async, txn, dbi, 4, get_ex_loop_key_func,
                               get_ex_loop_result_func, &abort_get_ex_loop_probe,
                               &abort_get_ex_loop_completed, &ops[2]));
  CHECK(mdbx_async_get_equal_or_great_loop(async, txn, dbi, 4,
                                           get_equal_or_great_loop_key_func,
                                           get_equal_or_great_loop_data_func,
                                           get_equal_or_great_loop_result_func,
                                           &abort_lower_loop_probe,
                                           &abort_lower_loop_completed, &ops[3]));
  CHECK(mdbx_async_cache_get_loop(async, txn, dbi, 4, cache_loop_key_func,
                                  abort_cache_loop_entries, cache_loop_result_func,
                                  &abort_cache_loop_probe,
                                  &abort_cache_loop_completed, &ops[4]));
  CHECK(mdbx_async_cursor_get_loop(async, cursor, 4, MDBX_FIRST, MDBX_NEXT,
                                   cursor_get_loop_probe_func,
                                   &abort_cursor_loop_probe,
                                   &abort_cursor_loop_completed, &ops[5]));
  CHECK(mdbx_async_txn_abort(async, txn, NULL, &ops[6]));
  txn = NULL;
  cursor = NULL;
  CHECK(async_block_probe_release(&loop_abort_order_probe));
  CHECK(wait_many_success("queued async read loops before txn abort", ops, 7,
                          op_results, __FILE__, __LINE__));
  async_block_probe_close(&loop_abort_order_probe);
  REQUIRE(loop_abort_order_probe.calls == 1,
          "loop abort-order async blocker did not run exactly once");
  REQUIRE(abort_get_loop_completed == 4 && abort_get_loop_probe.keys == 4 &&
              abort_get_loop_probe.results == 4,
          "queued get loop before abort did not complete");
  REQUIRE(abort_get_ex_loop_completed == 4 && abort_get_ex_loop_probe.keys == 4 &&
              abort_get_ex_loop_probe.results == 4 &&
              abort_get_ex_loop_probe.values == 4,
          "queued get_ex loop before abort did not complete");
  REQUIRE(abort_lower_loop_completed == 4 && abort_lower_loop_probe.keys == 4 &&
              abort_lower_loop_probe.data == 4 &&
              abort_lower_loop_probe.results == 4 &&
              abort_lower_loop_probe.greater_results == 2,
          "queued lowerbound loop before abort did not complete");
  REQUIRE(abort_cache_loop_completed == 4 && abort_cache_loop_probe.keys == 4 &&
              abort_cache_loop_probe.results == 4,
          "queued cache loop before abort did not complete");
  REQUIRE(abort_cursor_loop_completed == 4 && abort_cursor_loop_probe.calls == 4,
          "queued cursor loop before abort did not complete");

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "read transaction was not returned");

  memset(&env_stat, 0, sizeof(env_stat));
  CHECK(mdbx_async_env_stat_ex(async, txn, &env_stat, sizeof(env_stat), &op));
  CHECK_OP(op);
  REQUIRE(env_stat.ms_entries == ITEM_COUNT + LARGE_ITEM_COUNT + 1,
          "unexpected async txn-scoped environment stat entries");

  memset(&env_info, 0, sizeof(env_info));
  CHECK(mdbx_async_env_info_ex(async, txn, &env_info, sizeof(env_info), &op));
  CHECK_OP(op);
  REQUIRE(env_info.mi_recent_txnid != 0, "async txn-scoped environment info returned empty transaction id");

  MDBX_txn_info txn_info;
  memset(&txn_info, 0, sizeof(txn_info));
  CHECK(mdbx_async_txn_info(async, txn, &txn_info, false, &op));
  CHECK_OP(op);
  REQUIRE(txn_info.txn_id != 0, "async txn info returned empty transaction id");
  REQUIRE(txn_info.txn_space_limit_hard >= txn_info.txn_space_used, "async txn info returned inconsistent geometry");

  MDBX_gc_info_t gc_info;
  struct gc_probe gc_probe = {0};
  memset(&gc_info, 0, sizeof(gc_info));
  CHECK(mdbx_async_gc_info(async, txn, &gc_info, sizeof(gc_info), gc_probe_func, &gc_probe, &op));
  CHECK(wait_result("mdbx_async_gc_info", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_NOTFOUND,
          "unexpected async GC info result");
  REQUIRE(gc_info.pages_total > 0 && gc_info.pages_backed > 0 && gc_info.pages_allocated > 0,
          "async GC info returned empty geometry");
  REQUIRE(gc_info.pages_total >= gc_info.pages_allocated, "async GC info returned inconsistent allocation");

  CHECK(mdbx_async_txn_park(async, txn, false, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_unpark(async, txn, true, &op));
  CHECK_OP(op);
  int refresh_result = MDBX_SUCCESS;
  CHECK(mdbx_async_txn_refresh(async, txn, &op));
  CHECK(wait_result("mdbx_async_txn_refresh", &op, &refresh_result, __FILE__, __LINE__));
  REQUIRE(refresh_result == MDBX_SUCCESS || refresh_result == MDBX_RESULT_TRUE, "unexpected async txn refresh result");

  memset(&canary_read, 0, sizeof(canary_read));
  CHECK(mdbx_async_canary_get(async, txn, &canary_read, &op));
  CHECK_OP(op);
  REQUIRE(canary_read.x == canary.x && canary_read.y == canary.y && canary_read.z == canary.z,
          "unexpected persisted async canary values");
  REQUIRE(canary_read.v != 0, "persisted async canary has empty transaction id");

  sequence_value = UINT64_MAX;
  CHECK(mdbx_async_dbi_sequence(async, txn, dbi, &sequence_value, 0, &op));
  CHECK_OP(op);
  REQUIRE(sequence_value == 7, "unexpected persisted async dbi sequence");

  MDBX_stat dbi_stat;
  memset(&dbi_stat, 0, sizeof(dbi_stat));
  CHECK(mdbx_async_dbi_stat(async, txn, dbi, &dbi_stat, sizeof(dbi_stat), &op));
  CHECK_OP(op);
  REQUIRE(dbi_stat.ms_entries == ITEM_COUNT + 1, "unexpected async dbi stat entry count");

  unsigned dbi_flags = UINT_MAX;
  unsigned dbi_state = UINT_MAX;
  CHECK(mdbx_async_dbi_flags_ex(async, txn, dbi, &dbi_flags, &dbi_state, &op));
  CHECK_OP(op);
  REQUIRE((dbi_flags & MDBX_DUPSORT) == 0, "unexpected async dbi dupsort flag");
  dbi_flags = UINT_MAX;
  CHECK(mdbx_async_dbi_flags(async, txn, dbi, &dbi_flags, &op));
  CHECK_OP(op);
  REQUIRE((dbi_flags & MDBX_DUPSORT) == 0, "unexpected async dbi shortcut dupsort flag");

  uint32_t depthmask = UINT32_MAX;
  int depthmask_result = MDBX_SUCCESS;
  CHECK(mdbx_async_dbi_dupsort_depthmask(async, txn, dbi, &depthmask, &op));
  CHECK(wait_result("mdbx_async_dbi_dupsort_depthmask", &op, &depthmask_result, __FILE__, __LINE__));
  REQUIRE(depthmask_result == MDBX_RESULT_TRUE, "non-dupsort dbi did not report MDBX_RESULT_TRUE");
  REQUIRE(depthmask == 0, "unexpected non-dupsort depthmask");

  CHECK(mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &origin_txn));
  MDBX_val sync_get_data = val(NULL, 0);
  CHECK(mdbx_get(origin_txn, dbi, &key_values[3], &sync_get_data));
  CHECK(expect_value(&sync_get_data, keys[3], __FILE__, __LINE__));

  MDBX_val sync_get_ex_key = key_values[4];
  MDBX_val sync_get_ex_data = val(NULL, 0);
  size_t sync_get_ex_count = SIZE_MAX;
  CHECK(mdbx_get_ex(origin_txn, dbi, &sync_get_ex_key, &sync_get_ex_data, &sync_get_ex_count));
  REQUIRE(sync_get_ex_count == 1, "blocking get_ex saw wrong value count");
  REQUIRE(sync_get_ex_key.iov_len == sizeof(uint64_t), "blocking get_ex returned wrong key size");
  uint64_t sync_actual_key = UINT64_MAX;
  memcpy(&sync_actual_key, sync_get_ex_key.iov_base, sizeof(sync_actual_key));
  REQUIRE(sync_actual_key == keys[4], "blocking get_ex returned wrong key");
  CHECK(expect_value(&sync_get_ex_data, sync_actual_key, __FILE__, __LINE__));

  uint64_t sync_lower_key_data = 5;
  MDBX_val sync_lower_key = val(&sync_lower_key_data, sizeof(sync_lower_key_data));
  MDBX_val sync_lower_data = val(NULL, 0);
  CHECK(mdbx_get_equal_or_great(origin_txn, dbi, &sync_lower_key, &sync_lower_data));
  REQUIRE(sync_lower_key.iov_len == sizeof(uint64_t), "blocking lowerbound returned wrong key size");
  memcpy(&sync_actual_key, sync_lower_key.iov_base, sizeof(sync_actual_key));
  REQUIRE(sync_actual_key == sync_lower_key_data, "blocking lowerbound returned wrong exact key");
  CHECK(expect_value(&sync_lower_data, sync_actual_key, __FILE__, __LINE__));

  uint8_t sync_greater_probe_bytes[sizeof(uint64_t) + 1];
  memcpy(sync_greater_probe_bytes, key_values[6].iov_base, sizeof(uint64_t));
  sync_greater_probe_bytes[sizeof(uint64_t)] = 0;
  MDBX_val sync_greater_key = val(sync_greater_probe_bytes, sizeof(sync_greater_probe_bytes));
  MDBX_val sync_greater_data = val(NULL, 0);
  rc = mdbx_get_equal_or_great(origin_txn, dbi, &sync_greater_key, &sync_greater_data);
  REQUIRE(rc == MDBX_RESULT_TRUE, "blocking lowerbound did not report greater key");
  REQUIRE(sync_greater_key.iov_len == sizeof(uint64_t), "blocking greater lowerbound returned wrong key size");
  memcpy(&sync_actual_key, sync_greater_key.iov_base, sizeof(sync_actual_key));
  REQUIRE(sync_actual_key == keys[7], "blocking greater lowerbound returned wrong key");
  CHECK(expect_value(&sync_greater_data, sync_actual_key, __FILE__, __LINE__));
  rc = MDBX_SUCCESS;

  uint8_t sync_missing_key_bytes[sizeof(uint64_t)];
  memset(sync_missing_key_bytes, 0xff, sizeof(sync_missing_key_bytes));
  MDBX_val sync_missing_key = val(sync_missing_key_bytes, sizeof(sync_missing_key_bytes));
  MDBX_val sync_missing_data = val(NULL, 0);
  rc = mdbx_get(origin_txn, dbi, &sync_missing_key, &sync_missing_data);
  REQUIRE(rc == MDBX_NOTFOUND, "blocking get missing key returned wrong result");
  CHECK(expect_empty_value(&sync_missing_data, __FILE__, __LINE__));

  MDBX_val sync_missing_get_ex_key = sync_missing_key;
  MDBX_val sync_missing_get_ex_data = val(NULL, 0);
  sync_get_ex_count = SIZE_MAX;
  rc = mdbx_get_ex(origin_txn, dbi, &sync_missing_get_ex_key, &sync_missing_get_ex_data, &sync_get_ex_count);
  REQUIRE(rc == MDBX_NOTFOUND, "blocking get_ex missing key returned wrong result");
  REQUIRE(sync_get_ex_count == 0, "blocking get_ex missing key returned values");
  CHECK(expect_empty_value(&sync_missing_get_ex_data, __FILE__, __LINE__));

  MDBX_val sync_missing_lower_key = sync_missing_key;
  MDBX_val sync_missing_lower_data = val(NULL, 0);
  rc = mdbx_get_equal_or_great(origin_txn, dbi, &sync_missing_lower_key, &sync_missing_lower_data);
  REQUIRE(rc == MDBX_NOTFOUND, "blocking lowerbound missing key returned wrong result");
  CHECK(expect_empty_value(&sync_missing_lower_data, __FILE__, __LINE__));
  rc = MDBX_SUCCESS;
  CHECK(mdbx_txn_abort(origin_txn));
  origin_txn = NULL;

  struct async_block_probe block_probe;
  MDBX_val queued_get_data = val(NULL, 0);
  ops[0] = NULL;
  ops[1] = NULL;
  CHECK(async_block_probe_prepare(&block_probe));
  CHECK(mdbx_async_submit(async, async_block_probe_func, &block_probe, &ops[0]));
  rc = mdbx_async_get(async, txn, dbi, &key_values[0], &queued_get_data, &ops[1]);
  if (rc != MDBX_SUCCESS) {
    (void)async_block_probe_release(&block_probe);
    if (ops[0])
      (void)mdbx_async_wait_release_all(ops, 1, op_results);
    async_block_probe_close(&block_probe);
    rc = fail_rc("mdbx_async_get queued behind blocker", rc, __FILE__, __LINE__);
    goto bailout;
  }
  const int early_release_rc = mdbx_async_op_release(ops[1]);
  const int early_destroy_rc = mdbx_async_destroy(async, false);
  CHECK(async_block_probe_release(&block_probe));
  CHECK(wait_many_success("blocked async read drain", ops, 2, op_results, __FILE__, __LINE__));
  async_block_probe_close(&block_probe);
  REQUIRE(block_probe.calls == 1, "async blocker did not run exactly once");
  REQUIRE(early_release_rc == MDBX_BUSY, "pending async read operation was released early");
  REQUIRE(early_destroy_rc == MDBX_BUSY, "async executor destroyed with pending read operation");
  CHECK(expect_value(&queued_get_data, keys[0], __FILE__, __LINE__));

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    get_values[i] = val(NULL, 0);
    CHECK(mdbx_async_get(async, txn, dbi, &key_values[i], &get_values[i], &ops[i]));
  }
  CHECK(wait_many_success("mdbx_async_get", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < ITEM_COUNT; ++i)
    get_values[i] = val(NULL, 0);
  CHECK(mdbx_async_get_many(async, txn, dbi, key_values, get_values, ITEM_COUNT, ops));
  CHECK(wait_many_success("mdbx_async_get_many", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  MDBX_val mixed_cursor_key = val(NULL, 0);
  MDBX_val mixed_cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &mixed_cursor_key, &mixed_cursor_data, MDBX_FIRST,
                              &ops[0]));
  for (unsigned i = 0; i < 4; ++i) {
    get_values[i] = val(NULL, 0);
    CHECK(mdbx_async_get(async, txn, dbi, &key_values[i + 8], &get_values[i], &ops[i + 1]));
  }
  CHECK(wait_many_success("mixed cursor get and async gets", ops, 5, op_results, __FILE__, __LINE__));
  REQUIRE(mixed_cursor_key.iov_len == sizeof(uint64_t), "mixed cursor get returned wrong key size");
  uint64_t mixed_cursor_actual_key = UINT64_MAX;
  memcpy(&mixed_cursor_actual_key, mixed_cursor_key.iov_base, sizeof(mixed_cursor_actual_key));
  REQUIRE(mixed_cursor_actual_key == 0, "mixed cursor get returned wrong first key");
  CHECK(expect_value(&mixed_cursor_data, mixed_cursor_actual_key, __FILE__, __LINE__));
  for (unsigned i = 0; i < 4; ++i)
    CHECK(expect_value(&get_values[i], keys[i + 8], __FILE__, __LINE__));

  MDBX_cache_entry_t mixed_cache_entries[4];
  MDBX_cache_result_t mixed_cache_results[4];
  MDBX_val mixed_cache_keys[4];
  MDBX_val mixed_cache_data[4];
  mixed_cursor_key = val(NULL, 0);
  mixed_cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &mixed_cursor_key, &mixed_cursor_data, MDBX_NEXT,
                              &ops[0]));
  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&mixed_cache_entries[i]);
    mixed_cache_results[i].errcode = MDBX_PROBLEM;
    mixed_cache_results[i].status = MDBX_CACHE_ERROR;
    mixed_cache_keys[i] = key_values[i + 12];
    mixed_cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_many(async, txn, dbi, mixed_cache_keys, mixed_cache_data,
                                  mixed_cache_entries, mixed_cache_results, 4, &ops[1]));
  CHECK(wait_many_success("mixed cursor get and cache gets", ops, 5, op_results, __FILE__, __LINE__));
  REQUIRE(mixed_cursor_key.iov_len == sizeof(uint64_t), "mixed cursor cache returned wrong key size");
  memcpy(&mixed_cursor_actual_key, mixed_cursor_key.iov_base, sizeof(mixed_cursor_actual_key));
  REQUIRE(mixed_cursor_actual_key == 1, "mixed cursor cache returned wrong next key");
  CHECK(expect_value(&mixed_cursor_data, mixed_cursor_actual_key, __FILE__, __LINE__));
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(mixed_cache_results[i].errcode == MDBX_SUCCESS &&
                mixed_cache_results[i].status == MDBX_CACHE_REFRESHED,
            "unexpected mixed async cache get result");
    CHECK(expect_value(&mixed_cache_data[i], keys[i + 12], __FILE__, __LINE__));
  }

  MDBX_val same_cursor_pairs[4];
  size_t same_cursor_count = 0;
  MDBX_val same_cursor_key = val(NULL, 0);
  MDBX_val same_cursor_data = val(NULL, 0);
  get_values[0] = val(NULL, 0);
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &same_cursor_count,
                                    same_cursor_pairs, 4, MDBX_NEXT, &ops[0]));
  CHECK(mdbx_async_cursor_get(async, cursor, &same_cursor_key, &same_cursor_data,
                              MDBX_NEXT, &ops[1]));
  CHECK(mdbx_async_get(async, txn, dbi, &key_values[20], &get_values[0], &ops[2]));
  CHECK(wait_many_success("same cursor queued async reads", ops, 3, op_results,
                          __FILE__, __LINE__));
  REQUIRE(same_cursor_count == 4, "same cursor batch returned wrong value count");
  for (unsigned i = 0; i < 2; ++i) {
    REQUIRE(same_cursor_pairs[i * 2].iov_len == sizeof(uint64_t),
            "same cursor batch returned wrong key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, same_cursor_pairs[i * 2].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == (uint64_t)(i + 1), "same cursor batch returned wrong key");
    CHECK(expect_value(&same_cursor_pairs[i * 2 + 1], actual_key, __FILE__, __LINE__));
  }
  REQUIRE(same_cursor_key.iov_len == sizeof(uint64_t),
          "same cursor queued get returned wrong key size");
  uint64_t same_cursor_actual_key = UINT64_MAX;
  memcpy(&same_cursor_actual_key, same_cursor_key.iov_base,
         sizeof(same_cursor_actual_key));
  REQUIRE(same_cursor_actual_key == 4, "same cursor queued get returned wrong key");
  CHECK(expect_value(&same_cursor_data, same_cursor_actual_key, __FILE__, __LINE__));
  CHECK(expect_value(&get_values[0], keys[20], __FILE__, __LINE__));

  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  MDBX_val get_ex_key = key_values[5];
  MDBX_val get_ex_data = val(NULL, 0);
  size_t values_count = 0;
  CHECK(mdbx_async_get_ex(async, txn, dbi, &get_ex_key, &get_ex_data, &values_count, &op));
  CHECK_OP(op);
  REQUIRE(values_count == 1, "unexpected get_ex value count");
  REQUIRE(get_ex_key.iov_len == sizeof(uint64_t), "unexpected get_ex key size");
  uint64_t get_ex_actual_key = 0;
  memcpy(&get_ex_actual_key, get_ex_key.iov_base, sizeof(get_ex_actual_key));
  REQUIRE(get_ex_actual_key == 5, "unexpected get_ex key");
  CHECK(expect_value(&get_ex_data, get_ex_actual_key, __FILE__, __LINE__));

  MDBX_val get_ex_many_keys[ITEM_COUNT];
  size_t get_ex_many_counts[ITEM_COUNT];
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    get_ex_many_keys[i] = key_values[i];
    get_values[i] = val(NULL, 0);
    get_ex_many_counts[i] = SIZE_MAX;
  }
  CHECK(mdbx_async_get_ex_many(async, txn, dbi, get_ex_many_keys, get_values, get_ex_many_counts,
                               ITEM_COUNT, ops));
  CHECK(wait_many_success("mdbx_async_get_ex_many", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    REQUIRE(get_ex_many_counts[i] == 1, "async get_ex many saw wrong value count");
    REQUIRE(get_ex_many_keys[i].iov_len == sizeof(uint64_t), "unexpected async get_ex many key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, get_ex_many_keys[i].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == keys[i], "unexpected async get_ex many key");
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

  uint64_t lower_key_data = 6;
  MDBX_val lower_key = val(&lower_key_data, sizeof(lower_key_data));
  MDBX_val lower_data = val(NULL, 0);
  CHECK(mdbx_async_get_equal_or_great(async, txn, dbi, &lower_key, &lower_data, &op));
  CHECK_OP(op);
  REQUIRE(lower_key.iov_len == sizeof(uint64_t), "unexpected equal-or-great key size");
  uint64_t lower_actual_key = 0;
  memcpy(&lower_actual_key, lower_key.iov_base, sizeof(lower_actual_key));
  REQUIRE(lower_actual_key == lower_key_data, "unexpected equal-or-great key");
  CHECK(expect_value(&lower_data, lower_actual_key, __FILE__, __LINE__));

  uint8_t greater_probe_bytes[sizeof(uint64_t) + 1];
  memcpy(greater_probe_bytes, key_values[6].iov_base, sizeof(uint64_t));
  greater_probe_bytes[sizeof(uint64_t)] = 0;
  MDBX_val greater_probe_key = val(greater_probe_bytes, sizeof(greater_probe_bytes));
  MDBX_val greater_probe_data = val(NULL, 0);
  int greater_probe_result = MDBX_SUCCESS;
  CHECK(mdbx_async_get_equal_or_great(async, txn, dbi, &greater_probe_key, &greater_probe_data, &op));
  CHECK(wait_result("mdbx_async_get_equal_or_great greater", &op, &greater_probe_result, __FILE__, __LINE__));
  REQUIRE(greater_probe_result == MDBX_RESULT_TRUE, "equal-or-great probe did not report greater key");
  REQUIRE(greater_probe_key.iov_len == sizeof(uint64_t), "unexpected greater equal-or-great key size");
  uint64_t greater_actual_key = 0;
  memcpy(&greater_actual_key, greater_probe_key.iov_base, sizeof(greater_actual_key));
  REQUIRE(greater_actual_key == 7, "unexpected greater equal-or-great key");
  CHECK(expect_value(&greater_probe_data, greater_actual_key, __FILE__, __LINE__));

  MDBX_val equal_many_keys[8];
  MDBX_val equal_many_data[8];
  uint64_t equal_many_exact_keys[8];
  uint8_t equal_many_greater_keys[4][sizeof(uint64_t) + 1];
  for (unsigned i = 0; i < 8; ++i) {
    equal_many_exact_keys[i] = i;
    if (i & 1) {
      memcpy(equal_many_greater_keys[i / 2], &equal_many_exact_keys[i], sizeof(equal_many_exact_keys[i]));
      equal_many_greater_keys[i / 2][sizeof(equal_many_exact_keys[i])] = 0;
      equal_many_keys[i] = val(equal_many_greater_keys[i / 2], sizeof(equal_many_greater_keys[i / 2]));
    } else {
      equal_many_keys[i] = val(&equal_many_exact_keys[i], sizeof(equal_many_exact_keys[i]));
    }
    equal_many_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_get_equal_or_great_many(async, txn, dbi, equal_many_keys, equal_many_data, 8, ops));
  CHECK(wait_many_result("mdbx_async_get_equal_or_great_many", ops, 8, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < 8; ++i) {
    const uint64_t expected_key = i + (i & 1);
    REQUIRE(op_results[i] == ((i & 1) ? MDBX_RESULT_TRUE : MDBX_SUCCESS),
            "unexpected async equal-or-great many result");
    REQUIRE(equal_many_keys[i].iov_len == sizeof(uint64_t), "unexpected async equal-or-great many key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, equal_many_keys[i].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == expected_key, "unexpected async equal-or-great many key");
    CHECK(expect_value(&equal_many_data[i], expected_key, __FILE__, __LINE__));
  }

  uint8_t missing_key_bytes[sizeof(uint64_t)];
  memset(missing_key_bytes, 0xff, sizeof(missing_key_bytes));
  MDBX_val missing_key = val(missing_key_bytes, sizeof(missing_key_bytes));
  MDBX_val missing_data = val(NULL, 0);
  int missing_result = MDBX_SUCCESS;
  CHECK(mdbx_async_get(async, txn, dbi, &missing_key, &missing_data, &op));
  CHECK(wait_result("mdbx_async_get missing", &op, &missing_result, __FILE__, __LINE__));
  REQUIRE(missing_result == MDBX_NOTFOUND, "async get missing key returned wrong result");
  CHECK(expect_empty_value(&missing_data, __FILE__, __LINE__));

  MDBX_val missing_get_ex_key = missing_key;
  MDBX_val missing_get_ex_data = val(NULL, 0);
  size_t missing_values_count = SIZE_MAX;
  missing_result = MDBX_SUCCESS;
  CHECK(mdbx_async_get_ex(async, txn, dbi, &missing_get_ex_key, &missing_get_ex_data,
                          &missing_values_count, &op));
  CHECK(wait_result("mdbx_async_get_ex missing", &op, &missing_result, __FILE__, __LINE__));
  REQUIRE(missing_result == MDBX_NOTFOUND, "async get_ex missing key returned wrong result");
  REQUIRE(missing_values_count == 0, "async get_ex missing key returned values");
  CHECK(expect_empty_value(&missing_get_ex_data, __FILE__, __LINE__));

  MDBX_val missing_lower_key = missing_key;
  MDBX_val missing_lower_data = val(NULL, 0);
  missing_result = MDBX_SUCCESS;
  CHECK(mdbx_async_get_equal_or_great(async, txn, dbi, &missing_lower_key, &missing_lower_data, &op));
  CHECK(wait_result("mdbx_async_get_equal_or_great missing", &op, &missing_result, __FILE__, __LINE__));
  REQUIRE(missing_result == MDBX_NOTFOUND, "async equal-or-great missing key returned wrong result");
  CHECK(expect_empty_value(&missing_lower_data, __FILE__, __LINE__));

  MDBX_val mixed_keys[3] = {key_values[0], missing_key, key_values[1]};
  MDBX_val mixed_data[3] = {val(NULL, 0), val(NULL, 0), val(NULL, 0)};
  CHECK(mdbx_async_get_many(async, txn, dbi, mixed_keys, mixed_data, 3, ops));
  CHECK(wait_many_result("mdbx_async_get_many mixed", ops, 3, op_results, __FILE__, __LINE__));
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async get many mixed results were wrong");
  CHECK(expect_value(&mixed_data[0], keys[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_data[2], keys[1], __FILE__, __LINE__));

  MDBX_val mixed_get_ex_keys[3] = {key_values[2], missing_key, key_values[3]};
  MDBX_val mixed_get_ex_data[3] = {val(NULL, 0), val(NULL, 0), val(NULL, 0)};
  size_t mixed_get_ex_counts[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  CHECK(mdbx_async_get_ex_many(async, txn, dbi, mixed_get_ex_keys, mixed_get_ex_data,
                               mixed_get_ex_counts, 3, ops));
  CHECK(wait_many_result("mdbx_async_get_ex_many mixed", ops, 3, op_results, __FILE__, __LINE__));
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async get_ex many mixed results were wrong");
  REQUIRE(mixed_get_ex_counts[0] == 1 && mixed_get_ex_counts[1] == 0 &&
              mixed_get_ex_counts[2] == 1,
          "async get_ex many mixed value counts were wrong");
  CHECK(expect_value(&mixed_get_ex_data[0], keys[2], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_get_ex_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_get_ex_data[2], keys[3], __FILE__, __LINE__));

  MDBX_val mixed_lower_keys[3] = {key_values[4], missing_key, key_values[5]};
  MDBX_val mixed_lower_data[3] = {val(NULL, 0), val(NULL, 0), val(NULL, 0)};
  CHECK(mdbx_async_get_equal_or_great_many(async, txn, dbi, mixed_lower_keys, mixed_lower_data, 3,
                                           ops));
  CHECK(wait_many_result("mdbx_async_get_equal_or_great_many mixed", ops, 3, op_results, __FILE__,
                         __LINE__));
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async equal-or-great many mixed results were wrong");
  CHECK(expect_value(&mixed_lower_data[0], keys[4], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_lower_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_lower_data[2], keys[5], __FILE__, __LINE__));

  for (unsigned i = 0; i < ITEM_COUNT; ++i)
    get_values[i] = val(NULL, 0);
  CHECK(mdbx_async_get_batch(async, txn, dbi, key_values, get_values, op_results, ITEM_COUNT, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_get_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < ITEM_COUNT; ++i)
    get_values[i] = val(NULL, 0);
  struct get_batch_probe get_batch_probe = {0, 0};
  CHECK(mdbx_async_get_batch_cb(async, txn, dbi, key_values, get_values, op_results, ITEM_COUNT,
                                get_batch_probe_func, &get_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(get_batch_probe.calls == 1, "async get batch callback was not called");
  REQUIRE(get_batch_probe.successes == ITEM_COUNT, "async get batch callback saw wrong success count");

  mixed_data[0] = val(NULL, 0);
  mixed_data[1] = val(NULL, 0);
  mixed_data[2] = val(NULL, 0);
  CHECK(mdbx_async_get_batch(async, txn, dbi, mixed_keys, mixed_data, op_results, 3, &op));
  CHECK_OP(op);
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async get batch mixed results were wrong");
  CHECK(expect_value(&mixed_data[0], keys[0], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_data[2], keys[1], __FILE__, __LINE__));

  MDBX_val get_ex_batch_keys[ITEM_COUNT];
  size_t get_ex_values_counts[ITEM_COUNT];
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    get_ex_batch_keys[i] = key_values[i];
    get_values[i] = val(NULL, 0);
    get_ex_values_counts[i] = SIZE_MAX;
  }
  CHECK(mdbx_async_get_ex_batch(async, txn, dbi, get_ex_batch_keys, get_values, get_ex_values_counts, op_results,
                                ITEM_COUNT, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_get_ex_batch item", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    REQUIRE(get_ex_values_counts[i] == 1, "async get_ex batch saw wrong value count");
    REQUIRE(get_ex_batch_keys[i].iov_len == sizeof(uint64_t), "unexpected async get_ex batch key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, get_ex_batch_keys[i].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == keys[i], "unexpected async get_ex batch key");
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    get_ex_batch_keys[i] = key_values[i];
    get_values[i] = val(NULL, 0);
    get_ex_values_counts[i] = SIZE_MAX;
  }
  struct get_ex_batch_probe get_ex_batch_probe = {0, 0, 0};
  CHECK(mdbx_async_get_ex_batch_cb(async, txn, dbi, get_ex_batch_keys, get_values, get_ex_values_counts,
                                   op_results, ITEM_COUNT, get_ex_batch_probe_func, &get_ex_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(get_ex_batch_probe.calls == 1, "async get_ex batch callback was not called");
  REQUIRE(get_ex_batch_probe.successes == ITEM_COUNT, "async get_ex batch callback saw wrong success count");
  REQUIRE(get_ex_batch_probe.values == ITEM_COUNT, "async get_ex batch callback saw wrong value count");

  mixed_get_ex_data[0] = val(NULL, 0);
  mixed_get_ex_data[1] = val(NULL, 0);
  mixed_get_ex_data[2] = val(NULL, 0);
  mixed_get_ex_counts[0] = SIZE_MAX;
  mixed_get_ex_counts[1] = SIZE_MAX;
  mixed_get_ex_counts[2] = SIZE_MAX;
  CHECK(mdbx_async_get_ex_batch(async, txn, dbi, mixed_get_ex_keys, mixed_get_ex_data,
                                mixed_get_ex_counts, op_results, 3, &op));
  CHECK_OP(op);
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async get_ex batch mixed results were wrong");
  REQUIRE(mixed_get_ex_counts[0] == 1 && mixed_get_ex_counts[1] == 0 &&
              mixed_get_ex_counts[2] == 1,
          "async get_ex batch mixed value counts were wrong");
  CHECK(expect_value(&mixed_get_ex_data[0], keys[2], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_get_ex_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_get_ex_data[2], keys[3], __FILE__, __LINE__));

  MDBX_val equal_batch_keys[8];
  MDBX_val equal_batch_data[8];
  uint64_t equal_batch_exact_keys[8];
  uint8_t equal_batch_greater_keys[4][sizeof(uint64_t) + 1];
  for (unsigned i = 0; i < 8; ++i) {
    equal_batch_exact_keys[i] = i;
    if (i & 1) {
      memcpy(equal_batch_greater_keys[i / 2], &equal_batch_exact_keys[i], sizeof(equal_batch_exact_keys[i]));
      equal_batch_greater_keys[i / 2][sizeof(equal_batch_exact_keys[i])] = 0;
      equal_batch_keys[i] = val(equal_batch_greater_keys[i / 2], sizeof(equal_batch_greater_keys[i / 2]));
    } else {
      equal_batch_keys[i] = val(&equal_batch_exact_keys[i], sizeof(equal_batch_exact_keys[i]));
    }
    equal_batch_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_get_equal_or_great_batch(async, txn, dbi, equal_batch_keys, equal_batch_data, op_results, 8,
                                            &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 8; ++i) {
    const uint64_t expected_key = i + (i & 1);
    REQUIRE(op_results[i] == ((i & 1) ? MDBX_RESULT_TRUE : MDBX_SUCCESS),
            "unexpected async equal-or-great batch result");
    REQUIRE(equal_batch_keys[i].iov_len == sizeof(uint64_t), "unexpected async equal-or-great batch key size");
    uint64_t actual_key = UINT64_MAX;
    memcpy(&actual_key, equal_batch_keys[i].iov_base, sizeof(actual_key));
    REQUIRE(actual_key == expected_key, "unexpected async equal-or-great batch key");
    CHECK(expect_value(&equal_batch_data[i], expected_key, __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < 8; ++i) {
    equal_batch_exact_keys[i] = i;
    if (i & 1) {
      memcpy(equal_batch_greater_keys[i / 2], &equal_batch_exact_keys[i], sizeof(equal_batch_exact_keys[i]));
      equal_batch_greater_keys[i / 2][sizeof(equal_batch_exact_keys[i])] = 0;
      equal_batch_keys[i] = val(equal_batch_greater_keys[i / 2], sizeof(equal_batch_greater_keys[i / 2]));
    } else {
      equal_batch_keys[i] = val(&equal_batch_exact_keys[i], sizeof(equal_batch_exact_keys[i]));
    }
    equal_batch_data[i] = val(NULL, 0);
  }
  struct get_equal_or_great_batch_probe get_equal_or_great_batch_probe = {0, 0, 0};
  CHECK(mdbx_async_get_equal_or_great_batch_cb(async, txn, dbi, equal_batch_keys, equal_batch_data, op_results, 8,
                                               get_equal_or_great_batch_probe_func,
                                               &get_equal_or_great_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(get_equal_or_great_batch_probe.calls == 1,
          "async equal-or-great batch callback was not called");
  REQUIRE(get_equal_or_great_batch_probe.successes == 8,
          "async equal-or-great batch callback saw wrong success count");
  REQUIRE(get_equal_or_great_batch_probe.greater_results == 4,
          "async equal-or-great batch callback saw wrong greater-result count");

  mixed_lower_data[0] = val(NULL, 0);
  mixed_lower_data[1] = val(NULL, 0);
  mixed_lower_data[2] = val(NULL, 0);
  CHECK(mdbx_async_get_equal_or_great_batch(async, txn, dbi, mixed_lower_keys, mixed_lower_data,
                                            op_results, 3, &op));
  CHECK_OP(op);
  REQUIRE(op_results[0] == MDBX_SUCCESS && op_results[1] == MDBX_NOTFOUND &&
              op_results[2] == MDBX_SUCCESS,
          "async equal-or-great batch mixed results were wrong");
  CHECK(expect_value(&mixed_lower_data[0], keys[4], __FILE__, __LINE__));
  CHECK(expect_empty_value(&mixed_lower_data[1], __FILE__, __LINE__));
  CHECK(expect_value(&mixed_lower_data[2], keys[5], __FILE__, __LINE__));

  struct get_loop_probe get_loop_probe = {0, 0, 0};
  size_t get_loop_completed = 0;
  CHECK(mdbx_async_get_loop(async, txn, dbi, ITEM_COUNT, get_loop_key_func, get_loop_result_func, &get_loop_probe,
                            &get_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(get_loop_completed == ITEM_COUNT, "async get loop completed wrong count");
  REQUIRE(get_loop_probe.keys == ITEM_COUNT, "async get loop prepared wrong key count");
  REQUIRE(get_loop_probe.results == ITEM_COUNT, "async get loop saw wrong result count");

  struct get_ex_loop_probe get_ex_loop_probe = {0, 0, 0, 0};
  size_t get_ex_loop_completed = 0;
  CHECK(mdbx_async_get_ex_loop(async, txn, dbi, ITEM_COUNT, get_ex_loop_key_func, get_ex_loop_result_func,
                               &get_ex_loop_probe, &get_ex_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(get_ex_loop_completed == ITEM_COUNT, "async get_ex loop completed wrong count");
  REQUIRE(get_ex_loop_probe.keys == ITEM_COUNT, "async get_ex loop prepared wrong key count");
  REQUIRE(get_ex_loop_probe.results == ITEM_COUNT, "async get_ex loop saw wrong result count");
  REQUIRE(get_ex_loop_probe.values == ITEM_COUNT, "async get_ex loop saw wrong value count");

  struct get_equal_or_great_loop_probe get_equal_or_great_loop_probe = {0, {0}, 0, 0, 0, 0};
  size_t get_equal_or_great_loop_completed = 0;
  CHECK(mdbx_async_get_equal_or_great_loop(async, txn, dbi, 8, get_equal_or_great_loop_key_func,
                                           get_equal_or_great_loop_data_func,
                                           get_equal_or_great_loop_result_func,
                                           &get_equal_or_great_loop_probe,
                                           &get_equal_or_great_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(get_equal_or_great_loop_completed == 8, "async equal-or-great loop completed wrong count");
  REQUIRE(get_equal_or_great_loop_probe.keys == 8, "async equal-or-great loop prepared wrong key count");
  REQUIRE(get_equal_or_great_loop_probe.data == 8, "async equal-or-great loop prepared wrong data count");
  REQUIRE(get_equal_or_great_loop_probe.results == 8, "async equal-or-great loop saw wrong result count");
  REQUIRE(get_equal_or_great_loop_probe.greater_results == 4,
          "async equal-or-great loop saw wrong greater-result count");

  int dirty_result = MDBX_SUCCESS;
  CHECK(mdbx_async_is_dirty(async, txn, get_values[4].iov_base, &op));
  CHECK(wait_result("mdbx_async_is_dirty", &op, &dirty_result, __FILE__, __LINE__));
  REQUIRE(dirty_result == MDBX_RESULT_FALSE, "clean read value reported dirty");

  int value_comparison = 0;
  CHECK(mdbx_async_cmp(async, txn, dbi, &key_values[1], &key_values[2], &value_comparison, &op));
  CHECK_OP(op);
  REQUIRE(value_comparison < 0, "unexpected async key comparison result");
  value_comparison = 0;
  CHECK(mdbx_async_dcmp(async, txn, dbi, &put_values[1], &put_values[2], &value_comparison, &op));
  CHECK_OP(op);
  REQUIRE(value_comparison < 0, "unexpected async data comparison result");

  MDBX_cache_entry_t cache_entry = {1, 2, 3, 4};
  CHECK(mdbx_async_cache_init(async, &cache_entry, &op));
  CHECK_OP(op);
  REQUIRE(cache_entry.trunk_txnid == 0 && cache_entry.last_confirmed_txnid == 0 &&
              cache_entry.offset == 0 && cache_entry.length == 0,
          "async cache init did not reset the cache entry");
  MDBX_cache_result_t cache_result = {MDBX_SUCCESS, MDBX_CACHE_ERROR};
  MDBX_val cache_data = val(NULL, 0);
  CHECK(mdbx_async_cache_get(async, txn, dbi, &key_values[4], &cache_data, &cache_entry, &cache_result, &op));
  CHECK_OP(op);
  REQUIRE(cache_result.errcode == MDBX_SUCCESS && cache_result.status == MDBX_CACHE_REFRESHED,
          "unexpected async cache get result");
  CHECK(expect_value(&cache_data, keys[4], __FILE__, __LINE__));

  MDBX_cache_result_t cache_hit_result = {MDBX_SUCCESS, MDBX_CACHE_ERROR};
  MDBX_val cache_hit_data = val(NULL, 0);
  CHECK(mdbx_async_cache_get_SingleThreaded(async, txn, dbi, &key_values[4], &cache_hit_data, &cache_entry,
                                            &cache_hit_result, &op));
  CHECK_OP(op);
  REQUIRE(cache_hit_result.errcode == MDBX_SUCCESS && cache_hit_result.status == MDBX_CACHE_HIT,
          "unexpected async single-thread cache get result");
  CHECK(expect_value(&cache_hit_data, keys[4], __FILE__, __LINE__));

  MDBX_cache_entry_t cache_many_entries[4];
  MDBX_cache_result_t cache_many_results[4];
  MDBX_val cache_many_keys[4];
  MDBX_val cache_many_data[4];
  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&cache_many_entries[i]);
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_keys[i] = key_values[i + 2];
    cache_many_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_many(async, txn, dbi, cache_many_keys, cache_many_data, cache_many_entries,
                                  cache_many_results, 4, ops));
  CHECK(wait_many_success("mdbx_async_cache_get_many", ops, 4, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_many_results[i].errcode == MDBX_SUCCESS &&
                cache_many_results[i].status == MDBX_CACHE_REFRESHED,
            "unexpected async cache get many result");
    CHECK(expect_value(&cache_many_data[i], keys[i + 2], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < 4; ++i) {
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_many(async, txn, dbi, cache_many_keys, cache_many_data,
                                                 cache_many_entries, cache_many_results, 4, ops));
  CHECK(wait_many_success("mdbx_async_cache_get_SingleThreaded_many", ops, 4, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_many_results[i].errcode == MDBX_SUCCESS &&
                cache_many_results[i].status == MDBX_CACHE_HIT,
            "unexpected async single-thread cache get many result");
    CHECK(expect_value(&cache_many_data[i], keys[i + 2], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&cache_many_entries[i]);
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_batch(async, txn, dbi, cache_many_keys, cache_many_data,
                                   cache_many_entries, cache_many_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_many_results[i].errcode == MDBX_SUCCESS &&
                cache_many_results[i].status == MDBX_CACHE_REFRESHED,
            "unexpected async cache get batch result");
    CHECK(expect_value(&cache_many_data[i], keys[i + 2], __FILE__, __LINE__));
  }

  for (unsigned i = 0; i < 4; ++i) {
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_batch(async, txn, dbi, cache_many_keys, cache_many_data,
                                                  cache_many_entries, cache_many_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_many_results[i].errcode == MDBX_SUCCESS &&
                cache_many_results[i].status == MDBX_CACHE_HIT,
            "unexpected async single-thread cache get batch result");
    CHECK(expect_value(&cache_many_data[i], keys[i + 2], __FILE__, __LINE__));
  }

  MDBX_cache_entry_t cache_duplicate_entries[4];
  MDBX_cache_result_t cache_duplicate_results[4];
  MDBX_val cache_duplicate_keys[4];
  MDBX_val cache_duplicate_data[4];
  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&cache_duplicate_entries[i]);
    cache_duplicate_results[i].errcode = MDBX_PROBLEM;
    cache_duplicate_results[i].status = MDBX_CACHE_ERROR;
    cache_duplicate_keys[i] = key_values[4];
    cache_duplicate_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_batch(async, txn, dbi, cache_duplicate_keys, cache_duplicate_data,
                                   cache_duplicate_entries, cache_duplicate_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_duplicate_results[i].errcode == MDBX_SUCCESS &&
                cache_duplicate_results[i].status != MDBX_CACHE_ERROR,
            "unexpected async duplicate cache get batch result");
    CHECK(expect_value(&cache_duplicate_data[i], keys[4], __FILE__, __LINE__));
    cache_duplicate_results[i].errcode = MDBX_PROBLEM;
    cache_duplicate_results[i].status = MDBX_CACHE_ERROR;
    cache_duplicate_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_batch(async, txn, dbi, cache_duplicate_keys,
                                                  cache_duplicate_data, cache_duplicate_entries,
                                                  cache_duplicate_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(cache_duplicate_results[i].errcode == MDBX_SUCCESS &&
                cache_duplicate_results[i].status == MDBX_CACHE_HIT,
            "unexpected async duplicate single-thread cache batch result");
    CHECK(expect_value(&cache_duplicate_data[i], keys[4], __FILE__, __LINE__));
  }

  MDBX_cache_entry_t large_cache_entries[LARGE_ITEM_COUNT];
  MDBX_cache_result_t large_cache_results[LARGE_ITEM_COUNT];
  MDBX_val large_cache_data[LARGE_ITEM_COUNT];
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    mdbx_cache_init(&large_cache_entries[i]);
    large_cache_results[i].errcode = MDBX_PROBLEM;
    large_cache_results[i].status = MDBX_CACHE_ERROR;
    large_cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_batch(async, txn, large_dbi, large_key_values, large_cache_data,
                                   large_cache_entries, large_cache_results,
                                   LARGE_ITEM_COUNT, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    REQUIRE(large_cache_results[i].errcode == MDBX_SUCCESS &&
                large_cache_results[i].status != MDBX_CACHE_ERROR,
            "unexpected async large cache get batch result");
    CHECK(expect_large_value(&large_cache_data[i], large_keys[i], __FILE__, __LINE__));
  }
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    large_cache_results[i].errcode = MDBX_PROBLEM;
    large_cache_results[i].status = MDBX_CACHE_ERROR;
    large_cache_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_batch(async, txn, large_dbi, large_key_values,
                                                  large_cache_data, large_cache_entries,
                                                  large_cache_results, LARGE_ITEM_COUNT, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    REQUIRE(large_cache_results[i].errcode == MDBX_SUCCESS &&
                large_cache_results[i].status == MDBX_CACHE_HIT,
            "unexpected async large single-thread cache get batch result");
    CHECK(expect_large_value(&large_cache_data[i], large_keys[i], __FILE__, __LINE__));
  }

  MDBX_cache_entry_t large_duplicate_entries[4];
  MDBX_cache_result_t large_duplicate_results[4];
  MDBX_val large_duplicate_keys[4];
  MDBX_val large_duplicate_data[4];
  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&large_duplicate_entries[i]);
    large_duplicate_results[i].errcode = MDBX_PROBLEM;
    large_duplicate_results[i].status = MDBX_CACHE_ERROR;
    large_duplicate_keys[i] = large_key_values[1];
    large_duplicate_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_batch(async, txn, large_dbi, large_duplicate_keys,
                                   large_duplicate_data, large_duplicate_entries,
                                   large_duplicate_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(large_duplicate_results[i].errcode == MDBX_SUCCESS &&
                large_duplicate_results[i].status != MDBX_CACHE_ERROR,
            "unexpected async duplicate large cache get batch result");
    CHECK(expect_large_value(&large_duplicate_data[i], large_keys[1], __FILE__, __LINE__));
    large_duplicate_results[i].errcode = MDBX_PROBLEM;
    large_duplicate_results[i].status = MDBX_CACHE_ERROR;
    large_duplicate_data[i] = val(NULL, 0);
  }
  CHECK(mdbx_async_cache_get_SingleThreaded_batch(async, txn, large_dbi,
                                                  large_duplicate_keys, large_duplicate_data,
                                                  large_duplicate_entries,
                                                  large_duplicate_results, 4, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(large_duplicate_results[i].errcode == MDBX_SUCCESS &&
                large_duplicate_results[i].status == MDBX_CACHE_HIT,
            "unexpected async duplicate large single-thread cache batch result");
    CHECK(expect_large_value(&large_duplicate_data[i], large_keys[1], __FILE__, __LINE__));
  }

  CHECK(mdbx_async_cursor_open(async, txn, large_dbi, &cursor, &op));
  CHECK_OP(op);
  MDBX_val large_cursor_pairs[LARGE_ITEM_COUNT * 2];
  size_t large_cursor_count = 0;
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &large_cursor_count, large_cursor_pairs,
                                    LARGE_ITEM_COUNT * 2, MDBX_FIRST, &op));
  int large_cursor_result = MDBX_SUCCESS;
  CHECK(wait_result("mdbx_async_cursor_get_batch large", &op, &large_cursor_result, __FILE__, __LINE__));
  REQUIRE(large_cursor_result == MDBX_SUCCESS || large_cursor_result == MDBX_RESULT_TRUE,
          "unexpected async large cursor batch result");
  REQUIRE(large_cursor_count == LARGE_ITEM_COUNT * 2, "unexpected async large cursor batch count");
  for (unsigned i = 0; i < LARGE_ITEM_COUNT; ++i) {
    MDBX_val *const key = &large_cursor_pairs[i * 2];
    MDBX_val *const data = &large_cursor_pairs[i * 2 + 1];
    REQUIRE(key->iov_len == sizeof(uint64_t), "unexpected async large cursor key size");
    uint64_t actual_key = 0;
    memcpy(&actual_key, key->iov_base, sizeof(actual_key));
    REQUIRE(actual_key == large_keys[i], "unexpected async large cursor key");
    CHECK(expect_large_value(data, large_keys[i], __FILE__, __LINE__));
  }
  MDBX_val large_cursor_seek_key = large_key_values[1];
  MDBX_val large_cursor_seek_data = val(NULL, 0);
  int large_cursor_seek_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_get(async, cursor, &large_cursor_seek_key,
                              &large_cursor_seek_data, MDBX_SET_KEY, &op));
  CHECK(wait_result("mdbx_async_cursor_get large set-key", &op,
                    &large_cursor_seek_result, __FILE__, __LINE__));
  REQUIRE(large_cursor_seek_result == MDBX_SUCCESS,
          "unexpected async large cursor set-key result");
  REQUIRE(large_cursor_seek_key.iov_len == sizeof(uint64_t),
          "unexpected async large cursor set-key key size");
  uint64_t large_cursor_seek_actual = 0;
  memcpy(&large_cursor_seek_actual, large_cursor_seek_key.iov_base,
         sizeof(large_cursor_seek_actual));
  REQUIRE(large_cursor_seek_actual == large_keys[1],
          "unexpected async large cursor set-key key");
  CHECK(expect_large_value(&large_cursor_seek_data, large_keys[1], __FILE__, __LINE__));
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_drop(async, txn, large_dbi, true, &op));
  CHECK_OP(op);
  large_dbi = 0;
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;
  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);

  for (unsigned i = 0; i < 4; ++i) {
    mdbx_cache_init(&cache_many_entries[i]);
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_data[i] = val(NULL, 0);
  }
  struct cache_batch_probe cache_batch_probe = {0, 0, 0};
  CHECK(mdbx_async_cache_get_batch_cb(async, txn, dbi, cache_many_keys, cache_many_data,
                                      cache_many_entries, cache_many_results, 4,
                                      cache_batch_probe_func, &cache_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(cache_batch_probe.calls == 1, "async cache batch callback was not called");
  REQUIRE(cache_batch_probe.successes == 4, "async cache batch callback saw wrong success count");

  for (unsigned i = 0; i < 4; ++i) {
    cache_many_results[i].errcode = MDBX_PROBLEM;
    cache_many_results[i].status = MDBX_CACHE_ERROR;
    cache_many_data[i] = val(NULL, 0);
  }
  cache_batch_probe.calls = 0;
  cache_batch_probe.successes = 0;
  cache_batch_probe.hits = 0;
  CHECK(mdbx_async_cache_get_SingleThreaded_batch_cb(async, txn, dbi, cache_many_keys, cache_many_data,
                                                     cache_many_entries, cache_many_results, 4,
                                                     cache_batch_probe_func, &cache_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(cache_batch_probe.calls == 1, "async single-thread cache batch callback was not called");
  REQUIRE(cache_batch_probe.successes == 4 && cache_batch_probe.hits == 4,
          "async single-thread cache batch callback did not hit all items");

  for (unsigned i = 0; i < 4; ++i)
    mdbx_cache_init(&cache_many_entries[i]);
  struct cache_loop_probe cache_loop_probe = {0, 2, 0, 0, 0};
  size_t cache_loop_completed = 0;
  CHECK(mdbx_async_cache_get_loop(async, txn, dbi, 4, cache_loop_key_func, cache_many_entries,
                                  cache_loop_result_func, &cache_loop_probe, &cache_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(cache_loop_completed == 4, "unexpected async cache loop completion count");
  REQUIRE(cache_loop_probe.keys == 4 && cache_loop_probe.results == 4,
          "async cache loop did not process all items");

  cache_loop_probe.key = 0;
  cache_loop_probe.keys = 0;
  cache_loop_probe.results = 0;
  cache_loop_probe.hits = 0;
  cache_loop_completed = 0;
  CHECK(mdbx_async_cache_get_SingleThreaded_loop(async, txn, dbi, 4, cache_loop_key_func, cache_many_entries,
                                                 cache_loop_result_func, &cache_loop_probe,
                                                 &cache_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(cache_loop_completed == 4, "unexpected async single-thread cache loop completion count");
  REQUIRE(cache_loop_probe.keys == 4 && cache_loop_probe.results == 4 && cache_loop_probe.hits == 4,
          "async single-thread cache loop did not hit all items");

  CHECK(mdbx_async_txn_copy2pathname(async, txn, copy_txn_path, MDBX_CP_COMPACT | MDBX_CP_DONT_FLUSH, &op));
  CHECK_OP(op);
  CHECK(verify_copied_value(copy_txn_path, keys[8], __FILE__, __LINE__));

  int userctx_a = 1;
  int userctx_b = 2;
  void *cursor_context = NULL;
  MDBX_cursor *utility_cursor = NULL;
  CHECK(mdbx_async_cursor_create(async, &userctx_a, &utility_cursor, &op));
  CHECK_OP(op);
  REQUIRE(utility_cursor != NULL, "async cursor create returned null");
  CHECK(mdbx_async_cursor_get_userctx(async, utility_cursor, &cursor_context, &op));
  CHECK_OP(op);
  REQUIRE(cursor_context == &userctx_a, "unexpected async cursor initial context");
  CHECK(mdbx_async_cursor_set_userctx(async, utility_cursor, &userctx_b, &op));
  CHECK_OP(op);
  cursor_context = NULL;
  CHECK(mdbx_async_cursor_get_userctx(async, utility_cursor, &cursor_context, &op));
  CHECK_OP(op);
  REQUIRE(cursor_context == &userctx_b, "unexpected async cursor updated context");
  CHECK(mdbx_async_cursor_bind(async, txn, utility_cursor, dbi, &op));
  CHECK_OP(op);
  MDBX_txn *utility_txn = NULL;
  CHECK(mdbx_async_cursor_txn(async, utility_cursor, &utility_txn, &op));
  CHECK_OP(op);
  REQUIRE(utility_txn == txn, "unexpected async cursor transaction");
  MDBX_dbi utility_dbi = UINT32_MAX;
  CHECK(mdbx_async_cursor_dbi(async, utility_cursor, &utility_dbi, &op));
  CHECK_OP(op);
  REQUIRE(utility_dbi == dbi, "unexpected async cursor dbi");
  CHECK(mdbx_async_cursor_ignord(async, utility_cursor, &op));
  CHECK_OP(op);
  MDBX_val utility_key = val(NULL, 0);
  MDBX_val utility_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, utility_cursor, &utility_key, &utility_data, MDBX_FIRST, &op));
  CHECK_OP(op);
  CHECK(expect_value(&utility_data, 0, __FILE__, __LINE__));
  MDBX_cursor *copy_cursor = NULL;
  CHECK(mdbx_async_cursor_create(async, NULL, &copy_cursor, &op));
  CHECK_OP(op);
  REQUIRE(copy_cursor != NULL, "async copy cursor create returned null");
  CHECK(mdbx_async_cursor_copy(async, utility_cursor, copy_cursor, &op));
  CHECK_OP(op);
  int cursor_comparison = INT_MIN;
  CHECK(mdbx_async_cursor_compare(async, utility_cursor, copy_cursor, false, &cursor_comparison, &op));
  CHECK_OP(op);
  REQUIRE(cursor_comparison == 0, "copied cursor position differs");
  CHECK(mdbx_async_cursor_get(async, utility_cursor, &utility_key, &utility_data, MDBX_NEXT, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_compare(async, utility_cursor, copy_cursor, false, &cursor_comparison, &op));
  CHECK_OP(op);
  REQUIRE(cursor_comparison > 0, "advanced cursor did not compare after copy");
  CHECK(mdbx_async_cursor_unbind(async, copy_cursor, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_close2(async, copy_cursor, &op));
  CHECK_OP(op);
  copy_cursor = NULL;
  CHECK(mdbx_async_cursor_unbind(async, utility_cursor, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_close(async, utility_cursor, &op));
  CHECK_OP(op);
  utility_cursor = NULL;

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  REQUIRE(cursor != NULL, "cursor was not returned");

  unsigned seen = 0;
  MDBX_val cursor_key = val(NULL, 0);
  MDBX_val cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_FIRST, &op));
  CHECK_OP(op);
  REQUIRE(cursor_key.iov_len == sizeof(uint64_t), "unexpected cursor key size before reset");
  uint64_t first_key = 0;
  memcpy(&first_key, cursor_key.iov_base, sizeof(first_key));
  REQUIRE(first_key == 0, "unexpected cursor key before reset");
  CHECK(expect_value(&cursor_data, first_key, __FILE__, __LINE__));

  size_t cursor_count = 0;
  CHECK(mdbx_async_cursor_count(async, cursor, &cursor_count, &op));
  CHECK_OP(op);
  REQUIRE(cursor_count == 1, "unexpected cursor count");

  MDBX_stat cursor_stat;
  memset(&cursor_stat, 0, sizeof(cursor_stat));
  cursor_count = 0;
  CHECK(mdbx_async_cursor_count_ex(async, cursor, &cursor_count, &cursor_stat, sizeof(cursor_stat), &op));
  CHECK_OP(op);
  REQUIRE(cursor_count == 1, "unexpected cursor count_ex count");

  int cursor_state = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_eof(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_eof", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_FALSE, "cursor unexpectedly at eof");
  CHECK(mdbx_async_cursor_on_first(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_first", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_TRUE, "cursor not on first item");
  CHECK(mdbx_async_cursor_on_last(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_last", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_FALSE, "cursor unexpectedly on last item");
  CHECK(mdbx_async_cursor_on_first_dup(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_first_dup", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_TRUE, "cursor not on first dup");
  CHECK(mdbx_async_cursor_on_last_dup(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_last_dup", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_TRUE, "cursor not on last dup");

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor2, &op));
  CHECK_OP(op);
  REQUIRE(cursor2 != NULL, "second read cursor was not returned");
  MDBX_val cursor2_key = val(NULL, 0);
  MDBX_val cursor2_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor2, &cursor2_key, &cursor2_data, MDBX_LAST, &op));
  CHECK_OP(op);

  intptr_t cursor_distance = 0;
  CHECK(mdbx_async_cursor_distance(async, cursor, cursor2, &cursor_distance, 42, &op));
  CHECK_OP(op);
  REQUIRE(cursor_distance == (intptr_t)ITEM_COUNT - 1, "unexpected async cursor distance");

  ptrdiff_t estimated_distance = 0;
  CHECK(mdbx_async_estimate_distance(async, cursor, cursor2, &estimated_distance, &op));
  CHECK_OP(op);
  REQUIRE(estimated_distance >= 0 && estimated_distance <= (ptrdiff_t)ITEM_COUNT,
          "unexpected async estimate distance");

  MDBX_val estimate_key = val(NULL, 0);
  MDBX_val estimate_data = val(NULL, 0);
  ptrdiff_t estimated_move = 0;
  CHECK(mdbx_async_estimate_move(async, cursor, &estimate_key, &estimate_data, MDBX_LAST, &estimated_move, &op));
  CHECK_OP(op);
  REQUIRE(estimated_move >= 0 && estimated_move <= (ptrdiff_t)ITEM_COUNT, "unexpected async estimate move");
  REQUIRE(estimate_key.iov_len == sizeof(uint64_t), "unexpected async estimate move key size");
  uint64_t estimate_actual_key = 0;
  memcpy(&estimate_actual_key, estimate_key.iov_base, sizeof(estimate_actual_key));
  REQUIRE(estimate_actual_key == ITEM_COUNT - 1, "unexpected async estimate move key");
  CHECK(expect_value(&estimate_data, estimate_actual_key, __FILE__, __LINE__));

  ptrdiff_t estimated_range = -1;
  CHECK(mdbx_async_estimate_range(async, txn, dbi, NULL, NULL, NULL, NULL, &estimated_range, &op));
  CHECK_OP(op);
  REQUIRE(estimated_range == (ptrdiff_t)ITEM_COUNT, "unexpected async full-range estimate");

  estimated_range = 0;
  CHECK(mdbx_async_estimate_range(async, txn, dbi, &key_values[10], NULL, &key_values[20], NULL,
                                  &estimated_range, &op));
  CHECK_OP(op);
  REQUIRE(estimated_range > 0 && estimated_range <= (ptrdiff_t)ITEM_COUNT, "unexpected async bounded estimate");

  estimated_range = 0;
  CHECK(mdbx_async_estimate_range(async, txn, dbi, &key_values[5], NULL, MDBX_EPSILON, NULL, &estimated_range, &op));
  CHECK_OP(op);
  REQUIRE(estimated_range == 1, "unexpected async epsilon estimate");

  MDBX_cursor *distribution[3] = {NULL, NULL, NULL};
  const size_t distribution_count = sizeof(distribution) / sizeof(distribution[0]);
  for (size_t i = 0; i < distribution_count; ++i) {
    CHECK(mdbx_async_cursor_open(async, txn, dbi, &distribution[i], &op));
    CHECK_OP(op);
    REQUIRE(distribution[i] != NULL, "distribution cursor was not returned");
  }
  CHECK(mdbx_async_cursor_distribute(async, cursor, cursor2, distribution, (intptr_t)distribution_count, 42, &op));
  CHECK_OP(op);
  for (size_t i = 0; i < distribution_count; ++i) {
    MDBX_val distributed_key = val(NULL, 0);
    MDBX_val distributed_data = val(NULL, 0);
    CHECK(mdbx_async_cursor_get(async, distribution[i], &distributed_key, &distributed_data, MDBX_GET_CURRENT, &op));
    CHECK_OP(op);
    REQUIRE(distributed_key.iov_len == sizeof(uint64_t), "unexpected distributed cursor key size");
    uint64_t actual_key = 0;
    memcpy(&actual_key, distributed_key.iov_base, sizeof(actual_key));
    REQUIRE(actual_key == (uint64_t)((i + 1) * (ITEM_COUNT - 1) / distribution_count),
            "unexpected distributed cursor key");
    CHECK(expect_value(&distributed_data, actual_key, __FILE__, __LINE__));
    CHECK(mdbx_async_cursor_close(async, distribution[i], &op));
    CHECK_OP(op);
    distribution[i] = NULL;
  }

  CHECK(mdbx_async_cursor_scroll(async, cursor, 5, 42, &op));
  CHECK_OP(op);
  cursor_key = val(NULL, 0);
  cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_GET_CURRENT, &op));
  CHECK_OP(op);
  REQUIRE(cursor_key.iov_len == sizeof(uint64_t), "unexpected scrolled cursor key size");
  uint64_t scrolled_key = 0;
  memcpy(&scrolled_key, cursor_key.iov_base, sizeof(scrolled_key));
  REQUIRE(scrolled_key == 5, "async cursor scroll landed on wrong key");
  CHECK(expect_value(&cursor_data, scrolled_key, __FILE__, __LINE__));

  cursor_distance = 0;
  CHECK(mdbx_async_cursor_distance(async, cursor, cursor2, &cursor_distance, 42, &op));
  CHECK_OP(op);
  REQUIRE(cursor_distance == (intptr_t)ITEM_COUNT - 6, "unexpected async cursor distance after scroll");
  CHECK(mdbx_async_cursor_close(async, cursor2, &op));
  CHECK_OP(op);
  cursor2 = NULL;

  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_LAST, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_on_first(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_first last", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_FALSE, "last cursor unexpectedly on first item");
  CHECK(mdbx_async_cursor_on_last(async, cursor, &op));
  CHECK(wait_result("mdbx_async_cursor_on_last last", &op, &cursor_state, __FILE__, __LINE__));
  REQUIRE(cursor_state == MDBX_RESULT_TRUE, "cursor not on last item");

  CHECK(mdbx_async_cursor_reset(async, cursor, &op));
  CHECK_OP(op);

  MDBX_val chunk_pairs[8];
  size_t chunk_count = 0;
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &chunk_count, chunk_pairs, 8, MDBX_FIRST, &op));
  CHECK_OP(op);
  REQUIRE(chunk_count == 8, "unexpected cursor batch chunk count");
  for (unsigned i = 0; i < chunk_count / 2; ++i) {
    REQUIRE(chunk_pairs[i * 2].iov_len == sizeof(uint64_t), "unexpected cursor batch key size");
    uint64_t batch_key = 0;
    memcpy(&batch_key, chunk_pairs[i * 2].iov_base, sizeof(batch_key));
    REQUIRE(batch_key == i, "unexpected cursor batch key");
    CHECK(expect_value(&chunk_pairs[i * 2 + 1], batch_key, __FILE__, __LINE__));
  }

  CHECK(mdbx_async_cursor_reset(async, cursor, &op));
  CHECK_OP(op);

  MDBX_val all_pairs[ITEM_COUNT * 2];
  size_t all_count = 0;
  int batch_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_get_batch(async, cursor, &all_count, all_pairs, ITEM_COUNT * 2, MDBX_FIRST, &op));
  CHECK(wait_result("mdbx_async_cursor_get_batch all", &op, &batch_result, __FILE__, __LINE__));
  REQUIRE(batch_result == MDBX_RESULT_TRUE, "cursor batch all did not report end of data");
  REQUIRE(all_count == ITEM_COUNT * 2, "unexpected cursor batch all count");
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    REQUIRE(all_pairs[i * 2].iov_len == sizeof(uint64_t), "unexpected cursor batch all key size");
    uint64_t batch_key = 0;
    memcpy(&batch_key, all_pairs[i * 2].iov_base, sizeof(batch_key));
    REQUIRE(batch_key == keys[i], "unexpected cursor batch all key");
    CHECK(expect_value(&all_pairs[i * 2 + 1], batch_key, __FILE__, __LINE__));
  }

  struct batch_probe batch_probe = {0, 0};
  size_t loop_pairs = 0;
  CHECK(mdbx_async_cursor_get_batches(async, cursor, ITEM_COUNT + 5, 8, batch_probe_func, &batch_probe,
                                      &loop_pairs, &op));
  CHECK_OP(op);
  REQUIRE(loop_pairs >= ITEM_COUNT + 5, "async cursor batch loop consumed too few pairs");
  REQUIRE(batch_probe.pairs == loop_pairs, "async cursor batch loop probe mismatch");
  REQUIRE(batch_probe.calls >= 2, "async cursor batch loop did not restart");

  uint64_t batch_from_key_data = 18;
  MDBX_val batch_from_key = val(&batch_from_key_data, sizeof(batch_from_key_data));
  MDBX_val batch_from_data = val(NULL, 0);
  struct batch_probe batch_from_probe = {0, 0};
  size_t batch_from_pairs = 0;
  CHECK(mdbx_async_cursor_get_batches_from(async, cursor, 10, 4, MDBX_SET_LOWERBOUND,
                                           &batch_from_key, &batch_from_data, batch_probe_func,
                                           &batch_from_probe, &batch_from_pairs, &op));
  CHECK_OP(op);
  REQUIRE(batch_from_pairs >= 10, "async cursor batch loop-from consumed too few pairs");
  REQUIRE(batch_from_probe.pairs == batch_from_pairs, "async cursor batch loop-from probe mismatch");
  REQUIRE(batch_from_key.iov_len == sizeof(uint64_t), "unexpected cursor batch loop-from key size");
  uint64_t batch_from_actual_key = 0;
  memcpy(&batch_from_actual_key, batch_from_key.iov_base, sizeof(batch_from_actual_key));
  REQUIRE(batch_from_actual_key == batch_from_key_data + batch_from_pairs - 1,
          "unexpected cursor batch loop-from key");
  CHECK(expect_value(&batch_from_data, batch_from_actual_key, __FILE__, __LINE__));

  uint64_t batch_setkey_data = 19;
  MDBX_val batch_setkey_key = val(&batch_setkey_data, sizeof(batch_setkey_data));
  MDBX_val batch_setkey_value = val(NULL, 0);
  struct batch_probe batch_setkey_probe = {0, 0};
  size_t batch_setkey_pairs = 0;
  CHECK(mdbx_async_cursor_get_batches_from(async, cursor, 3, 2, MDBX_SET_KEY,
                                           &batch_setkey_key, &batch_setkey_value,
                                           batch_probe_func, &batch_setkey_probe,
                                           &batch_setkey_pairs, &op));
  CHECK_OP(op);
  REQUIRE(batch_setkey_pairs >= 3, "async cursor batch set-key consumed too few pairs");
  REQUIRE(batch_setkey_probe.pairs == batch_setkey_pairs,
          "async cursor batch set-key probe mismatch");
  REQUIRE(batch_setkey_key.iov_len == sizeof(uint64_t),
          "unexpected cursor batch set-key key size");
  uint64_t batch_setkey_actual_key = 0;
  memcpy(&batch_setkey_actual_key, batch_setkey_key.iov_base,
         sizeof(batch_setkey_actual_key));
  REQUIRE(batch_setkey_actual_key == batch_setkey_data + batch_setkey_pairs - 1,
          "unexpected cursor batch set-key key");
  CHECK(expect_value(&batch_setkey_value, batch_setkey_actual_key, __FILE__, __LINE__));

  uint64_t batch_lowerbound_seed = 18;
  uint8_t batch_lowerbound_bytes[sizeof(batch_lowerbound_seed) + 1];
  memcpy(batch_lowerbound_bytes, &batch_lowerbound_seed, sizeof(batch_lowerbound_seed));
  batch_lowerbound_bytes[sizeof(batch_lowerbound_seed)] = 0;
  MDBX_val batch_lowerbound_key = val(batch_lowerbound_bytes, sizeof(batch_lowerbound_bytes));
  MDBX_val batch_lowerbound_data = val(NULL, 0);
  struct batch_probe batch_lowerbound_probe = {0, 0};
  size_t batch_lowerbound_pairs = 0;
  CHECK(mdbx_async_cursor_get_batches_from(async, cursor, 1, 2, MDBX_SET_LOWERBOUND,
                                           &batch_lowerbound_key, &batch_lowerbound_data,
                                           batch_probe_func, &batch_lowerbound_probe,
                                           &batch_lowerbound_pairs, &op));
  CHECK_OP(op);
  REQUIRE(batch_lowerbound_pairs >= 1, "async cursor batch lower-bound consumed too few pairs");
  REQUIRE(batch_lowerbound_probe.pairs == batch_lowerbound_pairs,
          "async cursor batch lower-bound probe mismatch");
  REQUIRE(batch_lowerbound_key.iov_len == sizeof(uint64_t),
          "unexpected cursor batch lower-bound key size");
  uint64_t batch_lowerbound_actual_key = 0;
  memcpy(&batch_lowerbound_actual_key, batch_lowerbound_key.iov_base, sizeof(batch_lowerbound_actual_key));
  REQUIRE(batch_lowerbound_actual_key > batch_lowerbound_seed &&
              batch_lowerbound_actual_key < ITEM_COUNT,
          "unexpected cursor batch lower-bound key");
  CHECK(expect_value(&batch_lowerbound_data, batch_lowerbound_actual_key, __FILE__, __LINE__));

  CHECK(mdbx_async_cursor_reset(async, cursor, &op));
  CHECK_OP(op);
  struct cursor_get_loop_probe cursor_get_loop_one_probe = {0};
  size_t cursor_get_loop_one_completed = 0;
  CHECK(mdbx_async_cursor_get_loop(async, cursor, 1, MDBX_FIRST, MDBX_NEXT,
                                   cursor_get_loop_probe_func, &cursor_get_loop_one_probe,
                                   &cursor_get_loop_one_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_get_loop_one_completed == 1, "unexpected one-item async cursor get loop count");
  REQUIRE(cursor_get_loop_one_probe.calls == 1, "one-item async cursor get loop probe mismatch");

  struct cursor_get_loop_probe cursor_get_loop_probe = {0};
  size_t cursor_get_loop_completed = 0;
  CHECK(mdbx_async_cursor_get_loop(async, cursor, ITEM_COUNT, MDBX_FIRST, MDBX_NEXT,
                                   cursor_get_loop_probe_func, &cursor_get_loop_probe,
                                   &cursor_get_loop_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_get_loop_completed == ITEM_COUNT, "unexpected async cursor get loop count");
  REQUIRE(cursor_get_loop_probe.calls == ITEM_COUNT, "async cursor get loop probe mismatch");

  struct cursor_get_loop_probe cursor_get_loop_repeat_probe = {0};
  size_t cursor_get_loop_repeat_completed = 0;
  CHECK(mdbx_async_cursor_get_loop(async, cursor, ITEM_COUNT, MDBX_FIRST, MDBX_NEXT,
                                   cursor_get_loop_probe_func, &cursor_get_loop_repeat_probe,
                                   &cursor_get_loop_repeat_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_get_loop_repeat_completed == ITEM_COUNT,
          "unexpected repeated async cursor get loop count");
  REQUIRE(cursor_get_loop_repeat_probe.calls == ITEM_COUNT,
          "repeated async cursor get loop probe mismatch");

  struct cursor_get_loop_abort_probe cursor_get_loop_abort_probe = {0, 2, MDBX_EINTR};
  size_t cursor_get_loop_abort_completed = SIZE_MAX;
  int cursor_get_loop_abort_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_get_loop(async, cursor, ITEM_COUNT, MDBX_FIRST, MDBX_NEXT,
                                   cursor_get_loop_abort_probe_func,
                                   &cursor_get_loop_abort_probe,
                                   &cursor_get_loop_abort_completed, &op));
  CHECK(wait_result("mdbx_async_cursor_get_loop callback abort", &op,
                    &cursor_get_loop_abort_result, __FILE__, __LINE__));
  REQUIRE(cursor_get_loop_abort_result == MDBX_EINTR,
          "async cursor get loop did not propagate callback abort");
  REQUIRE(cursor_get_loop_abort_completed == cursor_get_loop_abort_probe.fail_index,
          "async cursor get loop completed count included aborted item");
  REQUIRE(cursor_get_loop_abort_probe.calls == cursor_get_loop_abort_probe.fail_index + 1,
          "async cursor get loop abort callback count mismatch");

  uint64_t cursor_loop_from_key_data = 18;
  MDBX_val cursor_loop_from_key = val(&cursor_loop_from_key_data, sizeof(cursor_loop_from_key_data));
  MDBX_val cursor_loop_from_data = val(NULL, 0);
  struct cursor_get_loop_probe cursor_get_loop_from_probe = {0};
  size_t cursor_get_loop_from_completed = 0;
  CHECK(mdbx_async_cursor_get_loop_from(async, cursor, 4, MDBX_SET_LOWERBOUND,
                                        &cursor_loop_from_key, &cursor_loop_from_data, MDBX_NEXT,
                                        cursor_get_loop_probe_func, &cursor_get_loop_from_probe,
                                        &cursor_get_loop_from_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_get_loop_from_completed == 4, "unexpected async cursor get loop-from count");
  REQUIRE(cursor_get_loop_from_probe.calls == 4, "async cursor get loop-from probe mismatch");
  REQUIRE(cursor_loop_from_key.iov_len == sizeof(uint64_t), "unexpected cursor get loop-from key size");
  uint64_t cursor_loop_from_actual_key = 0;
  memcpy(&cursor_loop_from_actual_key, cursor_loop_from_key.iov_base, sizeof(cursor_loop_from_actual_key));
  REQUIRE(cursor_loop_from_actual_key == cursor_loop_from_key_data + 3, "unexpected cursor get loop-from key");
  CHECK(expect_value(&cursor_loop_from_data, cursor_loop_from_actual_key, __FILE__, __LINE__));

  uint64_t cursor_loop_setkey_data = 19;
  MDBX_val cursor_loop_setkey_key = val(&cursor_loop_setkey_data, sizeof(cursor_loop_setkey_data));
  MDBX_val cursor_loop_setkey_value = val(NULL, 0);
  struct cursor_get_loop_probe cursor_loop_setkey_probe = {0};
  size_t cursor_loop_setkey_completed = 0;
  CHECK(mdbx_async_cursor_get_loop_from(async, cursor, 3, MDBX_SET_KEY,
                                        &cursor_loop_setkey_key, &cursor_loop_setkey_value,
                                        MDBX_NEXT, cursor_get_loop_probe_func,
                                        &cursor_loop_setkey_probe,
                                        &cursor_loop_setkey_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_loop_setkey_completed == 3, "unexpected async cursor get loop set-key count");
  REQUIRE(cursor_loop_setkey_probe.calls == 3, "async cursor get loop set-key probe mismatch");
  REQUIRE(cursor_loop_setkey_key.iov_len == sizeof(uint64_t),
          "unexpected cursor get loop set-key key size");
  uint64_t cursor_loop_setkey_actual = 0;
  memcpy(&cursor_loop_setkey_actual, cursor_loop_setkey_key.iov_base,
         sizeof(cursor_loop_setkey_actual));
  REQUIRE(cursor_loop_setkey_actual == cursor_loop_setkey_data + 2,
          "unexpected cursor get loop set-key key");
  CHECK(expect_value(&cursor_loop_setkey_value, cursor_loop_setkey_actual,
                     __FILE__, __LINE__));

  uint64_t cursor_get_setkey_data = 16;
  MDBX_val cursor_get_setkey_key = val(&cursor_get_setkey_data, sizeof(cursor_get_setkey_data));
  MDBX_val cursor_get_setkey_value = val(NULL, 0);
  int cursor_get_setkey_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_get_setkey_key,
                              &cursor_get_setkey_value, MDBX_SET_KEY, &op));
  CHECK(wait_result("mdbx_async_cursor_get set-key", &op,
                    &cursor_get_setkey_result, __FILE__, __LINE__));
  REQUIRE(cursor_get_setkey_result == MDBX_SUCCESS,
          "unexpected async cursor get set-key result");
  REQUIRE(cursor_get_setkey_key.iov_len == sizeof(uint64_t),
          "unexpected cursor get set-key key size");
  uint64_t cursor_get_setkey_actual = 0;
  memcpy(&cursor_get_setkey_actual, cursor_get_setkey_key.iov_base,
         sizeof(cursor_get_setkey_actual));
  REQUIRE(cursor_get_setkey_actual == cursor_get_setkey_data,
          "unexpected cursor get set-key key");
  CHECK(expect_value(&cursor_get_setkey_value, cursor_get_setkey_actual,
                     __FILE__, __LINE__));

  uint64_t cursor_get_lowerbound_seed = 17;
  uint8_t cursor_get_lowerbound_bytes[sizeof(cursor_get_lowerbound_seed) + 1];
  memcpy(cursor_get_lowerbound_bytes, &cursor_get_lowerbound_seed, sizeof(cursor_get_lowerbound_seed));
  cursor_get_lowerbound_bytes[sizeof(cursor_get_lowerbound_seed)] = 0;
  MDBX_val cursor_get_lowerbound_key =
      val(cursor_get_lowerbound_bytes, sizeof(cursor_get_lowerbound_bytes));
  MDBX_val cursor_get_lowerbound_data = val(NULL, 0);
  int cursor_get_lowerbound_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_get_lowerbound_key,
                              &cursor_get_lowerbound_data, MDBX_SET_LOWERBOUND, &op));
  CHECK(wait_result("mdbx_async_cursor_get lowerbound", &op,
                    &cursor_get_lowerbound_result, __FILE__, __LINE__));
  REQUIRE(cursor_get_lowerbound_result == MDBX_RESULT_TRUE,
          "unexpected async cursor get lower-bound result");
  REQUIRE(cursor_get_lowerbound_key.iov_len == sizeof(uint64_t),
          "unexpected cursor get lower-bound key size");
  uint64_t cursor_get_lowerbound_actual_key = 0;
  memcpy(&cursor_get_lowerbound_actual_key, cursor_get_lowerbound_key.iov_base,
         sizeof(cursor_get_lowerbound_actual_key));
  REQUIRE(cursor_get_lowerbound_actual_key > cursor_get_lowerbound_seed &&
              cursor_get_lowerbound_actual_key < ITEM_COUNT,
          "unexpected cursor get lower-bound key");
  CHECK(expect_value(&cursor_get_lowerbound_data, cursor_get_lowerbound_actual_key,
                     __FILE__, __LINE__));

  uint64_t cursor_lowerbound_seed = 18;
  uint8_t cursor_lowerbound_bytes[sizeof(cursor_lowerbound_seed) + 1];
  memcpy(cursor_lowerbound_bytes, &cursor_lowerbound_seed, sizeof(cursor_lowerbound_seed));
  cursor_lowerbound_bytes[sizeof(cursor_lowerbound_seed)] = 0;
  MDBX_val cursor_lowerbound_key = val(cursor_lowerbound_bytes, sizeof(cursor_lowerbound_bytes));
  MDBX_val cursor_lowerbound_data = val(NULL, 0);
  struct cursor_get_loop_probe cursor_lowerbound_probe = {0};
  size_t cursor_lowerbound_completed = 0;
  CHECK(mdbx_async_cursor_get_loop_from(async, cursor, 1, MDBX_SET_LOWERBOUND,
                                        &cursor_lowerbound_key, &cursor_lowerbound_data, MDBX_NEXT,
                                        cursor_get_loop_probe_func, &cursor_lowerbound_probe,
                                        &cursor_lowerbound_completed, &op));
  CHECK_OP(op);
  REQUIRE(cursor_lowerbound_completed == 1, "unexpected async cursor lower-bound count");
  REQUIRE(cursor_lowerbound_probe.calls == 1, "async cursor lower-bound probe mismatch");
  REQUIRE(cursor_lowerbound_key.iov_len == sizeof(uint64_t),
          "unexpected cursor lower-bound key size");
  uint64_t cursor_lowerbound_actual_key = 0;
  memcpy(&cursor_lowerbound_actual_key, cursor_lowerbound_key.iov_base, sizeof(cursor_lowerbound_actual_key));
  REQUIRE(cursor_lowerbound_actual_key > cursor_lowerbound_seed &&
              cursor_lowerbound_actual_key < ITEM_COUNT,
          "unexpected cursor lower-bound key");
  CHECK(expect_value(&cursor_lowerbound_data, cursor_lowerbound_actual_key, __FILE__, __LINE__));

  struct scan_probe scan_probe = {12, 0};
  int scan_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_scan(async, cursor, scan_probe_func, &scan_probe, MDBX_FIRST, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan", &op, &scan_result, __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_probe.calls == 13, "unexpected async cursor scan result");

  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    struct scan_probe repeat_scan_probe = {ITEM_COUNT - 1, 0};
    CHECK(mdbx_async_cursor_scan(async, cursor, scan_probe_func, &repeat_scan_probe,
                                 MDBX_FIRST, MDBX_NEXT, NULL, &op));
    CHECK(wait_result("mdbx_async_cursor_scan repeat", &op, &scan_result,
                      __FILE__, __LINE__));
    REQUIRE(scan_result == MDBX_RESULT_TRUE && repeat_scan_probe.calls == ITEM_COUNT,
            "unexpected repeated async cursor scan result");
  }

  uint64_t scan_from_key_data = 18;
  MDBX_val scan_from_key = val(&scan_from_key_data, sizeof(scan_from_key_data));
  MDBX_val scan_from_data = val(NULL, 0);
  struct scan_probe scan_from_probe = {20, 0};
  CHECK(mdbx_async_cursor_scan_from(async, cursor, scan_probe_func, &scan_from_probe, MDBX_SET_LOWERBOUND,
                                    &scan_from_key, &scan_from_data, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan_from", &op, &scan_result, __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_from_probe.calls == 3, "unexpected async cursor scan_from result");
  REQUIRE(scan_from_key.iov_len == sizeof(uint64_t), "unexpected scan_from key size");
  uint64_t scan_from_actual_key = 0;
  memcpy(&scan_from_actual_key, scan_from_key.iov_base, sizeof(scan_from_actual_key));
  REQUIRE(scan_from_actual_key == scan_from_probe.target, "unexpected scan_from key");
  CHECK(expect_value(&scan_from_data, scan_from_actual_key, __FILE__, __LINE__));

  uint64_t scan_from_full_key_data = 0;
  MDBX_val scan_from_full_key = val(&scan_from_full_key_data, sizeof(scan_from_full_key_data));
  MDBX_val scan_from_full_data = val(NULL, 0);
  struct scan_probe scan_from_full_probe = {ITEM_COUNT - 1, 0};
  CHECK(mdbx_async_cursor_scan_from(async, cursor, scan_probe_func, &scan_from_full_probe,
                                    MDBX_SET_LOWERBOUND, &scan_from_full_key,
                                    &scan_from_full_data, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan_from full", &op, &scan_result,
                    __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_from_full_probe.calls == ITEM_COUNT,
          "unexpected full async cursor scan_from result");
  REQUIRE(scan_from_full_key.iov_len == sizeof(uint64_t), "unexpected full scan_from key size");
  uint64_t scan_from_full_actual = 0;
  memcpy(&scan_from_full_actual, scan_from_full_key.iov_base,
         sizeof(scan_from_full_actual));
  REQUIRE(scan_from_full_actual == scan_from_full_probe.target,
          "unexpected full scan_from key");
  CHECK(expect_value(&scan_from_full_data, scan_from_full_actual, __FILE__, __LINE__));

  uint64_t scan_setkey_data = 19;
  MDBX_val scan_setkey_key = val(&scan_setkey_data, sizeof(scan_setkey_data));
  MDBX_val scan_setkey_value = val(NULL, 0);
  struct scan_probe scan_setkey_probe = {21, 0};
  CHECK(mdbx_async_cursor_scan_from(async, cursor, scan_probe_func, &scan_setkey_probe,
                                    MDBX_SET_KEY, &scan_setkey_key, &scan_setkey_value,
                                    MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan_from set-key", &op, &scan_result,
                    __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_setkey_probe.calls == 3,
          "unexpected async cursor scan_from set-key result");
  REQUIRE(scan_setkey_key.iov_len == sizeof(uint64_t),
          "unexpected scan_from set-key key size");
  uint64_t scan_setkey_actual = 0;
  memcpy(&scan_setkey_actual, scan_setkey_key.iov_base, sizeof(scan_setkey_actual));
  REQUIRE(scan_setkey_actual == scan_setkey_probe.target, "unexpected scan_from set-key key");
  CHECK(expect_value(&scan_setkey_value, scan_setkey_actual, __FILE__, __LINE__));

  uint64_t scan_lowerbound_seed = 18;
  uint8_t scan_lowerbound_bytes[sizeof(scan_lowerbound_seed) + 1];
  memcpy(scan_lowerbound_bytes, &scan_lowerbound_seed, sizeof(scan_lowerbound_seed));
  scan_lowerbound_bytes[sizeof(scan_lowerbound_seed)] = 0;
  MDBX_val scan_lowerbound_key = val(scan_lowerbound_bytes, sizeof(scan_lowerbound_bytes));
  MDBX_val scan_lowerbound_data = val(NULL, 0);
  struct scan_probe scan_lowerbound_probe = {20, 0};
  CHECK(mdbx_async_cursor_scan_from(async, cursor, scan_probe_func, &scan_lowerbound_probe,
                                    MDBX_SET_LOWERBOUND, &scan_lowerbound_key,
                                    &scan_lowerbound_data, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan_from lowerbound", &op, &scan_result,
                    __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_lowerbound_probe.calls == 2,
          "unexpected async cursor scan_from lower-bound result");
  REQUIRE(scan_lowerbound_key.iov_len == sizeof(uint64_t),
          "unexpected scan_from lower-bound key size");
  uint64_t scan_lowerbound_actual_key = 0;
  memcpy(&scan_lowerbound_actual_key, scan_lowerbound_key.iov_base,
         sizeof(scan_lowerbound_actual_key));
  REQUIRE(scan_lowerbound_actual_key == scan_lowerbound_probe.target,
          "unexpected scan_from lower-bound key");
  CHECK(expect_value(&scan_lowerbound_data, scan_lowerbound_actual_key,
                     __FILE__, __LINE__));

  CHECK(mdbx_async_cursor_reset(async, cursor, &op));
  CHECK_OP(op);
  cursor_key = val(NULL, 0);
  cursor_data = val(NULL, 0);
  for (;;) {
    int operation_rc = MDBX_SUCCESS;
    CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, seen ? MDBX_NEXT : MDBX_FIRST, &op));
    CHECK(wait_result("mdbx_async_cursor_get", &op, &operation_rc, __FILE__, __LINE__));
    if (operation_rc == MDBX_NOTFOUND)
      break;
    if (operation_rc != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_cursor_get", operation_rc, __FILE__, __LINE__);
      goto bailout;
    }
    REQUIRE(cursor_key.iov_len == sizeof(uint64_t), "unexpected cursor key size");
    uint64_t actual_key = 0;
    memcpy(&actual_key, cursor_key.iov_base, sizeof(actual_key));
    REQUIRE(actual_key == seen, "unexpected cursor key order");
    CHECK(expect_value(&cursor_data, actual_key, __FILE__, __LINE__));
    seen += 1;
  }
  REQUIRE(seen == ITEM_COUNT, "unexpected cursor item count");

  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    cursor_key = val(NULL, 0);
    cursor_data = val(NULL, 0);
    CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_FIRST, &op));
    CHECK_OP(op);
    REQUIRE(cursor_key.iov_len == sizeof(uint64_t), "unexpected repeated first key size");
    uint64_t repeated_first = 0;
    memcpy(&repeated_first, cursor_key.iov_base, sizeof(repeated_first));
    REQUIRE(repeated_first == 0, "unexpected repeated async cursor first key");
    CHECK(expect_value(&cursor_data, repeated_first, __FILE__, __LINE__));
  }

  CHECK(mdbx_async_txn_reset(async, txn, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_renew(async, txn, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_renew(async, txn, cursor, &op));
  CHECK_OP(op);

  cursor_key = val(NULL, 0);
  cursor_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_key, &cursor_data, MDBX_FIRST, &op));
  CHECK_OP(op);
  REQUIRE(cursor_key.iov_len == sizeof(uint64_t), "unexpected renewed cursor key size");
  first_key = 0;
  memcpy(&first_key, cursor_key.iov_base, sizeof(first_key));
  REQUIRE(first_key == 0, "unexpected renewed cursor key");
  CHECK(expect_value(&cursor_data, first_key, __FILE__, __LINE__));

  size_t released_cursor_count = 0;
  CHECK(mdbx_async_txn_release_all_cursors_ex(async, txn, true, &released_cursor_count, &op));
  CHECK_OP(op);
  REQUIRE(released_cursor_count == 1, "unexpected async release-all cursor count");
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_async_txn_abort_ex(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  REQUIRE(cursor != NULL, "write cursor was not returned");
  CHECK(mdbx_async_cursor_put(async, cursor, &cursor_extra_key_value, &cursor_extra_put_value, 0, &op));
  CHECK_OP(op);
  uint64_t cursor_batch_keys[] = {ITEM_COUNT + 1, ITEM_COUNT + 2};
  uint64_t cursor_batch_values[] = {expected_value(ITEM_COUNT + 1), expected_value(ITEM_COUNT + 2)};
  MDBX_val cursor_batch_key_values[] = {
      val(&cursor_batch_keys[0], sizeof(cursor_batch_keys[0])),
      val(&cursor_batch_keys[1], sizeof(cursor_batch_keys[1]))};
  MDBX_val cursor_batch_put_values[] = {
      val(&cursor_batch_values[0], sizeof(cursor_batch_values[0])),
      val(&cursor_batch_values[1], sizeof(cursor_batch_values[1]))};
  memset(op_results, 0, sizeof(op_results));
  CHECK(mdbx_async_cursor_put_batch(async, cursor, cursor_batch_key_values, cursor_batch_put_values, op_results, 2,
                                    0, &op));
  CHECK_OP(op);
  for (size_t i = 0; i < 2; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_cursor_put_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    MDBX_val batch_key = cursor_batch_key_values[i];
    MDBX_val batch_data = val(NULL, 0);
    CHECK(mdbx_async_cursor_get(async, cursor, &batch_key, &batch_data, MDBX_SET_KEY, &op));
    CHECK_OP(op);
    CHECK(expect_payload(&batch_data, cursor_batch_values[i], __FILE__, __LINE__));
  }
  struct put_loop_probe cursor_put_loop_probe;
  memset(&cursor_put_loop_probe, 0, sizeof(cursor_put_loop_probe));
  const size_t cursor_put_loop_count = sizeof(cursor_put_loop_probe.keys) / sizeof(cursor_put_loop_probe.keys[0]);
  for (size_t i = 0; i < cursor_put_loop_count; ++i)
    cursor_put_loop_probe.keys[i] = ITEM_COUNT + 3 + i;
  size_t cursor_put_loop_completed = 0;
  CHECK(mdbx_async_cursor_put_loop(async, cursor, cursor_put_loop_count, put_loop_item_func, put_loop_result_func,
                                   &cursor_put_loop_probe, &cursor_put_loop_completed, 0, &op));
  CHECK_OP(op);
  REQUIRE(cursor_put_loop_completed == cursor_put_loop_count, "unexpected async cursor put loop completion count");
  REQUIRE(cursor_put_loop_probe.items == cursor_put_loop_count && cursor_put_loop_probe.results == cursor_put_loop_count,
          "async cursor put loop callbacks did not cover all items");
  MDBX_val cursor_put_loop_key = val(&cursor_put_loop_probe.keys[2], sizeof(cursor_put_loop_probe.keys[2]));
  MDBX_val cursor_put_loop_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &cursor_put_loop_key, &cursor_put_loop_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&cursor_put_loop_data, cursor_put_loop_probe.values[2], __FILE__, __LINE__));
  MDBX_val delete_key = key_values[0];
  MDBX_val delete_data = val(NULL, 0);
  CHECK(mdbx_async_cursor_get(async, cursor, &delete_key, &delete_data, MDBX_SET_KEY, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_del(async, cursor, MDBX_CURRENT, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_async_put(async, txn, dbi, &key_values[2], &replace_ex_dirty_put_value, MDBX_CURRENT, &op));
  CHECK_OP(op);
  struct preserve_probe preserve_probe = {0};
  uint64_t replace_ex_old_buffer = 0;
  MDBX_val replace_ex_old_value = val(&replace_ex_old_buffer, sizeof(replace_ex_old_buffer));
  CHECK(mdbx_async_replace_ex(async, txn, dbi, &key_values[2], &put_values[2], &replace_ex_old_value, 0,
                              preserve_probe_func, &preserve_probe, &op));
  CHECK_OP(op);
  REQUIRE(preserve_probe.calls == 1, "async replace_ex preserver was not called");
  CHECK(expect_payload(&replace_ex_old_value, replace_ex_dirty_value, __FILE__, __LINE__));

  uint64_t replace_ex_batch_dirty_values[] = {expected_value(6) + UINT64_C(6000),
                                              expected_value(7) + UINT64_C(7000)};
  uint64_t replace_ex_batch_old_buffers[] = {0, 0};
  MDBX_val replace_ex_batch_keys[] = {key_values[6], key_values[7]};
  MDBX_val replace_ex_batch_dirty_data[] = {
      val(&replace_ex_batch_dirty_values[0], sizeof(replace_ex_batch_dirty_values[0])),
      val(&replace_ex_batch_dirty_values[1], sizeof(replace_ex_batch_dirty_values[1]))};
  MDBX_val replace_ex_batch_old_data[] = {
      val(&replace_ex_batch_old_buffers[0], sizeof(replace_ex_batch_old_buffers[0])),
      val(&replace_ex_batch_old_buffers[1], sizeof(replace_ex_batch_old_buffers[1]))};
  for (size_t i = 0; i < 2; ++i) {
    CHECK(mdbx_async_put(async, txn, dbi, &replace_ex_batch_keys[i], &replace_ex_batch_dirty_data[i],
                         MDBX_CURRENT, &op));
    CHECK_OP(op);
  }
  memset(op_results, 0, sizeof(op_results));
  struct preserve_probe preserve_batch_probe = {0};
  CHECK(mdbx_async_replace_ex_batch(async, txn, dbi, replace_ex_batch_keys, put_values + 6,
                                    replace_ex_batch_old_data, op_results, 2, 0, preserve_probe_func,
                                    &preserve_batch_probe, &op));
  CHECK_OP(op);
  REQUIRE(preserve_batch_probe.calls == 2, "async replace_ex_batch preserver was not called per item");
  for (size_t i = 0; i < 2; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_ex_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    CHECK(expect_payload(&replace_ex_batch_old_data[i], replace_ex_batch_dirty_values[i], __FILE__, __LINE__));
  }

  uint64_t replace_ex_loop_dirty_values[] = {expected_value(8) + UINT64_C(8000),
                                             expected_value(9) + UINT64_C(9000),
                                             expected_value(10) + UINT64_C(10000)};
  MDBX_val replace_ex_loop_dirty_data[] = {
      val(&replace_ex_loop_dirty_values[0], sizeof(replace_ex_loop_dirty_values[0])),
      val(&replace_ex_loop_dirty_values[1], sizeof(replace_ex_loop_dirty_values[1])),
      val(&replace_ex_loop_dirty_values[2], sizeof(replace_ex_loop_dirty_values[2]))};
  struct replace_loop_probe replace_ex_loop_probe;
  memset(&replace_ex_loop_probe, 0, sizeof(replace_ex_loop_probe));
  const size_t replace_ex_loop_count = sizeof(replace_ex_loop_probe.keys) / sizeof(replace_ex_loop_probe.keys[0]);
  for (size_t i = 0; i < replace_ex_loop_count; ++i) {
    replace_ex_loop_probe.keys[i] = keys[i + 8];
    replace_ex_loop_probe.expected_old[i] = replace_ex_loop_dirty_values[i];
    CHECK(mdbx_async_put(async, txn, dbi, &key_values[i + 8], &replace_ex_loop_dirty_data[i], MDBX_CURRENT, &op));
    CHECK_OP(op);
  }
  struct preserve_probe preserve_loop_probe = {0};
  size_t replace_ex_loop_completed = 0;
  CHECK(mdbx_async_replace_ex_loop(async, txn, dbi, replace_ex_loop_count, replace_loop_item_func,
                                   replace_loop_result_func, &replace_ex_loop_probe, &replace_ex_loop_completed, 0,
                                   preserve_probe_func, &preserve_loop_probe, &op));
  CHECK_OP(op);
  REQUIRE(replace_ex_loop_completed == replace_ex_loop_count, "unexpected async replace_ex loop completion count");
  REQUIRE(replace_ex_loop_probe.items == replace_ex_loop_count && replace_ex_loop_probe.results == replace_ex_loop_count,
          "async replace_ex loop callbacks did not cover all items");
  REQUIRE(preserve_loop_probe.calls == replace_ex_loop_count, "async replace_ex_loop preserver was not called per item");
  MDBX_val replace_ex_loop_key = val(&replace_ex_loop_probe.keys[2], sizeof(replace_ex_loop_probe.keys[2]));
  MDBX_val replace_ex_loop_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, dbi, &replace_ex_loop_key, &replace_ex_loop_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&replace_ex_loop_data, replace_ex_loop_probe.new_values[2], __FILE__, __LINE__));
  CHECK(mdbx_async_put_batch(async, txn, dbi, key_values + 8, put_values + 8, op_results, replace_ex_loop_count, 0,
                             &op));
  CHECK_OP(op);
  for (size_t i = 0; i < replace_ex_loop_count; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_put_batch replace_ex loop restore", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }

  uint64_t replace_old_buffer = 0;
  MDBX_val replace_old_value = val(&replace_old_buffer, sizeof(replace_old_buffer));
  CHECK(mdbx_async_replace(async, txn, dbi, &key_values[1], &replacement_put_value, &replace_old_value, 0, &op));
  CHECK_OP(op);
  CHECK(expect_value(&replace_old_value, 1, __FILE__, __LINE__));

  replace_old_buffer = 0;
  replace_old_value = val(&replace_old_buffer, sizeof(replace_old_buffer));
  CHECK(mdbx_async_replace(async, txn, dbi, &key_values[1], &put_values[1], &replace_old_value, 0, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&replace_old_value, replacement_value, __FILE__, __LINE__));

  uint64_t replace_batch_values[] = {expected_value(4) + UINT64_C(4000), expected_value(5) + UINT64_C(5000)};
  uint64_t replace_batch_old_values[] = {0, 0};
  MDBX_val replace_batch_keys[] = {key_values[4], key_values[5]};
  MDBX_val replace_batch_new_values[] = {
      val(&replace_batch_values[0], sizeof(replace_batch_values[0])),
      val(&replace_batch_values[1], sizeof(replace_batch_values[1]))};
  MDBX_val replace_batch_old_data[] = {
      val(&replace_batch_old_values[0], sizeof(replace_batch_old_values[0])),
      val(&replace_batch_old_values[1], sizeof(replace_batch_old_values[1]))};
  CHECK(mdbx_async_replace_batch(async, txn, dbi, replace_batch_keys, replace_batch_new_values,
                                 replace_batch_old_data, op_results, 2, 0, &op));
  CHECK_OP(op);
  for (size_t i = 0; i < 2; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    CHECK(expect_value(&replace_batch_old_data[i], i + 4, __FILE__, __LINE__));
  }
  memset(op_results, 0, sizeof(op_results));
  CHECK(mdbx_async_replace_batch(async, txn, dbi, replace_batch_keys, put_values + 4, replace_batch_old_data,
                                 op_results, 2, 0, &op));
  CHECK_OP(op);
  for (size_t i = 0; i < 2; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_replace_batch restore", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
    CHECK(expect_payload(&replace_batch_old_data[i], replace_batch_values[i], __FILE__, __LINE__));
  }

  size_t pending = 0;
  CHECK(mdbx_async_del(async, txn, dbi, &key_values[3], NULL, &op));
  CHECK_OP(op);
  for (unsigned i = 6; i < ITEM_COUNT; i += 3)
    delete_keys[pending++] = key_values[i];
  CHECK(mdbx_async_del_batch(async, txn, dbi, delete_keys, NULL, op_results, pending, &op));
  CHECK_OP(op);
  for (size_t i = 0; i < pending; ++i) {
    if (op_results[i] != MDBX_SUCCESS) {
      rc = fail_rc("mdbx_async_del_batch", op_results[i], __FILE__, __LINE__);
      goto bailout;
    }
  }
  CHECK(mdbx_async_txn_commit_embark_read(async, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "commit-embark-read did not return a read transaction");
  txn_flags = MDBX_TXN_INVALID;
  CHECK(mdbx_async_txn_flags(async, txn, &txn_flags, &op));
  CHECK_OP(op);
  REQUIRE((txn_flags & MDBX_TXN_RDONLY) != 0, "commit-embark-read did not return a read transaction");

  for (unsigned i = 0; i < ITEM_COUNT; ++i)
    get_values[i] = val(NULL, 0);
  CHECK(mdbx_async_get_batch(async, txn, dbi, key_values, get_values, op_results, ITEM_COUNT, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    const int operation_rc = op_results[i];
    if (i % 3 == 0) {
      REQUIRE(operation_rc == MDBX_NOTFOUND, "deleted key was found");
    } else {
      if (operation_rc != MDBX_SUCCESS) {
        rc = fail_rc("mdbx_async_get", operation_rc, __FILE__, __LINE__);
        goto bailout;
      }
      CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
    }
  }

  CHECK(mdbx_async_txn_amend(async, txn, &txn, 0, &txn_userctx_b, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "async transaction amend did not return a write transaction");
  txn_flags = MDBX_TXN_INVALID;
  CHECK(mdbx_async_txn_flags(async, txn, &txn_flags, &op));
  CHECK_OP(op);
  REQUIRE((txn_flags & MDBX_TXN_RDONLY) == 0, "async transaction amend did not return a write transaction");
  txn_context = NULL;
  CHECK(mdbx_async_txn_get_userctx(async, txn, &txn_context, &op));
  CHECK_OP(op);
  REQUIRE(txn_context == &txn_userctx_b, "unexpected amended transaction context");
  CHECK(mdbx_async_put(async, txn, dbi, &amend_key_value, &amend_put_value, 0, &op));
  CHECK_OP(op);
  MDBX_commit_latency commit_latency;
  memset(&commit_latency, 0, sizeof(commit_latency));
  CHECK(mdbx_async_txn_commit_ex(async, txn, &commit_latency, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin_ex(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  MDBX_val amend_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, dbi, &amend_key_value, &amend_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&amend_data, amend_value, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_break(async, txn, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_abort_ex(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  MDBX_defrag_result_t defrag_result;
  memset(&defrag_result, 0, sizeof(defrag_result));
  CHECK(mdbx_async_env_defrag(async, 0, 0, 0, 0, -1, 8, NULL, NULL, &defrag_result, &op));
  CHECK(wait_result("mdbx_async_env_defrag", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async defrag result");
  REQUIRE((defrag_result.stopping_reasons & MDBX_defrag_error) == 0, "async defrag reported an error stop reason");

  MDBX_chk_callbacks_t chk_callbacks;
  MDBX_chk_context_t chk_context;
  struct chk_probe chk_probe = {0};
  memset(&chk_callbacks, 0, sizeof(chk_callbacks));
  memset(&chk_context, 0, sizeof(chk_context));
  chk_callbacks.stage_begin = chk_probe_stage_begin;
  chk_callbacks.stage_end = chk_probe_stage_end;
  active_chk_probe = &chk_probe;
  CHECK(mdbx_async_env_chk(async, &chk_callbacks, &chk_context,
                           MDBX_CHK_SKIP_BTREE_TRAVERSAL | MDBX_CHK_SKIP_KV_TRAVERSAL, MDBX_chk_result, 0, &op));
  CHECK(wait_result("mdbx_async_env_chk", &op, &env_operation_result, __FILE__, __LINE__));
  active_chk_probe = NULL;
  REQUIRE(env_operation_result == MDBX_SUCCESS, "unexpected async environment check result");
  REQUIRE(chk_probe.stage_begin_calls > 0 && chk_probe.stage_end_calls > 0,
          "async environment check did not report stages");
  REQUIRE(chk_context.internal == NULL && chk_context.txn == NULL, "async environment check left context active");
  REQUIRE(chk_context.result.total_problems == 0, "async environment check reported problems");

  CHECK(mdbx_env_get_async_read_stats(env, &read_stats, sizeof(read_stats), false));
  if (expect_explicit_reads) {
    REQUIRE(read_stats.storage_read_items > 0, "explicit read path did not submit storage reads");
    REQUIRE(read_stats.storage_read_batches > 0, "explicit read path did not start storage read batches");
    REQUIRE(read_stats.storage_read_max_batch > 0, "explicit read path did not report storage read batch depth");
    REQUIRE(read_stats.storage_read_completed >= read_stats.storage_read_items,
            "explicit read path did not complete submitted reads");
    REQUIRE(read_stats.storage_read_errors == 0, "explicit read path reported read errors");
    REQUIRE(read_stats.page_cache_misses > 0, "explicit read path did not report page-cache misses");
    REQUIRE(read_stats.page_cache_fills > 0, "explicit read path did not fill page-cache entries");
    if (read_stats.iouring_read_items > 0) {
      REQUIRE(read_stats.iouring_read_batches > 0, "io_uring read path did not report read batches");
      REQUIRE(read_stats.iouring_read_max_batch > 0, "io_uring read path did not report read batch depth");
    }
    if (read_stats.pending_polls > 0)
      REQUIRE(read_stats.iouring_read_max_inflight > 0, "io_uring read path did not report in-flight reads");
  }

  CHECK(mdbx_async_env_close_ex(async, false, &op));
  CHECK(wait_result("mdbx_async_env_close_ex", &op, &close_result, __FILE__, __LINE__));
  if (close_result != MDBX_BUSY)
    env = NULL;
  REQUIRE(close_result == MDBX_SUCCESS, "unexpected async environment close result");
  REQUIRE(mdbx_async_env(async) == NULL, "async environment close did not unbind the executor");
  CHECK(exercise_async_preopen_recovery(path, __FILE__, __LINE__));

  CHECK(mdbx_async_env_delete(async, path, MDBX_ENV_JUST_DELETE, &op));
  CHECK(wait_result("mdbx_async_env_delete cleanup", &op, &env_operation_result, __FILE__, __LINE__));
  REQUIRE(env_operation_result == MDBX_SUCCESS || env_operation_result == MDBX_RESULT_TRUE,
          "unexpected async environment delete result");
  CHECK(mdbx_async_destroy(async, true));
  async = NULL;
  int cleanup_rc = mdbx_env_delete(copy_env_path, MDBX_ENV_JUST_DELETE);
  if (rc == MDBX_SUCCESS && cleanup_rc != MDBX_SUCCESS && cleanup_rc != MDBX_RESULT_TRUE)
    rc = fail_rc("mdbx_env_delete copy-env cleanup", cleanup_rc, __FILE__, __LINE__);
  cleanup_rc = mdbx_env_delete(copy_txn_path, MDBX_ENV_JUST_DELETE);
  if (rc == MDBX_SUCCESS && cleanup_rc != MDBX_SUCCESS && cleanup_rc != MDBX_RESULT_TRUE)
    rc = fail_rc("mdbx_env_delete copy-txn cleanup", cleanup_rc, __FILE__, __LINE__);
  return rc;

bailout:
  if (origin_txn)
    (void)mdbx_txn_abort(origin_txn);
  if (async)
    (void)mdbx_async_destroy(async, true);
  if (env)
    (void)mdbx_env_close(env);
  (void)mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  (void)mdbx_env_delete(copy_env_path, MDBX_ENV_JUST_DELETE);
  (void)mdbx_env_delete(copy_txn_path, MDBX_ENV_JUST_DELETE);
  return rc ? rc : MDBX_PROBLEM;
}
