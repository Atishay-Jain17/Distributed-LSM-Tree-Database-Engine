find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(rpc_tests
  validation_test.cpp
  serialization_test.cpp
  node_options_test.cpp
  kv_service_test.cpp
  node_service_test.cpp)
target_link_libraries(rpc_tests PRIVATE lsmdb_rpc GTest::gtest_main)
gtest_discover_tests(rpc_tests)
