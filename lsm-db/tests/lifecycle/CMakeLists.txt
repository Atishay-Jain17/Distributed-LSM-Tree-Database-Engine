find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(lifecycle_tests lifecycle_test.cpp single_node_flow_test.cpp)
target_link_libraries(lifecycle_tests PRIVATE lsmdb_lifecycle lsmdb_recovery GTest::gtest_main)
gtest_discover_tests(lifecycle_tests)
