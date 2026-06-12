add_library(common STATIC
  src/uuid.cpp
  src/logger.cpp
)

target_include_directories(common PUBLIC include)

target_compile_features(common PUBLIC cxx_std_20)

target_link_libraries(common PUBLIC Threads::Threads)
