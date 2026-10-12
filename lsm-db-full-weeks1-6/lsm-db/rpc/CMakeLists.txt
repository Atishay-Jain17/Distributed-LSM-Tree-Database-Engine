add_library(lsmdb_rpc STATIC
  src/status.cpp
  src/validation.cpp
  src/in_memory_backend.cpp
  src/kv_service_impl.cpp
  src/node_service_impl.cpp
  src/node_options.cpp
  src/node_server.cpp
  src/clients.cpp
  src/cluster_client.cpp)
target_include_directories(lsmdb_rpc PUBLIC include)
target_link_libraries(lsmdb_rpc PUBLIC lsmdb_proto Threads::Threads)

add_executable(lsmdb_node node_main.cpp)
target_link_libraries(lsmdb_node PRIVATE lsmdb_rpc lsmdb_consensus)

add_executable(kv_cli tools/kv_cli.cpp)
target_link_libraries(kv_cli PRIVATE lsmdb_rpc)
