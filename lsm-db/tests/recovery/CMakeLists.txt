find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(recovery_tests recovery_test.cpp)
target_link_libraries(recovery_tests PRIVATE lsmdb_recovery GTest::gtest_main)
gtest_discover_tests(recovery_tests)
