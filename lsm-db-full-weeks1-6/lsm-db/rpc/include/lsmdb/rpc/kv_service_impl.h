#pragma once
#include <memory>

#include "lsmdb/rpc/storage_backend.h"
#include "lsmdb/rpc/write_handler.h"
#include "lsmdb/v1/kv.grpc.pb.h"

namespace lsmdb::rpc {

// Handles client PUT/GET/DELETE: validate -> call storage -> build Status.
// Application errors are returned in the response body; the gRPC status is
// always OK for requests that reached the handler.
//
// Writes go through `write_handler` when one is supplied (replicated mode), otherwise
// directly to `backend`. Reads always come from the local backend.
class KvServiceImpl final : public v1::KvService::Service {
 public:
  explicit KvServiceImpl(std::shared_ptr<StorageBackend> backend,
                         std::shared_ptr<WriteHandler> write_handler = nullptr);

  grpc::Status Put(grpc::ServerContext*, const v1::PutRequest* req, v1::PutResponse* resp) override;
  grpc::Status Get(grpc::ServerContext*, const v1::GetRequest* req, v1::GetResponse* resp) override;
  grpc::Status Delete(grpc::ServerContext*, const v1::DeleteRequest* req,
                      v1::DeleteResponse* resp) override;

 private:
  std::shared_ptr<StorageBackend> backend_;
  std::shared_ptr<WriteHandler> write_handler_;
};

}  // namespace lsmdb::rpc
