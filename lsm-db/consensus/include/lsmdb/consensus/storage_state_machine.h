#pragma once
// Applies committed commands to a StorageBackend (MemTableBackend, LsmStore, ...).
#include <atomic>
#include <memory>

#include "lsmdb/consensus/boundary.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::consensus {

class StorageStateMachine : public StateMachine {
 public:
  // `last_applied`: highest log index already reflected in storage (0 for a new node; restored
  // from the Raft/storage metadata after a restart).
  explicit StorageStateMachine(std::shared_ptr<rpc::StorageBackend> storage, LogIndex last_applied = 0);

  // Called from ONE thread (Raft's apply loop), indexes strictly increasing by 1:
  //   index <= last applied  -> kSkippedDuplicate
  //   index >  last + 1      -> std::logic_error (a gap means Raft lost an entry)
  ApplyOutcome Apply(LogIndex index, const std::string& payload) override;
  LogIndex LastApplied() const override { return last_applied_.load(std::memory_order_acquire); }
  // Used once during Raft log recovery, before any serving/election threads start.
  void RestoreLastApplied(LogIndex index) override { last_applied_.store(index, std::memory_order_release); }

  // Counters for tests and metrics.
  std::uint64_t rejected() const { return rejected_.load(); }

 private:
  std::shared_ptr<rpc::StorageBackend> storage_;
  std::atomic<LogIndex> last_applied_;
  std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace lsmdb::consensus
