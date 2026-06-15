add_library(manager_lib STATIC
  src/priority_queue.cpp
  src/job_manager.cpp
  src/worker_registry.cpp
  src/lease_manager.cpp
)

target_include_directories(manager_lib PUBLIC include)

target_compile_features(manager_lib PUBLIC cxx_std_20)

target_link_libraries(manager_lib
  PUBLIC  common storage
  PRIVATE Threads::Threads
)
