cmake_minimum_required(VERSION 3.16)
project(lsmdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
enable_testing()

# --- Week 1 (Vinayak): RPC layer. If this file already exists in the repo,
# --- only ADD the three add_subdirectory() lines below.
add_subdirectory(proto)
add_subdirectory(rpc)
add_subdirectory(tests/rpc)

# --- Weeks 2-4 (Vinayak): storage layer pieces
add_subdirectory(storage)
add_subdirectory(benchmarks)
add_subdirectory(tests/storage)

# --- Weeks 5-6 (Vinayak): peer discovery and distributed write path
add_subdirectory(consensus)
add_subdirectory(tests/consensus)
