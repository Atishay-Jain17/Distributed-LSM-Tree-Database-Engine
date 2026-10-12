find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(api_tests
  memtable_test.cpp
  memtable_backend_test.cpp)
target_link_libraries(api_tests PRIVATE lsmdb_api GTest::gtest_main)
gtest_discover_tests(api_tests)

# End-to-end through gRPC; only built when the RPC library is part of the build.
if(TARGET lsmdb_rpc)
  add_executable(api_grpc_tests kv_api_grpc_test.cpp)
  target_link_libraries(api_grpc_tests PRIVATE lsmdb_api lsmdb_rpc GTest::gtest_main)
  gtest_discover_tests(api_grpc_tests)
endif()
