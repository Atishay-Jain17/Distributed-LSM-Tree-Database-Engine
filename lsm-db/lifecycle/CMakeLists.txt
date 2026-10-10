# Flush lifecycle + single-node LSM store (Week 4). Depends on the api library only (no gRPC).
add_library(lsmdb_lifecycle STATIC
  src/lsm_store.cpp
  src/reference_table_store.cpp)
target_include_directories(lsmdb_lifecycle PUBLIC include)
target_link_libraries(lsmdb_lifecycle PUBLIC lsmdb_api)
