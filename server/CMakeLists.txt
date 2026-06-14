# server/CMakeLists.txt

add_library(server_lib STATIC
    src/server.cpp
)

target_include_directories(server_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(server_lib
    PUBLIC  net
    PUBLIC  manager_lib
    PUBLIC  storage
    PUBLIC  common
    PRIVATE Threads::Threads
)

# ── Server binary ─────────────────────────────────────────────────────────────
add_executable(job_manager_server src/main.cpp)
target_link_libraries(job_manager_server
    PRIVATE server_lib
    PRIVATE storage        # for SQLiteBackend
    PRIVATE SQLite::SQLite3
)
