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

# --- Week 2 (Suhani): single-node storage API over a MemTable.
add_subdirectory(api)
add_subdirectory(tests/api)

# --- Week 3 (Suhani): startup recovery (WAL replay into the MemTable).
add_subdirectory(recovery)
add_subdirectory(tests/recovery)
