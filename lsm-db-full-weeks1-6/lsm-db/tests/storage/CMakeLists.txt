find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(storage_tests
  memtable_test.cpp
  sstable_test.cpp
  compaction_test.cpp)
target_link_libraries(storage_tests PRIVATE lsmdb_storage GTest::gtest_main)
gtest_discover_tests(storage_tests)
