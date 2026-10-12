#include "lsmdb/rpc/node_server.h"

#include <chrono>
#include <stdexcept>

#include "lsmdb/rpc/validation.h"

namespace lsmdb::rpc {
namespace {
v1::NodeInfo MakeSelf(const NodeOptions& o) {
  v1::NodeInfo self;
  self.set_node_id(o.node_id);
  self.set_host(o.advertise_host);
  self.set_port(o.port);
  return self;
}
}  // namespace

NodeServer::NodeServer(NodeOptions options, std::shared_ptr<StorageBackend> backend)
    : options_(std::move(options)),
      kv_service_(std::move(backend)),
      node_service_(MakeSelf(options_), options_.peers) {}

NodeServer::~NodeServer() { Shutdown(); }

void NodeServer::Start() {
  ValidateNodeOptions(options_, /*allow_ephemeral_port=*/true);

  int selected_port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(options_.port),
                           grpc::InsecureServerCredentials(), &selected_port);
  // gRPC enables SO_REUSEPORT by default on Linux, which would let two nodes
  // silently share one port. Turn it off so a port clash fails loudly.
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder.SetMaxSendMessageSize(kMaxMessageBytes);
  builder.RegisterService(&kv_service_);
  builder.RegisterService(&node_service_);

  server_ = builder.BuildAndStart();
  if (!server_ || selected_port == 0) {
    server_.reset();
    throw std::runtime_error("node " + std::to_string(options_.node_id) +
                             ": failed to bind port " + std::to_string(options_.port));
  }
  bound_port_ = static_cast<std::uint32_t>(selected_port);
  node_service_.set_self_port(bound_port_);
}

void NodeServer::Wait() {
  if (server_) server_->Wait();
}

void NodeServer::Shutdown() {
  if (server_) {
    server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  }
}

}  // namespace lsmdb::rpc
