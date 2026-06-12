add_library(worker_lib STATIC
  src/handler_registry.cpp
  src/worker.cpp
)

target_include_directories(worker_lib PUBLIC include)

target_compile_features(worker_lib PUBLIC cxx_std_20)

target_link_libraries(worker_lib
  PUBLIC  common manager_lib
  PRIVATE Threads::Threads
)
