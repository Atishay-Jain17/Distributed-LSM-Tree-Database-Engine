#pragma once
// Request validation. Every function returns STATUS_CODE_OK or INVALID_ARGUMENT.
// The service layer calls these BEFORE touching storage, so storage only ever
// sees well-formed keys and values.
#include <cstddef>
#include <string>

#include "lsmdb/v1/kv.pb.h"

namespace lsmdb::rpc {

inline constexpr std::size_t kMaxKeyBytes = 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;  // 1 MiB
inline constexpr std::size_t kMaxRequestIdBytes = 128;
// gRPC transport cap: value limit + headroom for key/meta framing.
inline constexpr int kMaxMessageBytes = 2 * 1024 * 1024;

v1::Status ValidateMeta(const v1::RequestMeta& meta);
v1::Status ValidateKey(const std::string& key);
v1::Status ValidatePut(const v1::PutRequest& req);
v1::Status ValidateGet(const v1::GetRequest& req);
v1::Status ValidateDelete(const v1::DeleteRequest& req);

}  // namespace lsmdb::rpc
