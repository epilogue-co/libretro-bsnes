# Standalone build for the cross-platform determinism tracer. Self-contained — vendored here so the
# CI determinism jobs build it with a bare `cmake -S determinism/hashtrace -B <build>` in seconds,
# no external repo, no Qt/SDL/Vulkan. Needs only a C++20 compiler + dlopen/LoadLibrary; the three
# headers it includes (libretro.h, CoreDeterminismOptions.h, xxhash.h) sit alongside this file.
cmake_minimum_required(VERSION 3.20)
project(libretro-hashtrace CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(libretro-hashtrace main.cpp)
target_include_directories(libretro-hashtrace PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# dlopen lives in libdl on Linux; macOS/Windows resolve it from the system C runtime automatically.
if(UNIX AND NOT APPLE)
  target_link_libraries(libretro-hashtrace PRIVATE ${CMAKE_DL_LIBS})
endif()
