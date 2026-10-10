# Startup recovery (Week 3). Depends on the api library only (no gRPC).
add_library(lsmdb_recovery STATIC
  src/recovery.cpp
  src/reference_wal.cpp)
target_include_directories(lsmdb_recovery PUBLIC include)
target_link_libraries(lsmdb_recovery PUBLIC lsmdb_api)
