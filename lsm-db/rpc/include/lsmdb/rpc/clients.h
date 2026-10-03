#pragma once
// Thin client wrappers. Used by kv_cli, tests, and (from Week 5) node-to-node calls.
// Every method returns a v1::Status: transport failures are converted with
// FromGrpcStatus, so callers handle a single error model.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "lsmdb/v1/kv.grpc.pb.h"
#include "lsmdb/v1/node.grpc.pb.h"

namespace lsmdb::rpc {

std::shared_ptr<grpc::Channel> MakeChannel(const std::string& target);  // "host:port"

struct ClientOptions {
  std::chrono::milliseconds deadline{5000};
  std::uint32_t sender_node_id = 0;  // 0 = external client
};

class KvClient {
 public:
  struct GetResult {
    v1::Status status;
    std::string value;
  };

  explicit KvClient(std::shared_ptr<grpc::Channel> channel, ClientOptions options = {});

  v1::Status Put(const std::string& key, const std::string& value);
  GetResult Get(const std::string& key);
  v1::Status Delete(const std::string& key);

 private:
  v1::RequestMeta NextMeta();
  void Prepare(grpc::ClientContext& ctx) const;

  std::unique_ptr<v1::KvService::Stub> stub_;
  ClientOptions options_;
  std::atomic<std::uint64_t> counter_{0};
};

class NodeClient {
 public:
  struct PingResult {
    v1::Status status;
    v1::NodeInfo responder;
  };
  struct InfoResult {
    v1::Status status;
    v1::NodeInfo self;
    std::vector<v1::NodeInfo> peers;
  };

  explicit NodeClient(std::shared_ptr<grpc::Channel> channel, ClientOptions options = {});

  PingResult Ping();
  InfoResult GetNodeInfo();

 private:
  v1::RequestMeta NextMeta();
  void Prepare(grpc::ClientContext& ctx) const;

  std::unique_ptr<v1::NodeService::Stub> stub_;
  ClientOptions options_;
  std::atomic<std::uint64_t> counter_{0};
};

}  // namespace lsmdb::rpc
