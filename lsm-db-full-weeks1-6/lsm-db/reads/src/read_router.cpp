#include "lsmdb/reads/read_router.h"

namespace lsmdb::reads {

ReadRouter::ReadRouter(std::shared_ptr<rpc::StorageBackend> local_storage,
                       std::shared_ptr<LeaderLeaseProvider> lease_provider,
                       ConsistencyLevel level)
    : local_storage_(std::move(local_storage)),
      lease_provider_(std::move(lease_provider)),
      level_(level) {}

ConsistentReadResult ReadRouter::Read(const std::string& key) {
  ConsistentReadResult result;
  if (!local_storage_) {
    result.status = rpc::MakeStatus(v1::STATUS_CODE_INTERNAL, "storage backend not configured");
    return result;
  }

  if (level_ == ConsistencyLevel::kStaleTolerant) {
    auto val = local_storage_->Get(key);
    if (!val) {
      result.status = rpc::MakeStatus(v1::STATUS_CODE_NOT_FOUND, "key not found");
      return result;
    }
    result.status = rpc::MakeStatus(v1::STATUS_CODE_OK);
    result.value = *val;
    return result;
  }

  // Linearizable or Sequential: Must check leadership
  if (!lease_provider_) {
    result.status = rpc::MakeStatus(v1::STATUS_CODE_INTERNAL, "lease provider not configured");
    return result;
  }

  LeadershipInfo info = lease_provider_->GetLeadershipInfo();
  if (!info.is_leader) {
    result.status = rpc::MakeStatus(v1::STATUS_CODE_NOT_LEADER, "not the leader");
    if (info.leader_hint) {
      *result.status.mutable_leader_hint() = *info.leader_hint;
    }
    return result;
  }

  if (level_ == ConsistencyLevel::kLinearizable) {
    // Confirm leadership with quorum
    if (!lease_provider_->ConfirmQuorum()) {
      result.status = rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE,
                                      "leadership could not be verified by quorum");
      return result;
    }
  }

  // Safe to read local committed state
  try {
    auto val = local_storage_->Get(key);
    if (!val) {
      result.status = rpc::MakeStatus(v1::STATUS_CODE_NOT_FOUND, "key not found");
      return result;
    }
    result.status = rpc::MakeStatus(v1::STATUS_CODE_OK);
    result.value = *val;
  } catch (const std::exception& e) {
    result.status = rpc::MakeStatus(v1::STATUS_CODE_INTERNAL, e.what());
  }
  return result;
}

}  // namespace lsmdb::reads
