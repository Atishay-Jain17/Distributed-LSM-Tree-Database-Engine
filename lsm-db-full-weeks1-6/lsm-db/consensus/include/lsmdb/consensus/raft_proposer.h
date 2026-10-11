#pragma once
// ---------------------------------------------------------------------------
// INTEGRATION CONTRACT with the Raft implementation (Atishay: log + state,
// Sarthak: heartbeats + quorum). My write path depends ONLY on this interface.
//
//   Propose(payload, timeout)
//     * Leader: append `payload` to the replicated log, replicate, and BLOCK until a
//       quorum has acknowledged it (committed) or `timeout` passes.
//         success            -> status OK, log_index = the entry's index (1-based)
//         no quorum / timeout-> STATUS_CODE_UNAVAILABLE
//     * Follower: do not append. Return STATUS_CODE_NOT_LEADER, with status.leader_hint
//       set to the leader's NodeInfo when it is known.
//
//   SetCommitCallback(cb)
//     Raft calls cb(index, payload) for every COMMITTED entry on this node (leader and
//     followers). Entries should arrive in increasing index order; my OrderedApplier also
//     tolerates reordering and duplicates, so a retried delivery after a restart is safe.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::consensus {

struct ProposalResult {
  v1::Status status;
  std::uint64_t log_index = 0;
};

using CommitCallback = std::function<void(std::uint64_t index, const std::string& payload)>;

class RaftProposer {
 public:
  virtual ~RaftProposer() = default;
  virtual ProposalResult Propose(const std::string& payload, std::chrono::milliseconds timeout) = 0;
  virtual void SetCommitCallback(CommitCallback callback) = 0;
};

}  // namespace lsmdb::consensus
