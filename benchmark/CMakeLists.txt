add_library(benchmark_lib STATIC
    src/load_generator.cpp
    src/metrics_client.cpp
    src/benchmark_runner.cpp
    src/benchmark_report.cpp
)
target_include_directories(benchmark_lib PUBLIC include)
target_link_libraries(benchmark_lib PUBLIC common client_lib Threads::Threads)

add_executable(benchmark_runner src/benchmark_main.cpp)
target_link_libraries(benchmark_runner PRIVATE benchmark_lib)
