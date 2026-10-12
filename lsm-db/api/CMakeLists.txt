# Single-node storage API (Week 2). Depends only on the StorageBackend header,
# not on gRPC, so it builds and tests without the RPC stack.
add_library(lsmdb_api STATIC
  src/reference_memtable.cpp
  src/memtable_backend.cpp)
target_include_directories(lsmdb_api PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../rpc/include)
target_link_libraries(lsmdb_api PUBLIC Threads::Threads)
