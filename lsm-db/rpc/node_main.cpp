// lsmdb_node: one database node process.
//
//   lsmdb_node --node-id 1 --port 50051 --peers 2=127.0.0.1:50052,3=127.0.0.1:50053
//
// Every flag can also come from an environment variable (used by Kubernetes):
//   LSMDB_NODE_ID, LSMDB_PORT, LSMDB_ADVERTISE_HOST, LSMDB_PEERS
// Flags win over environment variables.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include "lsmdb/rpc/node_options.h"
#include "lsmdb/rpc/node_server.h"
#include "lsmdb/rpc/storage_backend.h"
#include "lsmdb/node/persistent_backend.h"
#include "lsmdb/consensus/replicated_backend.h"
#include "lsmdb/consensus/raft_node.h"
#include "lsmdb/consensus/storage_state_machine.h"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

void Usage() {
  std::cerr << "usage: lsmdb_node --node-id N --port P [--host H] [--peers id=host:port,...] [--data-dir DIR]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::map<std::string, std::string> flags;
  const std::set<std::string> supported_flags = {"--node-id", "--port", "--host", "--peers", "--data-dir"};
  for (int i = 1; i < argc; i += 2) {
    std::string name = argv[i];
    if (name == "--help" || name == "-h" || i + 1 >= argc || name.rfind("--", 0) != 0) {
      Usage();
      return name == "--help" || name == "-h" ? 0 : 2;
    }
    if (!supported_flags.count(name) || flags.count(name)) {
      Usage();
      return 2;
    }
    flags[name] = argv[i + 1];
  }
  auto get = [&](const std::string& flag, const char* env, const std::string& def) {
    if (auto it = flags.find(flag); it != flags.end()) return it->second;
    std::string e = Env(env);
    return e.empty() ? def : e;
  };

  try {
    lsmdb::rpc::NodeOptions opts;
    opts.node_id = lsmdb::rpc::ParseUint32Argument(get("--node-id", "LSMDB_NODE_ID", "0"), "node id");
    opts.port = lsmdb::rpc::ParseUint32Argument(get("--port", "LSMDB_PORT", "0"), "port");
    opts.advertise_host = get("--host", "LSMDB_ADVERTISE_HOST", "127.0.0.1");
    opts.peers = lsmdb::rpc::ParsePeers(get("--peers", "LSMDB_PEERS", ""));
    lsmdb::rpc::ValidateNodeOptions(opts);
    const std::string default_data_dir = "data/node-" + std::to_string(opts.node_id);
    const std::string data_dir = get("--data-dir", "LSMDB_DATA_DIR", default_data_dir);
    if (data_dir.empty()) throw std::invalid_argument("data directory must not be empty");

    auto storage = lsmdb::node::PersistentBackend::Open(data_dir);
    const auto& recovery = storage->recovery_stats();
    std::cout << "storage recovered records=" << recovery.records_applied
              << " skipped_flushed=" << recovery.records_skipped
              << " torn_tail=" << (recovery.dropped_torn_tail ? "yes" : "no")
              << " data_dir=" << storage->data_dir().string() << std::endl;
    auto state_machine = std::make_shared<lsmdb::consensus::StorageStateMachine>(storage);
    auto raft = std::make_shared<lsmdb::consensus::RaftNode>(
        opts.node_id, opts.peers, state_machine, data_dir, storage->store().LastSequence());
    auto backend = std::make_shared<lsmdb::consensus::ReplicatedBackend>(raft, storage);
    lsmdb::rpc::NodeServer server(opts, backend, raft);
    server.Start();
    raft->Start();

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << "lsmdb node " << opts.node_id << " listening on 0.0.0.0:" << server.port()
              << " (peers: " << opts.peers.size() << ")" << std::endl;

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "lsmdb node " << opts.node_id << " shutting down" << std::endl;
    server.Shutdown();
    raft->Stop();
    storage->Flush();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << std::endl;
    return 1;
  }
}
