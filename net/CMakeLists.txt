# net/CMakeLists.txt

add_library(net STATIC
    src/socket_utils.cpp
    src/protocol.cpp
    src/tcp_connection.cpp
    src/event_loop.cpp
)

target_include_directories(net
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(net
    PUBLIC common
    PUBLIC Threads::Threads
)
