# Copyright (c) 2020-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> ###############################################
# SPDX-License-Identifier: Apache-2.0
#
project(mdbx_ut_and_examples)

if(NOT DEFINED MDBX_LIBRARY)
  set(MDBX_LIBRARY mdbx)
endif()

if(MDBX_BUILD_CXX
   AND CMAKE_CXX_COMPILER_LOADED
   AND NOT MDBX_CXX_STANDARD LESS 17)
  add_executable(mdbx_modern_example example-mdbx.c++)
  set_target_properties(mdbx_modern_example PROPERTIES CXX_STANDARD ${MDBX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)
  target_link_libraries(mdbx_modern_example ${MDBX_LIBRARY})
  if(DEFINED INTERPROCEDURAL_OPTIMIZATION)
    set_target_properties(mdbx_modern_example PROPERTIES INTERPROCEDURAL_OPTIMIZATION
                                                         $<BOOL:${INTERPROCEDURAL_OPTIMIZATION}>)
  endif()
else()
  message(NOTICE
          "The C++ example will be skipped, since C++17 standard is unavailable or C++ API of libmdbx was disabled.")
  set(MDBX_BUILD_CXX FALSE)
endif()

add_executable(mdbx_legacy_example example-mdbx.c)
target_link_libraries(mdbx_legacy_example ${MDBX_LIBRARY})

add_executable(mdbx_migration_smoke migration-smoke.c)
target_link_libraries(mdbx_migration_smoke ${MDBX_LIBRARY})

add_executable(mdbx_async_api_smoke async-api-smoke.c)
target_link_libraries(mdbx_async_api_smoke ${MDBX_LIBRARY})

add_executable(mdbx_async_api_audit async-api-audit.c)

if(UNIX)
  add_executable(mdbx_async_api_bench async-api-bench.c)
  target_link_libraries(mdbx_async_api_bench ${MDBX_LIBRARY} Threads::Threads)
endif()

add_executable(mdbx_migration_smoke_stress migration-smoke.c)
target_compile_definitions(mdbx_migration_smoke_stress
                           PRIVATE MULTIPROCESS_STRESS_READERS=4 MULTIPROCESS_STRESS_WRITERS=4
                                   MULTIPROCESS_STRESS_WAVES=8)
target_link_libraries(mdbx_migration_smoke_stress ${MDBX_LIBRARY})

add_executable(mdbx_migration_smoke_randomized_stress migration-smoke.c)
target_compile_definitions(mdbx_migration_smoke_randomized_stress
                           PRIVATE MULTIPROCESS_STRESS_READERS=4 MULTIPROCESS_STRESS_WRITERS=4
                                   MULTIPROCESS_STRESS_WAVES=8 MULTIPROCESS_STRESS_RANDOMIZED=1)
target_link_libraries(mdbx_migration_smoke_randomized_stress ${MDBX_LIBRARY})

add_executable(mdbx_migration_smoke_crash_stress migration-smoke.c)
target_compile_definitions(mdbx_migration_smoke_crash_stress
                           PRIVATE MULTIPROCESS_STRESS_READERS=4 MULTIPROCESS_STRESS_WRITERS=4
                                   MULTIPROCESS_STRESS_WAVES=8 MULTIPROCESS_CRASH_STRESS=1)
target_link_libraries(mdbx_migration_smoke_crash_stress ${MDBX_LIBRARY})

if(MDBX_ENABLE_DXB_FAULT_INJECTION)
  add_executable(mdbx_migration_fault_injection migration-smoke.c)
  target_compile_definitions(mdbx_migration_fault_injection PRIVATE MIGRATION_FAULT_INJECTION=1)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mdbx_migration_fault_injection PRIVATE -Wno-unused-function)
  endif()
  target_link_libraries(mdbx_migration_fault_injection ${MDBX_LIBRARY})
endif()

if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  message(NOTICE "No emulator to run cross-compiled tests")
  add_test(NAME fake_since_no_crosscompiling_emulator COMMAND ${CMAKE_COMMAND} -E echo
                                                              "No emulator to run cross-compiled tests")
else()
  add_test(NAME c_api COMMAND mdbx_legacy_example)
  add_test(NAME c_api_nommap COMMAND mdbx_legacy_example)
  set_tests_properties(c_api c_api_nommap PROPERTIES RESOURCE_LOCK mdbx_example_database)
  set_tests_properties(c_api_nommap PROPERTIES ENVIRONMENT
                                               "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  add_test(NAME async_api COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_nommap COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_get_ex_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_lowerbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_get_ex_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_lowerbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_scan_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_large_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_set_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_set_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_range_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_range_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_equal_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_to_key_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_last_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_last_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_prev_positioned_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_prev_positioned_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_get_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_batch_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_batch_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_current_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_current_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_current_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_sync_cursor_scan_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_notfound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_notfound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_cache_hit_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_single_get_cache_hit_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_many_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_many_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_many_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_set_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_set_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_range_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_range_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_equal_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_to_key_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_last_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_last_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_prev_positioned_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_get_prev_positioned_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batch_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batch_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_many_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_many_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_get_notfound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_get_notfound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_get_ex_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_lowerbound_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_get_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_batches_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_batches_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_scan_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_many_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_large_cache_st_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_abort_order_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_batch_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_batch_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_batch_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_batch_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_batch_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_batch_mixed_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cache_st_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_get_ex_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_lowerbound_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_current_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_current_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_current_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_setkey_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_setkey_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_equal_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_loop_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_current_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_setkey_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_setkey_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_equal_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_batches_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_current_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_current_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_current_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_prev_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_setkey_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_setkey_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_lt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_lte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_equal_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_equal_miss_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_gte_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_gt_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_upperbound_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_nodup_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_cursor_scan_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_dupsort_fallback_nommap_fallback COMMAND mdbx_async_api_smoke)
  add_test(NAME async_api_dupsort_fallback_nommap_io_uring COMMAND mdbx_async_api_smoke)
  set_tests_properties(async_api async_api_nommap async_api_nommap_io_uring
                       PROPERTIES RESOURCE_LOCK mdbx_async_api_database)
  set_tests_properties(async_api_sync_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_get_ex_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_get_ex_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_lowerbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_lowerbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_get_ex_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_get_ex_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_lowerbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_lowerbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_scan_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_large_cursor_scan_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_LARGE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_set_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_SET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_set_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_SET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_range_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_RANGE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_range_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_RANGE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_equal_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_equal_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_to_key_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_TO_KEY_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_last_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_LAST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_last_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_LAST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_prev_positioned_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_POSITIONED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_prev_positioned_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_PREV_POSITIONED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_get_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_GET_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_batch_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_batch_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_BATCH_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_current_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_current_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_current_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_current_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_sync_cursor_scan_from_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SYNC_CURSOR_SCAN_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_notfound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_NOTFOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_notfound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_NOTFOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_cache_hit_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_CACHE_HIT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_single_get_cache_hit_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_SINGLE_GET_CACHE_HIT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_many_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_many_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_many_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_many_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_many_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_many_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_set_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_set_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_range_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_RANGE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_range_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_RANGE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_equal_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_equal_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_to_key_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_TO_KEY_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_last_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LAST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_last_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LAST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_prev_positioned_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_PREV_POSITIONED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_get_prev_positioned_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_PREV_POSITIONED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batch_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCH_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batch_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCH_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_many_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_many_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_many_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_many_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_MANY_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_get_notfound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_GET_NOTFOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_get_notfound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_GET_NOTFOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_get_ex_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_GET_EX_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_lowerbound_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_LOWERBOUND_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_get_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_get_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_GET_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_batches_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCHES_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_batches_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_BATCHES_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_scan_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cursor_scan_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_many_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_many_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_MANY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_large_cache_st_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LARGE_CACHE_ST_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_abort_order_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_ABORT_ORDER_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_batch_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_batch_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_batch_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_batch_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_batch_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_batch_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_batch_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_batch_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_batch_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_batch_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_batch_mixed_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_batch_mixed_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_BATCH_MIXED_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cache_st_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CACHE_ST_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_get_ex_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_GET_EX_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_lowerbound_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_LOWERBOUND_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_current_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_current_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_current_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_current_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_setkey_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_setkey_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_setkey_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_setkey_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_equal_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_equal_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_loop_from_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_LOOP_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_current_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_current_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_setkey_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_setkey_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_setkey_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_setkey_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_equal_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_equal_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_batches_from_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_BATCHES_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_current_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_current_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_current_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_current_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_CURRENT_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_prev_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_prev_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_PREV_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_setkey_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_setkey_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_setkey_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_setkey_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_SETKEY_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_lt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_lt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_LT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_lte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_lte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_LTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_equal_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_equal_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_equal_miss_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_equal_miss_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_EQUAL_MISS_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_gte_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_gte_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GTE_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_gt_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_gt_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_GT_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_upperbound_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_upperbound_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_UPPERBOUND_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_nodup_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_cursor_scan_from_nodup_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_CURSOR_SCAN_FROM_NODUP_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_dupsort_fallback_nommap_fallback
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_DUPSORT_FALLBACK_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_NO_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_dupsort_fallback_nommap_io_uring
                       PROPERTIES
                         RESOURCE_LOCK mdbx_async_api_database
                         ENVIRONMENT
                           "MDBX_ASYNC_SMOKE_DUPSORT_FALLBACK_READ_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_nommap PROPERTIES ENVIRONMENT
                                                   "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  set_tests_properties(async_api_nommap_io_uring
                       PROPERTIES ENVIRONMENT
                                  "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  add_test(NAME async_api_audit COMMAND mdbx_async_api_audit ${CMAKE_SOURCE_DIR}/mdbx.h ${CMAKE_SOURCE_DIR}/mdbx.c)
  add_test(NAME migration_smoke COMMAND mdbx_migration_smoke)
  add_test(NAME migration_smoke_nommap COMMAND mdbx_migration_smoke)
  set_tests_properties(migration_smoke_nommap PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1")
  add_test(NAME migration_smoke_nommap_tinycache COMMAND mdbx_migration_smoke)
  set_tests_properties(migration_smoke_nommap_tinycache
                       PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  add_test(NAME migration_smoke_stress_nommap_tinycache COMMAND mdbx_migration_smoke_stress)
  set_tests_properties(migration_smoke_stress_nommap_tinycache
                       PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  add_test(NAME migration_smoke_randomized_stress_nommap_tinycache COMMAND mdbx_migration_smoke_randomized_stress)
  set_tests_properties(migration_smoke_randomized_stress_nommap_tinycache
                       PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  add_test(NAME migration_smoke_crash_stress_nommap_tinycache COMMAND mdbx_migration_smoke_crash_stress)
  set_tests_properties(migration_smoke_crash_stress_nommap_tinycache
                       PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  if(MDBX_ENABLE_DXB_FAULT_INJECTION)
    add_test(NAME async_api_read_fault_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_ex_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_lowerbound_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_ex_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_lowerbound_many_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_ex_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_lowerbound_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_ex_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_lowerbound_batch_mixed_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_st_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_st_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_st_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_set_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_range_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_to_key_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_last_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_prev_positioned_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_get_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_batch_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_get_ex_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_large_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_current_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_sync_cursor_scan_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_ex_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_ex_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_get_ex_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_lowerbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_lowerbound_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_lowerbound_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_lowerbound_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cursor_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cursor_batches_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_st_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_st_many_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_st_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_large_cache_st_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_get_ex_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_lowerbound_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cache_st_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_set_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_range_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_get_to_key_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_last_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_prev_positioned_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batch_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batch_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_loop_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_batches_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_current_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_prev_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_setkey_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_setkey_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_lt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_lte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_equal_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_equal_miss_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_gte_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_gt_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_upperbound_nommap_io_uring COMMAND mdbx_async_api_smoke)
    add_test(NAME async_api_read_fault_cursor_scan_from_nodup_nommap_io_uring COMMAND mdbx_async_api_smoke)
    set_tests_properties(async_api_read_fault_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_ex_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_lowerbound_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_many_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_MANY_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_ex_many_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_MANY_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_lowerbound_many_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_MANY_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_ex_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_lowerbound_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_batch_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_BATCH_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_ex_batch_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_BATCH_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_lowerbound_batch_mixed_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_BATCH_MIXED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_st_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_st_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_st_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_ex_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_get_ex_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_GET_EX_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_lowerbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LOWERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_set_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_SET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_range_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_RANGE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_equal_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_EQUAL_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_to_key_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_TO_KEY_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_last_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_LAST_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_prev_positioned_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_PREV_POSITIONED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_get_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_GET_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_batch_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_BATCH_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_get_ex_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_GET_EX_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_lowerbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_LOWERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_cursor_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_CURSOR_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_cursor_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_CURSOR_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_large_cursor_scan_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_LARGE_CURSOR_SCAN_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_current_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_CURRENT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_current_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_CURRENT_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_sync_cursor_scan_from_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_SYNC_CURSOR_SCAN_FROM_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_ex_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_ex_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_get_ex_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_GET_EX_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_lowerbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_lowerbound_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_lowerbound_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_lowerbound_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_LOWERBOUND_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cursor_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cursor_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cursor_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cursor_batches_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_BATCHES_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cursor_scan_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CURSOR_SCAN_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_st_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_st_many_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_MANY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_st_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_large_cache_st_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LARGE_CACHE_ST_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_get_ex_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_GET_EX_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_lowerbound_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_LOWERBOUND_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cache_st_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CACHE_ST_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_set_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_SET_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_range_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_RANGE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_GET_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_equal_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_EQUAL_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_get_to_key_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_TO_KEY_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_last_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LAST_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_prev_positioned_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_PREV_POSITIONED_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batch_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCH_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batch_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCH_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_current_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_CURRENT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_setkey_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_SETKEY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_setkey_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_SETKEY_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_equal_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_EQUAL_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_loop_from_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_LOOP_FROM_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_current_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_CURRENT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_setkey_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_SETKEY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_setkey_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_SETKEY_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_equal_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_EQUAL_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_batches_from_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_BATCHES_FROM_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_current_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_CURRENT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_prev_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_PREV_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_setkey_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_SETKEY_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_setkey_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_SETKEY_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_lt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_LT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_lte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_LTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_equal_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_EQUAL_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_equal_miss_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_EQUAL_MISS_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_gte_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_GTE_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_gt_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_GT_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_upperbound_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_UPPERBOUND_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    set_tests_properties(async_api_read_fault_cursor_scan_from_nodup_nommap_io_uring
                         PROPERTIES
                           RESOURCE_LOCK mdbx_async_api_database
                           ENVIRONMENT
                             "MDBX_ASYNC_SMOKE_READ_FAULT_CURSOR_SCAN_FROM_NODUP_ONLY=1;MDBX_ASYNC_SMOKE_EXPECT_IOURING=1;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_IO_BACKEND=io_uring;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
    add_test(NAME migration_fault_injection_nommap COMMAND mdbx_migration_fault_injection)
    set_tests_properties(migration_fault_injection_nommap PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1")
    add_test(NAME migration_fault_injection_nommap_tinycache COMMAND mdbx_migration_fault_injection)
    set_tests_properties(migration_fault_injection_nommap_tinycache
                         PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  endif()
  if(TARGET mdbx_chk
     AND TARGET mdbx_copy
     AND TARGET mdbx_defrag
     AND TARGET mdbx_drop
     AND TARGET mdbx_dump
     AND TARGET mdbx_load
     AND TARGET mdbx_stat)
    add_test(NAME migration_tool_roundtrip
             COMMAND
               ${CMAKE_COMMAND} -DMDBX_CHK=$<TARGET_FILE:mdbx_chk> -DMDBX_COPY=$<TARGET_FILE:mdbx_copy>
               -DMDBX_DEFRAG=$<TARGET_FILE:mdbx_defrag> -DMDBX_DROP=$<TARGET_FILE:mdbx_drop>
               -DMDBX_DUMP=$<TARGET_FILE:mdbx_dump>
               -DMDBX_LOAD=$<TARGET_FILE:mdbx_load>
               -DMDBX_STAT=$<TARGET_FILE:mdbx_stat>
               -DMDBX_WORKDIR=${CMAKE_CURRENT_BINARY_DIR}/migration-tool-roundtrip
               -P ${CMAKE_CURRENT_SOURCE_DIR}/migration-tool-roundtrip.cmake)
    add_test(NAME migration_tool_roundtrip_nommap
             COMMAND
               ${CMAKE_COMMAND} -DMDBX_CHK=$<TARGET_FILE:mdbx_chk> -DMDBX_COPY=$<TARGET_FILE:mdbx_copy>
               -DMDBX_DEFRAG=$<TARGET_FILE:mdbx_defrag> -DMDBX_DROP=$<TARGET_FILE:mdbx_drop>
               -DMDBX_DUMP=$<TARGET_FILE:mdbx_dump>
               -DMDBX_LOAD=$<TARGET_FILE:mdbx_load>
               -DMDBX_STAT=$<TARGET_FILE:mdbx_stat>
               -DMDBX_WORKDIR=${CMAKE_CURRENT_BINARY_DIR}/migration-tool-roundtrip-nommap
               -P ${CMAKE_CURRENT_SOURCE_DIR}/migration-tool-roundtrip.cmake)
    set_tests_properties(migration_tool_roundtrip_nommap PROPERTIES ENVIRONMENT "MDBX_FORCE_NO_DATA_MMAP=1")
    add_test(NAME migration_tool_roundtrip_nommap_tinycache
             COMMAND
               ${CMAKE_COMMAND} -DMDBX_CHK=$<TARGET_FILE:mdbx_chk> -DMDBX_COPY=$<TARGET_FILE:mdbx_copy>
               -DMDBX_DEFRAG=$<TARGET_FILE:mdbx_defrag> -DMDBX_DROP=$<TARGET_FILE:mdbx_drop>
               -DMDBX_DUMP=$<TARGET_FILE:mdbx_dump>
               -DMDBX_LOAD=$<TARGET_FILE:mdbx_load>
               -DMDBX_STAT=$<TARGET_FILE:mdbx_stat>
               -DMDBX_WORKDIR=${CMAKE_CURRENT_BINARY_DIR}/migration-tool-roundtrip-nommap-tinycache
               -P ${CMAKE_CURRENT_SOURCE_DIR}/migration-tool-roundtrip.cmake)
    set_tests_properties(migration_tool_roundtrip_nommap_tinycache
                         PROPERTIES ENVIRONMENT
                                    "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  endif()
  if(MDBX_BUILD_CXX)
    add_test(NAME c++_api COMMAND mdbx_modern_example)
    add_test(NAME c++_api_nommap COMMAND mdbx_modern_example)
    set_tests_properties(c++_api c++_api_nommap PROPERTIES RESOURCE_LOCK mdbx_example_database)
    set_tests_properties(c++_api_nommap PROPERTIES ENVIRONMENT
                                                   "MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  endif()
endif()

if(UNIX)
  add_executable(pcrf_simulator pcrf/pcrf_simulator.c)
  target_link_libraries(pcrf_simulator ${MDBX_LIBRARY})
  if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
    add_test(NAME pcrf_simulator_smoke COMMAND pcrf_simulator)
    set_tests_properties(pcrf_simulator_smoke
                         PROPERTIES
                           ENVIRONMENT
                           "PCRF_DB_PATH=./pcrf-smoke;PCRF_INITIAL_RECORDS=256;PCRF_CHURN_ROUNDS=3;PCRF_CHURN_BATCH=32;PCRF_CHURN_EXTRA=8;PCRF_STAT_INTERVAL_US=0")
    add_test(NAME pcrf_simulator_nommap_smoke COMMAND pcrf_simulator)
    set_tests_properties(pcrf_simulator_nommap_smoke
                         PROPERTIES
                           ENVIRONMENT
                           "PCRF_DB_PATH=./pcrf-nommap-smoke;PCRF_INITIAL_RECORDS=256;PCRF_CHURN_ROUNDS=3;PCRF_CHURN_BATCH=32;PCRF_CHURN_EXTRA=8;PCRF_STAT_INTERVAL_US=0;MDBX_FORCE_NO_DATA_MMAP=1;MDBX_EXPLICIT_PAGE_CACHE_LIMIT=64K")
  endif()
endif()
