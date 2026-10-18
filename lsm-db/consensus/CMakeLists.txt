# Consensus boundary (Week 5): replicated command format, validation, Raft/storage interfaces and a
# single-node stand-in. Depends on the api library only (header-only use of the StorageBackend
# interface); no gRPC or protobuf.
add_library(lsmdb_consensus STATIC
  src/command.cpp
  src/storage_state_machine.cpp
  src/reference_consensus.cpp
  src/replicated_backend.cpp)
target_include_directories(lsmdb_consensus PUBLIC include)
target_link_libraries(lsmdb_consensus PUBLIC lsmdb_api Threads::Threads)

if(LSMDB_BUILD_RPC)
  add_library(lsmdb_cluster_raft STATIC src/raft_node.cpp)
  target_include_directories(lsmdb_cluster_raft PUBLIC include)
  target_link_libraries(lsmdb_cluster_raft PUBLIC lsmdb_consensus lsmdb_proto Threads::Threads)
endif()
