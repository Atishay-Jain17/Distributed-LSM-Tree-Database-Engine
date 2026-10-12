find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(read_tests
  read_consistency_test.cpp
)

target_include_directories(read_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/lsm-db-full-weeks1-6/lsm-db
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)

target_link_libraries(read_tests PRIVATE
  lsmdb_reads
  lsmdb_consensus
  lsmdb_rpc
  lsmdb_proto
  lsmdb_api
  GTest::gtest_main
  Threads::Threads
)

gtest_discover_tests(read_tests)
