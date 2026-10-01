cmake_minimum_required(VERSION 3.20)
project(Distributed-LSM-Tree-Database-Engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Compiler warning flags
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# Common Library
# -----------------------------------------------------------------------------
add_library(common_lib STATIC
    src/common/status.cpp
    src/common/config.cpp
)
target_include_directories(common_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# Storage Engine Library
# -----------------------------------------------------------------------------
add_library(storage_lib STATIC
    src/storage/basic_storage_engine.cpp
)
target_include_directories(storage_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(storage_lib PUBLIC common_lib)

# -----------------------------------------------------------------------------
# LSM Node Daemon Executable
# -----------------------------------------------------------------------------
add_executable(lsm_node
    src/main.cpp
)
target_link_libraries(lsm_node PRIVATE storage_lib common_lib)

# -----------------------------------------------------------------------------
# Testing & Verification
# -----------------------------------------------------------------------------
enable_testing()

add_executable(lsm_unit_tests
    tests/test_main.cpp
    tests/test_storage.cpp
    tests/test_config.cpp
)
target_include_directories(lsm_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(lsm_unit_tests PRIVATE storage_lib common_lib)

add_executable(lsm_smoke_test
    tests/smoke_test.cpp
)
target_include_directories(lsm_smoke_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(lsm_smoke_test PRIVATE storage_lib common_lib)

add_test(NAME UnitTests COMMAND lsm_unit_tests)
add_test(NAME SmokeTest COMMAND lsm_smoke_test)