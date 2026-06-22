# tests/unit/CMakeLists.txt

add_executable(unit_tests
    test_priority_queue.cpp
    test_job_manager.cpp
    test_storage.cpp
    test_worker.cpp
    test_protocol.cpp
    test_worker_registry.cpp
    test_lease_manager.cpp
    test_counter.cpp
    test_gauge.cpp
    test_histogram.cpp
    test_metrics_registry.cpp
    test_latency_tracker.cpp
)

target_link_libraries(unit_tests
    PRIVATE manager_lib
    PRIVATE storage
    PRIVATE worker_lib
    PRIVATE net
    PRIVATE metrics_lib
    PRIVATE GTest::gtest_main
)

gtest_discover_tests(unit_tests
    PROPERTIES TIMEOUT 60
)
