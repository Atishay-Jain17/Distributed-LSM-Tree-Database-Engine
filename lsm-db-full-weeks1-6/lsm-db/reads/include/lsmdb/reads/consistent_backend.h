#pragma once
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "lsmdb/reads/read_router.h"
#include "lsmdb/rpc/storage_backend.h"

#include "lsmdb/rpc/status.h"

namespace lsmdb::reads {

// Thrown when a read hits a follower under linearizable consistency.
struct ReadNotLeaderException : public rpc::NotLeaderException {
  explicit ReadNotLeaderException(std::optional<v1::NodeInfo> hint)
      : rpc::NotLeaderException("cannot serve read: not the leader", std::move(hint)) {}
};

// StorageBackend adapter ensuring Get() operations obey configured consistency guarantees.
class ConsistentStorageBackend : public rpc::StorageBackend {
 public:
  ConsistentStorageBackend(std::shared_ptr<rpc::StorageBackend> underlying_storage,
                           std::shared_ptr<LeaderLeaseProvider> lease_provider,
                           ConsistencyLevel level = ConsistencyLevel::kLinearizable);

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;

  ReadRouter& router() { return router_; }
  const ReadRouter& router() const { return router_; }

 private:
  std::shared_ptr<rpc::StorageBackend> storage_;
  ReadRouter router_;
};

}  // namespace lsmdb::reads
