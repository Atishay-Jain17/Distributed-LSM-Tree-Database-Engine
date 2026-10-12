add_library(lsmdb_consensus STATIC
  src/peer_discovery.cpp
  src/peer_manager.cpp
  src/command.cpp
  src/ordered_applier.cpp
  src/replicated_write_handler.cpp)
target_include_directories(lsmdb_consensus PUBLIC include)
target_link_libraries(lsmdb_consensus PUBLIC lsmdb_rpc Threads::Threads)
