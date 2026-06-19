/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Focused smoke coverage for the public asynchronous executor API.
 */

#include "mdbx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITEM_COUNT 64

struct async_probe {
  unsigned calls;
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

static MDBX_val val(void *base, size_t len) {
  MDBX_val result;
  result.iov_base = base;
  result.iov_len = len;
  return result;
}

static uint64_t expected_value(uint64_t key) { return key * UINT64_C(17) + UINT64_C(11); }

static int async_probe_func(MDBX_env *env, void *context) {
  struct async_probe *const probe = (struct async_probe *)context;
  if (!env || !probe)
    return MDBX_EINVAL;
  probe->calls += 1;
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

  int rc = mdbx_async_wait_all(ops, count, operation_results);
  if (rc != MDBX_SUCCESS)
    return fail_rc(expr, rc, file, line);
  rc = mdbx_async_op_release_all(ops, count);
  if (rc != MDBX_SUCCESS)
    return fail_rc("mdbx_async_op_release_all", rc, file, line);
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

static int expect_value(const MDBX_val *data, uint64_t key, const char *file, int line) {
  if (data->iov_len != sizeof(uint64_t))
    return fail_msg("unexpected value size", file, line);
  uint64_t actual = 0;
  memcpy(&actual, data->iov_base, sizeof(actual));
  if (actual != expected_value(key))
    return fail_msg("unexpected value payload", file, line);
  return MDBX_SUCCESS;
}

int main(void) {
  char path[96];
  MDBX_env *env = NULL;
  MDBX_async *async = NULL;
  MDBX_txn *txn = NULL;
  MDBX_cursor *cursor = NULL;
  MDBX_async_op *op = NULL;
  MDBX_dbi dbi = 0;
  uint64_t keys[ITEM_COUNT];
  uint64_t values[ITEM_COUNT];
  MDBX_val put_values[ITEM_COUNT];
  MDBX_val get_values[ITEM_COUNT];
  MDBX_async_op *ops[ITEM_COUNT];
  int op_results[ITEM_COUNT];
  int rc = MDBX_SUCCESS;

  memset(ops, 0, sizeof(ops));
  snprintf(path, sizeof(path), "./async-api-smoke-%llx", smoke_run_id());

  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
    rc = fail_rc("mdbx_env_delete", rc, __FILE__, __LINE__);
    goto bailout;
  }

  CHECK(mdbx_env_create(&env));
  CHECK(mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664));
  CHECK(mdbx_async_create(env, MDBX_ASYNC_DEFAULTS, &async));
  REQUIRE(mdbx_async_env(async) == env, "async executor returned wrong environment");

  struct async_probe probe = {0};
  CHECK(mdbx_async_submit(async, async_probe_func, &probe, &op));
  CHECK_OP(op);
  REQUIRE(probe.calls == 1, "generic async callback was not executed exactly once");

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "write transaction was not returned");

  CHECK(mdbx_async_dbi_open(async, txn, NULL, MDBX_DB_DEFAULTS, &dbi, &op));
  CHECK_OP(op);

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    keys[i] = i;
    values[i] = expected_value(keys[i]);
    MDBX_val key = val(&keys[i], sizeof(keys[i]));
    put_values[i] = val(&values[i], sizeof(values[i]));
    CHECK(mdbx_async_put(async, txn, dbi, &key, &put_values[i], 0, &ops[i]));
  }
  CHECK(wait_many_success("mdbx_async_put", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "read transaction was not returned");

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    MDBX_val key = val(&keys[i], sizeof(keys[i]));
    get_values[i] = val(NULL, 0);
    CHECK(mdbx_async_get(async, txn, dbi, &key, &get_values[i], &ops[i]));
  }
  CHECK(wait_many_success("mdbx_async_get", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

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

  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  size_t pending = 0;
  for (unsigned i = 0; i < ITEM_COUNT; i += 3) {
    MDBX_val key = val(&keys[i], sizeof(keys[i]));
    CHECK(mdbx_async_del(async, txn, dbi, &key, NULL, &ops[pending++]));
  }
  CHECK(wait_many_success("mdbx_async_del", ops, pending, op_results, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    MDBX_val key = val(&keys[i], sizeof(keys[i]));
    get_values[i] = val(NULL, 0);
    CHECK(mdbx_async_get(async, txn, dbi, &key, &get_values[i], &ops[i]));
  }
  CHECK(wait_many_result("mdbx_async_get", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
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
  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_destroy(async, true));
  async = NULL;
  CHECK(mdbx_env_close(env));
  env = NULL;

  rc = mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  if (rc == MDBX_RESULT_TRUE)
    rc = MDBX_SUCCESS;
  if (rc != MDBX_SUCCESS)
    rc = fail_rc("mdbx_env_delete cleanup", rc, __FILE__, __LINE__);
  return rc;

bailout:
  if (async)
    (void)mdbx_async_destroy(async, true);
  if (env)
    (void)mdbx_env_close(env);
  (void)mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  return rc ? rc : MDBX_PROBLEM;
}
