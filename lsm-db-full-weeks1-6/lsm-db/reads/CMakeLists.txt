add_library(lsmdb_reads STATIC
  src/read_router.cpp
  src/consistent_backend.cpp
  src/consistent_cluster_client.cpp
)

target_include_directories(lsmdb_reads PUBLIC include)
target_link_libraries(lsmdb_reads PUBLIC
  lsmdb_rpc
  lsmdb_proto
  lsmdb_api
  Threads::Threads
)
