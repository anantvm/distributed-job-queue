add_library(storage STATIC
  src/sqlite_backend.cpp
  src/in_memory_backend.cpp
)

target_include_directories(storage PUBLIC include)

target_compile_features(storage PUBLIC cxx_std_20)

target_link_libraries(storage
  PUBLIC  common
  PRIVATE SQLite::SQLite3
)
