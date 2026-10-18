#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "lsmdb/v1/node.grpc.pb.h"

namespace lsmdb::rpc {

// Ping + GetNodeInfo. Gives each process a verifiable identity.
class NodeServiceImpl final : public v1::NodeService::Service {
 public:
  NodeServiceImpl(v1::NodeInfo self, std::vector<v1::NodeInfo> peers);

  // Called once the real listening port is known (matters when port 0 was requested).
  void set_self_port(std::uint32_t port) { self_port_.store(port); }

  grpc::Status Ping(grpc::ServerContext*, const v1::PingRequest* req,
                    v1::PingResponse* resp) override;
  grpc::Status GetNodeInfo(grpc::ServerContext*, const v1::GetNodeInfoRequest* req,
                           v1::GetNodeInfoResponse* resp) override;

 private:
  v1::NodeInfo SelfInfo() const;

  v1::NodeInfo self_;
  std::vector<v1::NodeInfo> peers_;
  std::atomic<std::uint32_t> self_port_;
};

}  // namespace lsmdb::rpc
