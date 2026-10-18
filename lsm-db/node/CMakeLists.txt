add_library(lsmdb_node_storage STATIC src/persistent_backend.cpp)
target_include_directories(lsmdb_node_storage PUBLIC include)
target_link_libraries(lsmdb_node_storage PUBLIC lsmdb_lifecycle lsmdb_recovery)
if(LSMDB_USE_ATISHAY_SKIPLIST)
  target_link_libraries(lsmdb_node_storage PUBLIC storage_lib)
endif()
if(TARGET lsmdb_storage)
  target_sources(lsmdb_node_storage PRIVATE src/atishay_sstable_table_store.cpp)
  target_link_libraries(lsmdb_node_storage PUBLIC lsmdb_storage)
  target_compile_definitions(lsmdb_node_storage PUBLIC LSMDB_HAS_ATISHAY_SSTABLE=1)
endif()

# Portable single-node CLI backed by the persistent WAL + LSM lifecycle. This
# remains available even when the optional gRPC node is not built.
add_executable(lsmdb_local_cli persistent_cli.cpp)
target_link_libraries(lsmdb_local_cli PRIVATE lsmdb_node_storage)

if(NOT LSMDB_BUILD_TESTS)
  add_subdirectory(../tests/node ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
