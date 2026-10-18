#pragma once
#include <cstdint>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "lsmdb/rpc/kv_service_impl.h"
#include "lsmdb/rpc/node_options.h"
#include "lsmdb/rpc/node_service_impl.h"
#include "lsmdb/rpc/storage_backend.h"
#include "lsmdb/rpc/write_handler.h"

namespace lsmdb::rpc {

// One database node process: a gRPC server hosting KvService and NodeService.
class NodeServer {
 public:
  // `write_handler` is optional: pass the consensus layer's handler to replicate writes.
  NodeServer(NodeOptions options, std::shared_ptr<StorageBackend> backend,
             std::shared_ptr<WriteHandler> write_handler = nullptr);
  ~NodeServer();
  NodeServer(const NodeServer&) = delete;
  NodeServer& operator=(const NodeServer&) = delete;

  // Binds 0.0.0.0:<port> and starts serving. Throws std::runtime_error on failure
  // (for example the port is already in use).
  void Start();
  void Wait();      // blocks until Shutdown()
  void Shutdown();  // safe to call more than once

  std::uint32_t port() const { return bound_port_; }  // valid after Start()
  const NodeOptions& options() const { return options_; }

 private:
  NodeOptions options_;
  KvServiceImpl kv_service_;
  NodeServiceImpl node_service_;
  std::unique_ptr<grpc::Server> server_;
  std::uint32_t bound_port_ = 0;
};

}  // namespace lsmdb::rpc
