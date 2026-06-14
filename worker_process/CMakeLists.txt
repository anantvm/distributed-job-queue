# worker_process/CMakeLists.txt

add_library(tcp_worker_lib STATIC
    src/tcp_worker.cpp
)

target_include_directories(tcp_worker_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(tcp_worker_lib
    PUBLIC  net
    PUBLIC  worker_lib      # HandlerRegistry, handler types
    PUBLIC  common
    PRIVATE Threads::Threads
)

# ── Worker binary ─────────────────────────────────────────────────────────────
add_executable(job_worker_process src/worker_main.cpp)
target_link_libraries(job_worker_process
    PRIVATE tcp_worker_lib
    PRIVATE worker_lib
)
