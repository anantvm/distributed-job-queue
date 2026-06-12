add_executable(unit_tests
  unit/test_priority_queue.cpp
  unit/test_storage.cpp
  unit/test_job_manager.cpp
  unit/test_worker.cpp
)

target_compile_features(unit_tests PRIVATE cxx_std_20)

target_link_libraries(unit_tests
  PRIVATE
    common
    storage
    manager_lib
    worker_lib
    GTest::gtest_main   # provides main(); no need to write one
)

# Register each test with CTest so `ctest` discovers them automatically.
gtest_discover_tests(unit_tests)
