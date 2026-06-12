cmake_minimum_required(VERSION 3.20)

project(
  distributed_job_queue
  VERSION 0.1.0
  DESCRIPTION "High-Performance Distributed Job Queue — Phase 1"
  LANGUAGES CXX
)

# ─── Language Standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / IDE support

# ─── Build Type ───────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  message(STATUS "No build type selected, defaulting to Debug")
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ─── Compiler Options ─────────────────────────────────────────────────────────
include(cmake/CompilerOptions.cmake)

# ─── Thread Support ───────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─── SQLite3 ──────────────────────────────────────────────────────────────────
find_package(SQLite3 REQUIRED)
message(STATUS "Found SQLite3: ${SQLite3_VERSION} at ${SQLite3_INCLUDE_DIRS}")

# ─── Google Test (fetched at configure time) ──────────────────────────────────
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)

# Prevent GTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Don't install GTest alongside our targets
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ─── Enable CTest ─────────────────────────────────────────────────────────────
enable_testing()
include(GoogleTest)

# ─── Subdirectories (order matters — lower layers first) ──────────────────────
add_subdirectory(common)
add_subdirectory(storage)
add_subdirectory(manager)
add_subdirectory(worker)
add_subdirectory(demo)
add_subdirectory(tests)

# ─── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════")
message(STATUS "  Distributed Job Queue — Phase 1")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  SQLite3    : ${SQLite3_VERSION}")
message(STATUS "══════════════════════════════════════════")
message(STATUS "")
