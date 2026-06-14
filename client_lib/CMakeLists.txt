# client_lib/CMakeLists.txt

add_library(client_lib STATIC
    src/job_client.cpp
)

target_include_directories(client_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(client_lib
    PUBLIC  net
    PUBLIC  common
)

# ── CLI client binary ─────────────────────────────────────────────────────────
add_executable(job_client src/client_main.cpp)
target_link_libraries(job_client
    PRIVATE client_lib
)
