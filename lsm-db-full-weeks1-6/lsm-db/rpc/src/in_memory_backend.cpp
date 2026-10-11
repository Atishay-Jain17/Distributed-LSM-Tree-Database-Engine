#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::rpc {

void InMemoryBackend::Put(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mu_);
  data_[key] = value;
}

std::optional<std::string> InMemoryBackend::Get(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = data_.find(key);
  if (it == data_.end()) return std::nullopt;
  return it->second;
}

void InMemoryBackend::Delete(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  data_.erase(key);
}

}  // namespace lsmdb::rpc
