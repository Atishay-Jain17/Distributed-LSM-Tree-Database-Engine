#include "lsmdb/rpc/kv_service_impl.h"

#include <exception>

#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"

namespace lsmdb::rpc {
namespace {
v1::Status Internal(const std::exception& e) {
  return MakeStatus(v1::STATUS_CODE_INTERNAL, std::string("storage failure: ") + e.what());
}
}  // namespace

KvServiceImpl::KvServiceImpl(std::shared_ptr<StorageBackend> backend,
                             std::shared_ptr<WriteHandler> write_handler)
    : backend_(std::move(backend)), write_handler_(std::move(write_handler)) {}

grpc::Status KvServiceImpl::Put(grpc::ServerContext*, const v1::PutRequest* req,
                                v1::PutResponse* resp) {
  if (auto s = ValidatePut(*req); !IsOk(s)) {
    *resp->mutable_status() = std::move(s);
    return grpc::Status::OK;
  }
  try {
    if (write_handler_) {
      *resp->mutable_status() = write_handler_->Put(req->key(), req->value());
    } else {
      backend_->Put(req->key(), req->value());
      *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_OK);
    }
  } catch (const std::exception& e) {
    *resp->mutable_status() = Internal(e);
  }
  return grpc::Status::OK;
}

grpc::Status KvServiceImpl::Get(grpc::ServerContext*, const v1::GetRequest* req,
                                v1::GetResponse* resp) {
  if (auto s = ValidateGet(*req); !IsOk(s)) {
    *resp->mutable_status() = std::move(s);
    return grpc::Status::OK;
  }
  try {
    if (auto value = backend_->Get(req->key())) {
      resp->set_value(std::move(*value));
      *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_OK);
    } else {
      *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_NOT_FOUND, "key not found");
    }
  } catch (const std::exception& e) {
    *resp->mutable_status() = Internal(e);
  }
  return grpc::Status::OK;
}

grpc::Status KvServiceImpl::Delete(grpc::ServerContext*, const v1::DeleteRequest* req,
                                   v1::DeleteResponse* resp) {
  if (auto s = ValidateDelete(*req); !IsOk(s)) {
    *resp->mutable_status() = std::move(s);
    return grpc::Status::OK;
  }
  try {
    if (write_handler_) {
      *resp->mutable_status() = write_handler_->Delete(req->key());
    } else {
      backend_->Delete(req->key());
      *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_OK);
    }
  } catch (const std::exception& e) {
    *resp->mutable_status() = Internal(e);
  }
  return grpc::Status::OK;
}

}  // namespace lsmdb::rpc
