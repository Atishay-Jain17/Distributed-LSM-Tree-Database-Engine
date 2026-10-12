#pragma once
// The leader-side (and follower-redirect) write path (Week 6):
//
//   client Put -> KvServiceImpl -> ReplicatedWriteHandler
//     -> RaftProposer::Propose        (leader only; followers answer NOT_LEADER + hint)
//     -> quorum commit
//     -> OrderedApplier applies it    (storage updated, in log order)
//     -> OK returned to the client
//
// Returning OK only after the entry is applied locally gives read-your-writes on the node
// the client talked to. Retrying after UNAVAILABLE is safe: Put/Delete are idempotent.
#include <chrono>
#include <memory>

#include "lsmdb/consensus/ordered_applier.h"
#include "lsmdb/consensus/raft_proposer.h"
#include "lsmdb/rpc/write_handler.h"
#include "lsmdb/v1/command.pb.h"

namespace lsmdb::consensus {

struct ReplicatedWriteOptions {
  std::chrono::milliseconds propose_timeout{2000};  // waiting for quorum commit
  std::chrono::milliseconds apply_timeout{2000};    // waiting for local apply after commit
};

class ReplicatedWriteHandler final : public rpc::WriteHandler {
 public:
  // Registers the applier as the proposer's commit callback.
  ReplicatedWriteHandler(std::shared_ptr<RaftProposer> proposer, std::shared_ptr<OrderedApplier> applier,
                         ReplicatedWriteOptions options = {});

  v1::Status Put(const std::string& key, const std::string& value) override;
  v1::Status Delete(const std::string& key) override;

 private:
  v1::Status Submit(const v1::ReplicatedCommand& command);

  std::shared_ptr<RaftProposer> proposer_;
  std::shared_ptr<OrderedApplier> applier_;
  ReplicatedWriteOptions options_;
};

}  // namespace lsmdb::consensus
