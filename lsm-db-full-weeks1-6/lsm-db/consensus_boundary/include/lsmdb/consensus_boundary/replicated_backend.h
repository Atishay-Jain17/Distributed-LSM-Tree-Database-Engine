#pragma once
// The KV layer's view of consensus (Week 5): a StorageBackend whose writes go through a Proposer.
//
//   Put/Delete -> Propose(Command) -> wait until applied locally -> return
//   Get        -> read local storage (see docs/consensus-boundary.md for read consistency)
//
// Drop-in for node_main.cpp: pass it to NodeServer instead of the plain storage backend.
#include <chrono>
#include <memory>
#include <stdexcept>

#include "lsmdb/consensus_boundary/boundary.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::consensus_boundary {

// Thrown by Put/Delete on a node that is not the leader. The RPC layer should turn it into
// STATUS_CODE_NOT_LEADER with `leader_hint` (today kv_service_impl reports unknown exceptions as
// INTERNAL; mapping this exception is a small follow-up there).
struct NotLeaderError : std::runtime_error {
  explicit NotLeaderError(std::uint32_t hint)
      : std::runtime_error("not the leader"), leader_hint(hint) {}
  std::uint32_t leader_hint;  // 0 = unknown
};

class ReplicatedBackend : public rpc::StorageBackend {
 public:
  // `reader` is the same storage the StorageStateMachine writes to.
  ReplicatedBackend(std::shared_ptr<Proposer> proposer, std::shared_ptr<rpc::StorageBackend> reader,
                    std::chrono::milliseconds apply_timeout = std::chrono::seconds(5));

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;

 private:
  void Replicate(const Command& command);

  std::shared_ptr<Proposer> proposer_;
  std::shared_ptr<rpc::StorageBackend> reader_;
  std::chrono::milliseconds timeout_;
};

}  // namespace lsmdb::consensus_boundary
