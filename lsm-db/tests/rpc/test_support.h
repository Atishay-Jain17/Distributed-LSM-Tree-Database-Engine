#pragma once
// Starts a real NodeServer on an ephemeral port (inside the test process) and
// gives out clients for it. Tests therefore exercise real gRPC + protobuf.
#include <memory>
#include <string>
#include <vector>

#include "lsmdb/rpc/clients.h"
#include "lsmdb/rpc/node_server.h"

namespace lsmdb::testing_support {

class TestNode {
 public:
  explicit TestNode(std::uint32_t node_id, std::vector<v1::NodeInfo> peers = {},
                    std::shared_ptr<rpc::StorageBackend> backend = nullptr) {
    rpc::NodeOptions opts;
    opts.node_id = node_id;
    opts.advertise_host = "127.0.0.1";
    opts.port = 0;  // ephemeral
    opts.peers = std::move(peers);
    if (!backend) backend = std::make_shared<rpc::InMemoryBackend>();
    server_ = std::make_unique<rpc::NodeServer>(std::move(opts), std::move(backend));
    server_->Start();
  }
  ~TestNode() { server_->Shutdown(); }

  std::string target() const { return "127.0.0.1:" + std::to_string(server_->port()); }
  std::uint32_t port() const { return server_->port(); }
  std::shared_ptr<grpc::Channel> channel() const { return rpc::MakeChannel(target()); }

 private:
  std::unique_ptr<rpc::NodeServer> server_;
};

inline v1::NodeInfo MakeNodeInfo(std::uint32_t id, const std::string& host, std::uint32_t port) {
  v1::NodeInfo n;
  n.set_node_id(id);
  n.set_host(host);
  n.set_port(port);
  return n;
}

}  // namespace lsmdb::testing_support
