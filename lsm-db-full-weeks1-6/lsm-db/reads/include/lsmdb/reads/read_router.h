#pragma once
#include <memory>
#include <string>

#include "lsmdb/reads/consistency_level.h"
#include "lsmdb/reads/leader_lease.h"
#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::reads {

struct ConsistentReadResult {
  v1::Status status;
  std::string value;  // populated if status is OK
};

class ReadRouter {
 public:
  ReadRouter(std::shared_ptr<rpc::StorageBackend> local_storage,
             std::shared_ptr<LeaderLeaseProvider> lease_provider,
             ConsistencyLevel level = ConsistencyLevel::kLinearizable);

  ConsistentReadResult Read(const std::string& key);

  ConsistencyLevel consistency_level() const { return level_; }
  void SetConsistencyLevel(ConsistencyLevel level) { level_ = level; }

 private:
  std::shared_ptr<rpc::StorageBackend> local_storage_;
  std::shared_ptr<LeaderLeaseProvider> lease_provider_;
  ConsistencyLevel level_;
};

}  // namespace lsmdb::reads
