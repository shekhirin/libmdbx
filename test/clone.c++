/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025
/// \copyright SPDX-License-Identifier: Apache-2.0

#include "test.h++"

#include <atomic>
#include <thread>

class testcase_clone : public testcase {
public:
  testcase_clone(const actor_config &config, const mdbx_pid_t pid) : testcase(config, pid) {}
  bool run() override;
};
REGISTER_TESTCASE(clone);

bool testcase_clone::run() {
  db_open();

  MDBX_dbi handle = 0;
  txn_begin(false);
  int err = mdbx_dbi_open(txn_guard.get(), nullptr, MDBX_DB_DEFAULTS, &handle);
  if (unlikely(err != MDBX_SUCCESS)) {
    log_notice("clone: bailout-prepare due '%s'", mdbx_strerror(err));
    return true;
  }

  static const char key1_cstr[] = "clone_k1";
  static const char val1_cstr[] = "clone_v1";
  static const char key2_cstr[] = "clone_k2";
  static const char val2_cstr[] = "clone_v2";
  MDBX_val key1 = {const_cast<char *>(key1_cstr), sizeof(key1_cstr) - 1};
  MDBX_val val1 = {const_cast<char *>(val1_cstr), sizeof(val1_cstr) - 1};
  MDBX_val key2 = {const_cast<char *>(key2_cstr), sizeof(key2_cstr) - 1};
  MDBX_val val2 = {const_cast<char *>(val2_cstr), sizeof(val2_cstr) - 1};

  mdbx_del(txn_guard.get(), handle, &key1, nullptr);
  mdbx_del(txn_guard.get(), handle, &key2, nullptr);

  err = mdbx_put(txn_guard.get(), handle, &key1, &val1, MDBX_UPSERT);
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_put(k1)", err);
  txn_end(false /* commit */);

  MDBX_txn *source_ptr = nullptr;
  err = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_RDONLY, &source_ptr);
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_txn_begin(source)", err);
  scoped_txn_guard source(source_ptr);
  const uint64_t source_id = mdbx_txn_id(source_ptr);
  if (unlikely(source_id == 0))
    failure("mdbx_txn_id(source) returned 0\n");

  std::atomic<int> writer_rc{MDBX_SUCCESS};
  std::thread writer([&] {
    MDBX_txn *writer_ptr = nullptr;
    int rc = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_READWRITE, &writer_ptr);
    if (rc != MDBX_SUCCESS) {
      writer_rc.store(rc);
      return;
    }
    rc = mdbx_put(writer_ptr, handle, &key2, &val2, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS) {
      mdbx_txn_abort(writer_ptr);
      writer_rc.store(rc);
      return;
    }
    writer_rc.store(mdbx_txn_commit(writer_ptr));
  });
  writer.join();
  err = writer_rc.load();
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("writer-transaction", err);

  std::atomic<int> latest_rc{MDBX_SUCCESS};
  std::atomic<uint64_t> latest_id{0};
  std::thread latest([&] {
    MDBX_txn *latest_ptr = nullptr;
    int rc = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_RDONLY, &latest_ptr);
    if (rc != MDBX_SUCCESS) {
      latest_rc.store(rc);
      return;
    }

    latest_id.store(mdbx_txn_id(latest_ptr));
    MDBX_val got;
    rc = mdbx_get(latest_ptr, handle, &key2, &got);
    mdbx_txn_abort(latest_ptr);
    latest_rc.store(rc);
  });
  latest.join();
  err = latest_rc.load();
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("latest-snapshot", err);
  if (unlikely(latest_id.load() == source_id))
    failure("expected latest txn-id differs from source\n");

  std::atomic<int> clone_rc{MDBX_SUCCESS};
  std::atomic<uint64_t> clone_id{0};
  std::atomic<bool> clone_sees_key1{false};
  std::atomic<bool> clone_sees_key2{false};
  std::atomic<bool> reused_handle{false};
  std::thread cloner([&] {
    MDBX_txn *clone_ptr = nullptr;
    int rc = mdbx_txn_clone(source_ptr, &clone_ptr);
    if (rc != MDBX_SUCCESS) {
      clone_rc.store(rc);
      return;
    }

    clone_id.store(mdbx_txn_id(clone_ptr));
    MDBX_val got;
    clone_sees_key2.store(mdbx_get(clone_ptr, handle, &key2, &got) == MDBX_SUCCESS);
    clone_sees_key1.store(mdbx_get(clone_ptr, handle, &key1, &got) == MDBX_SUCCESS);
    mdbx_txn_abort(clone_ptr);

    MDBX_txn *reuse_ptr = nullptr;
    rc = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_RDONLY, &reuse_ptr);
    if (rc != MDBX_SUCCESS) {
      clone_rc.store(rc);
      return;
    }
    rc = mdbx_txn_reset(reuse_ptr);
    if (rc != MDBX_SUCCESS) {
      mdbx_txn_abort(reuse_ptr);
      clone_rc.store(rc);
      return;
    }

    MDBX_txn *dest_ptr = reuse_ptr;
    rc = mdbx_txn_clone(source_ptr, &dest_ptr);
    if (rc != MDBX_SUCCESS) {
      mdbx_txn_abort(reuse_ptr);
      clone_rc.store(rc);
      return;
    }
    reused_handle.store(dest_ptr == reuse_ptr);
    mdbx_txn_abort(dest_ptr);
  });
  cloner.join();
  err = clone_rc.load();
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("clone-snapshot", err);
  if (unlikely(clone_id.load() != source_id))
    failure("cloned txn-id mismatch\n");
  if (unlikely(clone_sees_key2.load()))
    failure("cloned transaction unexpectedly sees newer data\n");
  if (unlikely(!clone_sees_key1.load()))
    failure("cloned transaction doesn't see expected data\n");
  if (unlikely(!reused_handle.load()))
    failure("expected destination txn handle to be reused\n");

  MDBX_val got;
  err = mdbx_get(source_ptr, handle, &key2, &got);
  if (unlikely(err != MDBX_NOTFOUND))
    failure_perror("mdbx_get(source, k2) != MDBX_NOTFOUND", err);

  return true;
}
