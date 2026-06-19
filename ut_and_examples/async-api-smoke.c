/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Focused smoke coverage for the public asynchronous executor API.
 */

#include "mdbx.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITEM_COUNT 64

struct async_probe {
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
  uint64_t keys[ITEM_COUNT];
  uint64_t values[ITEM_COUNT];
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
  uint8_t open2_name_bytes[] = {'a', 's', 'y', 'n', 'c', '-', 'o', 'p', 'e', 'n', '2', 0, 'o', 'l', 'd'};
  uint8_t rename2_name_bytes[] = {'a', 's', 'y', 'n', 'c', '-', 'o', 'p', 'e', 'n', '2', 0, 'n', 'e', 'w'};
  MDBX_val open2_name = val(open2_name_bytes, sizeof(open2_name_bytes));
  MDBX_val rename2_name = val(rename2_name_bytes, sizeof(rename2_name_bytes));
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

  memset(ops, 0, sizeof(ops));
  snprintf(path, sizeof(path), "./async-api-smoke-%llx", smoke_run_id());
  snprintf(copy_env_path, sizeof(copy_env_path), "./async-api-smoke-copy-env-%llx", smoke_run_id());
  snprintf(copy_txn_path, sizeof(copy_txn_path), "./async-api-smoke-copy-txn-%llx", smoke_run_id());

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
  CHECK(mdbx_async_env_set_option(async, MDBX_opt_max_db, 12, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_open(async, env, path, MDBX_NOSUBDIR | MDBX_LIFORECLAIM, 0664, &op));
  CHECK_OP(op);
  REQUIRE(mdbx_async_env(async) == env, "async executor returned wrong environment");

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

  uint64_t option_value = 0;
  CHECK(mdbx_async_env_set_option(async, MDBX_opt_sync_bytes, 131072, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_option(async, MDBX_opt_sync_bytes, &option_value, &op));
  CHECK_OP(op);
  REQUIRE(option_value == 131072, "unexpected async environment sync-bytes option");

  CHECK(mdbx_async_env_set_option(async, MDBX_opt_sync_period, 65536, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_env_get_option(async, MDBX_opt_sync_period, &option_value, &op));
  CHECK_OP(op);
  REQUIRE(option_value != 0, "unexpected async environment sync-period option");

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
  CHECK(mdbx_async_env_sync_ex(async, false, true, &op));
  CHECK(wait_result("mdbx_async_env_sync_ex", &op, &env_operation_result, __FILE__, __LINE__));
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

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);
  REQUIRE(txn != NULL, "write transaction was not returned");

  int txn_userctx_a = 51;
  int txn_userctx_b = 52;
  void *txn_context = NULL;
  CHECK(mdbx_async_txn_set_userctx(async, txn, &txn_userctx_a, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_get_userctx(async, txn, &txn_context, &op));
  CHECK_OP(op);
  REQUIRE(txn_context == &txn_userctx_a, "unexpected async transaction context");
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

  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  memset(&env_stat, 0, sizeof(env_stat));
  CHECK(mdbx_async_env_stat_ex(async, NULL, &env_stat, sizeof(env_stat), &op));
  CHECK_OP(op);
  REQUIRE(env_stat.ms_entries == ITEM_COUNT, "unexpected async environment stat entries after commit");

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
  REQUIRE(txn != NULL, "read transaction was not returned");

  memset(&env_stat, 0, sizeof(env_stat));
  CHECK(mdbx_async_env_stat_ex(async, txn, &env_stat, sizeof(env_stat), &op));
  CHECK_OP(op);
  REQUIRE(env_stat.ms_entries == ITEM_COUNT, "unexpected async txn-scoped environment stat entries");

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
  REQUIRE(dbi_stat.ms_entries == ITEM_COUNT, "unexpected async dbi stat entry count");

  unsigned dbi_flags = UINT_MAX;
  unsigned dbi_state = UINT_MAX;
  CHECK(mdbx_async_dbi_flags_ex(async, txn, dbi, &dbi_flags, &dbi_state, &op));
  CHECK_OP(op);
  REQUIRE((dbi_flags & MDBX_DUPSORT) == 0, "unexpected async dbi dupsort flag");

  uint32_t depthmask = UINT32_MAX;
  int depthmask_result = MDBX_SUCCESS;
  CHECK(mdbx_async_dbi_dupsort_depthmask(async, txn, dbi, &depthmask, &op));
  CHECK(wait_result("mdbx_async_dbi_dupsort_depthmask", &op, &depthmask_result, __FILE__, __LINE__));
  REQUIRE(depthmask_result == MDBX_RESULT_TRUE, "non-dupsort dbi did not report MDBX_RESULT_TRUE");
  REQUIRE(depthmask == 0, "unexpected non-dupsort depthmask");

  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    get_values[i] = val(NULL, 0);
    CHECK(mdbx_async_get(async, txn, dbi, &key_values[i], &get_values[i], &ops[i]));
  }
  CHECK(wait_many_success("mdbx_async_get", ops, ITEM_COUNT, op_results, __FILE__, __LINE__));
  for (unsigned i = 0; i < ITEM_COUNT; ++i) {
    CHECK(expect_value(&get_values[i], keys[i], __FILE__, __LINE__));
  }

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

  MDBX_cache_entry_t cache_entry;
  mdbx_cache_init(&cache_entry);
  MDBX_cache_result_t cache_result = {MDBX_SUCCESS, MDBX_CACHE_ERROR};
  MDBX_val cache_data = val(NULL, 0);
  CHECK(mdbx_async_cache_get(async, txn, dbi, &key_values[4], &cache_data, &cache_entry, &cache_result, &op));
  CHECK_OP(op);
  REQUIRE(cache_result.errcode == MDBX_SUCCESS && cache_result.status != MDBX_CACHE_ERROR,
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
  CHECK(mdbx_async_cursor_close(async, copy_cursor, &op));
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

  struct scan_probe scan_probe = {12, 0};
  int scan_result = MDBX_SUCCESS;
  CHECK(mdbx_async_cursor_scan(async, cursor, scan_probe_func, &scan_probe, MDBX_FIRST, MDBX_NEXT, NULL, &op));
  CHECK(wait_result("mdbx_async_cursor_scan", &op, &scan_result, __FILE__, __LINE__));
  REQUIRE(scan_result == MDBX_RESULT_TRUE && scan_probe.calls == 13, "unexpected async cursor scan result");

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

  size_t released_cursor_count = 0;
  CHECK(mdbx_async_txn_release_all_cursors(async, txn, true, &released_cursor_count, &op));
  CHECK_OP(op);
  REQUIRE(released_cursor_count == 1, "unexpected async release-all cursor count");
  CHECK(mdbx_async_cursor_close(async, cursor, &op));
  CHECK_OP(op);
  cursor = NULL;

  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, 0, &txn, NULL, &op));
  CHECK_OP(op);

  CHECK(mdbx_async_cursor_open(async, txn, dbi, &cursor, &op));
  CHECK_OP(op);
  REQUIRE(cursor != NULL, "write cursor was not returned");
  CHECK(mdbx_async_cursor_put(async, cursor, &cursor_extra_key_value, &cursor_extra_put_value, 0, &op));
  CHECK_OP(op);
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
  CHECK(mdbx_async_txn_commit(async, txn, NULL, &op));
  CHECK_OP(op);
  txn = NULL;

  CHECK(mdbx_async_txn_begin(async, NULL, MDBX_TXN_RDONLY, &txn, NULL, &op));
  CHECK_OP(op);
  MDBX_val amend_data = val(NULL, 0);
  CHECK(mdbx_async_get(async, txn, dbi, &amend_key_value, &amend_data, &op));
  CHECK_OP(op);
  CHECK(expect_payload(&amend_data, amend_value, __FILE__, __LINE__));
  CHECK(mdbx_async_txn_break(async, txn, &op));
  CHECK_OP(op);
  CHECK(mdbx_async_txn_abort(async, txn, NULL, &op));
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
