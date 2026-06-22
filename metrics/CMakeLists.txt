add_library(metrics_lib STATIC
    src/histogram.cpp
    src/metrics_registry.cpp
    src/latency_tracker.cpp
    src/metrics_reporter.cpp
    src/http_metrics_server.cpp
)

target_include_directories(metrics_lib PUBLIC include)

target_link_libraries(metrics_lib PUBLIC
    common
    manager_lib
    Threads::Threads
)
