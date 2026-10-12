#include "lsmdb/reads/consistent_backend.h"

namespace lsmdb::reads {

ConsistentStorageBackend::ConsistentStorageBackend(
    std::shared_ptr<rpc::StorageBackend> underlying_storage,
    std::shared_ptr<LeaderLeaseProvider> lease_provider,
    ConsistencyLevel level)
    : storage_(underlying_storage),
      router_(underlying_storage, lease_provider, level) {}

void ConsistentStorageBackend::Put(const std::string& key, const std::string& value) {
  if (!storage_) throw std::runtime_error("no underlying storage configured");
  storage_->Put(key, value);
}

void ConsistentStorageBackend::Delete(const std::string& key) {
  if (!storage_) throw std::runtime_error("no underlying storage configured");
  storage_->Delete(key);
}

std::optional<std::string> ConsistentStorageBackend::Get(const std::string& key) {
  ConsistentReadResult res = router_.Read(key);
  if (res.status.code() == v1::STATUS_CODE_NOT_LEADER) {
    std::optional<v1::NodeInfo> hint;
    if (res.status.has_leader_hint()) {
      hint = res.status.leader_hint();
    }
    throw ReadNotLeaderException(hint);
  }
  if (res.status.code() == v1::STATUS_CODE_NOT_FOUND) {
    return std::nullopt;
  }
  if (res.status.code() != v1::STATUS_CODE_OK) {
    throw std::runtime_error(res.status.message());
  }
  return res.value;
}

}  // namespace lsmdb::reads
