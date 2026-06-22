add_executable(phase1_demo phase1_demo.cpp)

target_compile_features(phase1_demo PRIVATE cxx_std_20)

target_link_libraries(phase1_demo
  PRIVATE
    common
    storage
    manager_lib
    worker_lib
    metrics_lib
)
