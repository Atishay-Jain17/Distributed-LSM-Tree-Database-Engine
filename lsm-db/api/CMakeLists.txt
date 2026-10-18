# Single-node storage API (Week 2). Depends only on the StorageBackend header,
# not on gRPC, so it builds and tests without the RPC stack.
add_library(lsmdb_api STATIC
  src/reference_memtable.cpp
  src/memtable_backend.cpp)
target_include_directories(lsmdb_api PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../rpc/include)
target_link_libraries(lsmdb_api PUBLIC Threads::Threads)
if(LSMDB_USE_ATISHAY_SKIPLIST)
  if(NOT LSMDB_ATISHAY_INCLUDE_DIR OR NOT EXISTS "${LSMDB_ATISHAY_INCLUDE_DIR}/storage/skiplist.h")
    message(FATAL_ERROR "LSMDB_USE_ATISHAY_SKIPLIST requires LSMDB_ATISHAY_INCLUDE_DIR/storage/skiplist.h")
  endif()
  target_sources(lsmdb_api PRIVATE src/atishay_skiplist_memtable.cpp)
  target_include_directories(lsmdb_api PUBLIC ${LSMDB_ATISHAY_INCLUDE_DIR})
  target_compile_definitions(lsmdb_api PUBLIC LSMDB_HAS_ATISHAY_SKIPLIST=1 LSMDB_HAS_ATISHAY_WAL=1)
endif()
