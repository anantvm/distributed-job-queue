# cmake/CompilerOptions.cmake
# Applies strict compiler warnings. Sanitizers are enabled in Debug builds.

if(MSVC)
  add_compile_options(/W4 /WX /permissive-)
else()
  # Common warnings for all build types
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wcast-align
    -Wunused
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wno-unused-parameter   # too noisy in early development
  )

  # Debug-only: AddressSanitizer + UndefinedBehaviorSanitizer
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds" ON)
    if(ENABLE_SANITIZERS)
      message(STATUS "Sanitizers: ASan + UBSan ENABLED")
      add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
      add_link_options(-fsanitize=address,undefined)
    endif()
  endif()

  # Release: full optimisation + LTO
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG -march=native)
    add_link_options(-flto)
  endif()
endif()
