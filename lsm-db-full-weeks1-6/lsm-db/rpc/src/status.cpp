#include "lsmdb/rpc/status.h"

namespace lsmdb::rpc {

v1::Status MakeStatus(v1::StatusCode code, std::string message) {
  v1::Status s;
  s.set_code(code);
  s.set_message(std::move(message));
  return s;
}

v1::Status FromGrpcStatus(const grpc::Status& s) {
  if (s.ok()) return MakeStatus(v1::STATUS_CODE_OK);
  std::string msg = "transport error: " + s.error_message();
  switch (s.error_code()) {
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return MakeStatus(v1::STATUS_CODE_UNAVAILABLE, msg);
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return MakeStatus(v1::STATUS_CODE_INVALID_ARGUMENT, msg);
    default:
      return MakeStatus(v1::STATUS_CODE_INTERNAL, msg);
  }
}

}  // namespace lsmdb::rpc
