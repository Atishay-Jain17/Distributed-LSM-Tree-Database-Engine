add_library(lsmdb_storage STATIC
  src/crc32.cpp
  src/bloom_filter.cpp
  src/memtable.cpp
  src/sstable_format.cpp
  src/sstable_writer.cpp
  src/sstable_reader.cpp
  src/flush.cpp
  src/file_meta.cpp
  src/version.cpp
  src/version_set.cpp
  src/compaction_picker.cpp
  src/compaction_job.cpp
  src/compaction_manager.cpp)
target_include_directories(lsmdb_storage PUBLIC include)
target_link_libraries(lsmdb_storage PUBLIC Threads::Threads)

# Atishay's Week 4 sparse-index implementation lives in the repository root.
# Keep the imported storage subproject self-contained when configured directly
# from this checkout, while reusing the same implementation in the root build.
get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_include_directories(lsmdb_storage PUBLIC "${REPOSITORY_ROOT}/include")
if(TARGET storage_lib AND TARGET common_lib)
  target_link_libraries(lsmdb_storage PUBLIC storage_lib common_lib)
else()
  target_sources(lsmdb_storage PRIVATE
    "${REPOSITORY_ROOT}/src/common/status.cpp"
    "${REPOSITORY_ROOT}/src/storage/sparse_index.cpp"
    "${REPOSITORY_ROOT}/src/storage/storage_metrics.cpp")
endif()
