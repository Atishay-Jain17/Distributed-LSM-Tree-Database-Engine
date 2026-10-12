#pragma once
// The two interfaces at the Raft / RPC / storage boundary (Week 5). See docs/consensus-boundary.md.
//
//   RPC layer ──Put/Delete──► ReplicatedBackend ──Propose(Command)──► Proposer (Raft leader side)
//                                                                          │ replicate, commit
//   Storage  ◄──Put/Delete── StorageStateMachine ◄──Apply(index, bytes)── Raft (every node)
//
// Atishay's Raft node implements Proposer and calls StateMachine::Apply for committed entries.
#include <chrono>
#include <cstdint>
#include <string>

#include "lsmdb/consensus/command.h"

namespace lsmdb::consensus {

using LogIndex = std::uint64_t;  // 1-based position in the replicated log; 0 = none

// ---- storage side ----------------------------------------------------------------------------

enum class ApplyOutcome {
  kApplied,         // decoded, validated and written to storage
  kRejectedMalformed,  // bytes do not decode; storage untouched; entry still consumed
  kRejectedInvalid,    // decodes but fails Validate(); storage untouched; entry still consumed
  kSkippedDuplicate,   // index <= last applied (e.g. replay after restart); nothing done
};

// Called by Raft for each COMMITTED entry, in log order, on every node (leader included).
class StateMachine {
 public:
  virtual ~StateMachine() = default;
  // `payload` is the entry bytes exactly as stored in the Raft log (Encode() output).
  // Rejections are deterministic, so every replica makes the same decision.
  // Throws if storage itself fails: the entry is then NOT counted as applied and Raft must retry
  // or stop; it must not skip it.
  virtual ApplyOutcome Apply(LogIndex index, const std::string& payload) = 0;
  virtual LogIndex LastApplied() const = 0;
};

// ---- proposal side ---------------------------------------------------------------------------

enum class ProposeStatus {
  kAccepted,    // entry is in the log at `index`; it is applied later, once committed
  kInvalid,     // command failed Validate(); nothing was logged
  kNotLeader,   // this node is not the leader; see leader_hint
  kStopped,     // consensus has halted (shutdown, or an apply failure)
};

struct ProposeResult {
  ProposeStatus status = ProposeStatus::kStopped;
  LogIndex index = 0;               // valid when kAccepted
  std::string message;              // detail for kInvalid / kStopped
  std::uint32_t leader_hint = 0;    // node id of the believed leader for kNotLeader, 0 = unknown
};

// Called by the KV layer on the node that received a client write.
class Proposer {
 public:
  virtual ~Proposer() = default;
  // Validates, encodes and appends the command to the log. Does NOT wait for commit.
  virtual ProposeResult Propose(const Command& command) = 0;
  // Blocks until the entry at `index` has been applied locally, or the timeout expires.
  virtual bool WaitApplied(LogIndex index, std::chrono::milliseconds timeout) = 0;
};

}  // namespace lsmdb::consensus
