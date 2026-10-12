find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(consensus_boundary_tests
  command_test.cpp
  boundary_test.cpp)
target_link_libraries(consensus_boundary_tests PRIVATE lsmdb_consensus_boundary lsmdb_lifecycle GTest::gtest_main)
gtest_discover_tests(consensus_boundary_tests)

# Over real gRPC; only built when the RPC library is part of the build.
if(TARGET lsmdb_rpc)
  add_executable(consensus_grpc_tests grpc_boundary_test.cpp)
  target_include_directories(consensus_grpc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../rpc)
  target_link_libraries(consensus_grpc_tests PRIVATE lsmdb_consensus_boundary lsmdb_rpc GTest::gtest_main)
  gtest_discover_tests(consensus_grpc_tests)
endif()
