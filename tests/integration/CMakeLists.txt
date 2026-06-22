# tests/integration/CMakeLists.txt

add_executable(test_net_integration
    test_net_integration.cpp
)

target_link_libraries(test_net_integration
    PRIVATE server_lib
    PRIVATE client_lib
    PRIVATE tcp_worker_lib
    PRIVATE worker_lib
    PRIVATE storage
    PRIVATE metrics_lib
    PRIVATE GTest::gtest_main
)

gtest_discover_tests(test_net_integration
    PROPERTIES TIMEOUT 30
)

add_executable(test_phase3_integration
    test_phase3_integration.cpp
)

target_link_libraries(test_phase3_integration
    PRIVATE server_lib
    PRIVATE client_lib
    PRIVATE tcp_worker_lib
    PRIVATE worker_lib
    PRIVATE storage
    PRIVATE manager_lib
    PRIVATE common
    PRIVATE net
    PRIVATE metrics_lib
    PRIVATE GTest::gtest_main
)

gtest_discover_tests(test_phase3_integration
    PROPERTIES TIMEOUT 60
)

add_executable(test_metrics_endpoint
    test_metrics_endpoint.cpp
)

target_link_libraries(test_metrics_endpoint
    PRIVATE metrics_lib
    PRIVATE GTest::gtest_main
)

gtest_discover_tests(test_metrics_endpoint
    PROPERTIES TIMEOUT 30
)

add_executable(test_metrics_accuracy
    test_metrics_accuracy.cpp
)

target_link_libraries(test_metrics_accuracy
    PRIVATE metrics_lib
    PRIVATE manager_lib
    PRIVATE storage
    PRIVATE GTest::gtest_main
)

gtest_discover_tests(test_metrics_accuracy
    PROPERTIES TIMEOUT 30
)
