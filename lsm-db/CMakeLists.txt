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
option(LSMDB_BUILD_RPC "Build the Protobuf/gRPC node and CLI" ON)
option(LSMDB_BUILD_TESTS "Build GoogleTest targets" ON)
option(LSMDB_USE_ATISHAY_SKIPLIST "Adapt the root project's SkipList for Suhani's versioned MemTable" OFF)
if(LSMDB_USE_ATISHAY_SKIPLIST AND NOT TARGET storage_lib)
  message(FATAL_ERROR "LSMDB_USE_ATISHAY_SKIPLIST is supported from the combined root CMake project")
endif()

# --- Week 2 (Suhani): single-node storage API over a MemTable.
add_subdirectory(api)
# --- Week 3 (Suhani): startup recovery (WAL replay into the MemTable).
add_subdirectory(recovery)
# --- Week 4 (Suhani): MemTable rotation, flush worker, compaction scheduling.
add_subdirectory(lifecycle)
# The cluster Raft implementation uses generated gRPC service types.
if(LSMDB_BUILD_RPC)
  add_subdirectory(proto)
endif()
# --- Week 5 (Suhani): replicated command format and the Raft/storage boundary.
add_subdirectory(consensus)
add_subdirectory(node)

if(LSMDB_BUILD_RPC)
  add_subdirectory(rpc)
endif()

if(LSMDB_BUILD_TESTS)
  add_subdirectory(tests/api)
  add_subdirectory(tests/recovery)
  add_subdirectory(tests/lifecycle)
  add_subdirectory(tests/consensus)
  add_subdirectory(tests/node)
  if(LSMDB_BUILD_RPC)
    add_subdirectory(tests/rpc)
  endif()
endif()
