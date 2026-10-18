#pragma once
// STAND-IN for Raft (Week 5): a single-node "consensus" with a real log and real commit/apply
// ordering, but no replication or elections. It lets the boundary, validation and the KV path be
// built and tested before Atishay's Raft node exists. When the real one lands, it implements the
// same Proposer interface and drives the same StateMachine.
//
// Behaviour:
//  * Propose() appends to the log; with auto_commit (default) it also commits and applies at once.
//  * With auto_commit=false entries stay PROPOSED (in the log, not applied) until CommitUpTo() /
//    CommitAll(); this models "proposed but not committed yet" for tests.
//  * Commit applies entries to the StateMachine strictly in log order.
//  * If the state machine throws while applying, consensus stops (kStopped) and the entry stays
//    uncommitted, mirroring "a replica must not skip an entry it could not apply".
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmdb/consensus/boundary.h"

namespace lsmdb::consensus {

struct ReferenceConsensusOptions {
  bool auto_commit = true;
  bool is_leader = true;
  std::uint32_t leader_hint = 0;  // reported by Propose() when !is_leader
};

class ReferenceConsensus : public Proposer {
 public:
  ReferenceConsensus(std::shared_ptr<StateMachine> state_machine,
                     ReferenceConsensusOptions options = {});

  ProposeResult Propose(const Command& command) override;
  bool WaitApplied(LogIndex index, std::chrono::milliseconds timeout) override;
  ReadBarrierResult ReadBarrier(std::chrono::milliseconds timeout) override;

  // Commit and apply entries up to and including `index` (clamped to the log size).
  // Returns false if applying failed (consensus is then stopped).
  bool CommitUpTo(LogIndex index);
  bool CommitAll();

  void SetLeader(bool is_leader, std::uint32_t leader_hint = 0);
  void Stop();

  LogIndex LastIndex() const;
  LogIndex CommitIndex() const;
  bool stopped() const;
  std::string stop_reason() const;
  std::vector<std::string> LogSnapshot() const;  // payload bytes, index 1 first

 private:
  bool CommitLocked(LogIndex target, std::unique_lock<std::mutex>& lock);

  std::shared_ptr<StateMachine> sm_;
  ReferenceConsensusOptions opt_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::string> log_;   // log_[i] is the entry at index i+1
  LogIndex commit_index_ = 0;
  bool stopped_ = false;
  std::string stop_reason_;
};

}  // namespace lsmdb::consensus
