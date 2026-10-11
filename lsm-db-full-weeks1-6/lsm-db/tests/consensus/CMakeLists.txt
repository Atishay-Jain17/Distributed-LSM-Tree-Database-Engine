find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(consensus_tests
  peer_discovery_test.cpp
  peer_manager_test.cpp
  ordered_applier_test.cpp
  replicated_write_handler_test.cpp
  cluster_integration_test.cpp)
target_link_libraries(consensus_tests PRIVATE lsmdb_consensus GTest::gtest_main)
gtest_discover_tests(consensus_tests)
