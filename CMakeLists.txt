cmake_minimum_required(VERSION 3.20)
project(Distributed-LSM-Tree-Database-Engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Compiler warning flags
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# Common Library
# -----------------------------------------------------------------------------
add_library(common_lib STATIC
    src/common/status.cpp
    src/common/config.cpp
)
target_include_directories(common_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# Storage Engine Library (Weeks 1-4 Atishay scope)
# -----------------------------------------------------------------------------
add_library(storage_lib STATIC
    src/storage/basic_storage_engine.cpp
    src/storage/memtable.cpp
    src/storage/wal.cpp
    src/storage/sparse_index.cpp
    src/storage/storage_metrics.cpp
)
target_include_directories(storage_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(storage_lib PUBLIC common_lib)

# The imported main-branch SSTable implementation uses POSIX pread/fsync.
# Include it on its intended Linux platform; the root demo remains portable.
if(UNIX)
    find_package(Threads REQUIRED)
    add_subdirectory(lsm-db-full-weeks1-6/lsm-db/storage)
endif()


# -----------------------------------------------------------------------------
# Consensus Library (Weeks 5-6 Atishay scope)
# -----------------------------------------------------------------------------
add_library(consensus_lib STATIC
    src/consensus/raft_state.cpp
    src/consensus/raft_log.cpp
)
target_include_directories(consensus_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(consensus_lib PUBLIC common_lib storage_lib)

# -----------------------------------------------------------------------------
# LSM Node Daemon Executable
# -----------------------------------------------------------------------------
add_executable(lsm_node
    src/main.cpp
)
target_link_libraries(lsm_node PRIVATE storage_lib consensus_lib common_lib)

# -----------------------------------------------------------------------------
# Local TCP demo (separate server and client terminals)
# -----------------------------------------------------------------------------
add_executable(lsm_server src/server.cpp)
target_link_libraries(lsm_server PRIVATE storage_lib common_lib)

add_executable(lsm_client src/client.cpp)
target_include_directories(lsm_client PRIVATE ${PROJECT_SOURCE_DIR}/include)
if (WIN32)
    target_link_libraries(lsm_server PRIVATE ws2_32)
    target_link_libraries(lsm_client PRIVATE ws2_32)
endif()

# -----------------------------------------------------------------------------
# Testing & Verification
# -----------------------------------------------------------------------------
enable_testing()

add_executable(lsm_unit_tests
    tests/test_main.cpp
    tests/test_storage.cpp
    tests/test_config.cpp
    tests/test_skiplist.cpp
    tests/test_memtable.cpp
    tests/test_wal.cpp
    tests/test_sparse_index.cpp
    tests/test_raft_election.cpp
    tests/test_raft_log.cpp
    tests/test_metrics.cpp
)
target_include_directories(lsm_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(lsm_unit_tests PRIVATE consensus_lib storage_lib common_lib)

add_executable(lsm_smoke_test
    tests/smoke_test.cpp
)
target_include_directories(lsm_smoke_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(lsm_smoke_test PRIVATE storage_lib common_lib)

add_test(NAME UnitTests COMMAND lsm_unit_tests)
add_test(NAME SmokeTest COMMAND lsm_smoke_test)

if(UNIX)
    target_sources(lsm_unit_tests PRIVATE tests/test_imported_sstable_index.cpp)
    target_link_libraries(lsm_unit_tests PRIVATE lsmdb_storage)
endif()

# -----------------------------------------------------------------------------
# Benchmarks (Weeks 2 & 4 Atishay scope)
# -----------------------------------------------------------------------------
add_executable(lsm_memtable_benchmark
    benchmarks/memtable_benchmark.cpp
)
target_include_directories(lsm_memtable_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(lsm_memtable_benchmark PRIVATE storage_lib common_lib)

add_executable(lsm_sparse_index_benchmark
    benchmarks/sparse_index_benchmark.cpp
)
target_include_directories(lsm_sparse_index_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(lsm_sparse_index_benchmark PRIVATE storage_lib common_lib)

# Week 8 repeatable storage read/write throughput and P99 benchmark.
add_executable(lsm_storage_benchmark benchmarks/storage_benchmark.cpp)
target_link_libraries(lsm_storage_benchmark PRIVATE storage_lib common_lib)
