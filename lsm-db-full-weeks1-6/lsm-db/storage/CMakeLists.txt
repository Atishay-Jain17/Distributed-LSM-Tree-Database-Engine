add_library(lsmdb_storage STATIC
  src/crc32.cpp
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
