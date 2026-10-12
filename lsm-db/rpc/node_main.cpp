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
#include <thread>

#include "lsmdb/rpc/node_options.h"
#include "lsmdb/rpc/node_server.h"
#include "lsmdb/rpc/storage_backend.h"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

void Usage() {
  std::cerr << "usage: lsmdb_node --node-id N --port P [--host H] [--peers id=host:port,...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::map<std::string, std::string> flags;
  for (int i = 1; i < argc; i += 2) {
    std::string name = argv[i];
    if (name == "--help" || name == "-h" || i + 1 >= argc || name.rfind("--", 0) != 0) {
      Usage();
      return name == "--help" || name == "-h" ? 0 : 2;
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
    opts.node_id = static_cast<std::uint32_t>(std::stoul(get("--node-id", "LSMDB_NODE_ID", "0")));
    opts.port = static_cast<std::uint32_t>(std::stoul(get("--port", "LSMDB_PORT", "0")));
    opts.advertise_host = get("--host", "LSMDB_ADVERTISE_HOST", "127.0.0.1");
    opts.peers = lsmdb::rpc::ParsePeers(get("--peers", "LSMDB_PEERS", ""));
    lsmdb::rpc::ValidateNodeOptions(opts);

    // TODO(integration): replace InMemoryBackend with the adapter over the real
    // storage engine once Atishay's storage API is merged (see storage_backend.h).
    lsmdb::rpc::NodeServer server(opts, std::make_shared<lsmdb::rpc::InMemoryBackend>());
    server.Start();

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << "lsmdb node " << opts.node_id << " listening on 0.0.0.0:" << server.port()
              << " (peers: " << opts.peers.size() << ")" << std::endl;

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "lsmdb node " << opts.node_id << " shutting down" << std::endl;
    server.Shutdown();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << std::endl;
    return 1;
  }
}
