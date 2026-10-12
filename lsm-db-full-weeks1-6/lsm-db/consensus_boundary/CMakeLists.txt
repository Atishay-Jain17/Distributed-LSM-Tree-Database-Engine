# Consensus boundary (Week 5): replicated command format, validation, Raft/storage interfaces and a
# single-node stand-in. Depends on the api library only (header-only use of the StorageBackend
# interface); no gRPC or protobuf.
add_library(lsmdb_consensus_boundary STATIC
  src/command.cpp
  src/storage_state_machine.cpp
  src/reference_consensus.cpp
  src/replicated_backend.cpp)
target_include_directories(lsmdb_consensus_boundary PUBLIC include)
target_link_libraries(lsmdb_consensus_boundary PUBLIC lsmdb_api lsmdb_proto Threads::Threads)
