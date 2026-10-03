#pragma once
// Helpers for building the protobuf Status carried in every response.
#include <string>

#include <grpcpp/support/status.h>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::rpc {

v1::Status MakeStatus(v1::StatusCode code, std::string message = {});

inline bool IsOk(const v1::Status& s) { return s.code() == v1::STATUS_CODE_OK; }

// Maps a transport-level gRPC failure to our Status so callers handle one error model.
//   UNAVAILABLE / DEADLINE_EXCEEDED          -> STATUS_CODE_UNAVAILABLE
//   INVALID_ARGUMENT / RESOURCE_EXHAUSTED    -> STATUS_CODE_INVALID_ARGUMENT
//   anything else                            -> STATUS_CODE_INTERNAL
v1::Status FromGrpcStatus(const grpc::Status& s);

}  // namespace lsmdb::rpc
