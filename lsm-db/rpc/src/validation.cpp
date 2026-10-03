#include "lsmdb/rpc/validation.h"

#include "lsmdb/rpc/status.h"

namespace lsmdb::rpc {
namespace {
v1::Status Invalid(std::string msg) {
  return MakeStatus(v1::STATUS_CODE_INVALID_ARGUMENT, std::move(msg));
}
v1::Status Ok() { return MakeStatus(v1::STATUS_CODE_OK); }
}  // namespace

v1::Status ValidateMeta(const v1::RequestMeta& meta) {
  if (meta.request_id().size() > kMaxRequestIdBytes) {
    return Invalid("request_id exceeds " + std::to_string(kMaxRequestIdBytes) + " bytes");
  }
  return Ok();
}

v1::Status ValidateKey(const std::string& key) {
  if (key.empty()) return Invalid("key must not be empty");
  if (key.size() > kMaxKeyBytes) {
    return Invalid("key exceeds " + std::to_string(kMaxKeyBytes) + " bytes (got " +
                   std::to_string(key.size()) + ")");
  }
  return Ok();
}

v1::Status ValidatePut(const v1::PutRequest& req) {
  if (auto s = ValidateMeta(req.meta()); !IsOk(s)) return s;
  if (auto s = ValidateKey(req.key()); !IsOk(s)) return s;
  if (req.value().size() > kMaxValueBytes) {
    return Invalid("value exceeds " + std::to_string(kMaxValueBytes) + " bytes (got " +
                   std::to_string(req.value().size()) + ")");
  }
  return Ok();
}

v1::Status ValidateGet(const v1::GetRequest& req) {
  if (auto s = ValidateMeta(req.meta()); !IsOk(s)) return s;
  return ValidateKey(req.key());
}

v1::Status ValidateDelete(const v1::DeleteRequest& req) {
  if (auto s = ValidateMeta(req.meta()); !IsOk(s)) return s;
  return ValidateKey(req.key());
}

}  // namespace lsmdb::rpc
