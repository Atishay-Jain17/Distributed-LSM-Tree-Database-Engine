#include "lsmdb/consensus/replicated_write_handler.h"

#include "lsmdb/consensus/command.h"
#include "lsmdb/rpc/status.h"

namespace lsmdb::consensus {

ReplicatedWriteHandler::ReplicatedWriteHandler(std::shared_ptr<RaftProposer> proposer,
                                               std::shared_ptr<OrderedApplier> applier,
                                               ReplicatedWriteOptions options)
    : proposer_(std::move(proposer)), applier_(std::move(applier)), options_(options) {
  proposer_->SetCommitCallback(
      [applier = applier_](std::uint64_t index, const std::string& payload) { applier->OnCommitted(index, payload); });
}

v1::Status ReplicatedWriteHandler::Put(const std::string& key, const std::string& value) {
  return Submit(MakePutCommand(key, value));
}

v1::Status ReplicatedWriteHandler::Delete(const std::string& key) {
  return Submit(MakeDeleteCommand(key));
}

v1::Status ReplicatedWriteHandler::Submit(const v1::ReplicatedCommand& command) {
  ProposalResult proposal = proposer_->Propose(EncodeCommand(command), options_.propose_timeout);
  if (!rpc::IsOk(proposal.status)) return proposal.status;  // NOT_LEADER (+hint) or UNAVAILABLE passes through

  if (!applier_->WaitForApplied(proposal.log_index, options_.apply_timeout)) {
    std::string err = applier_->apply_error();
    if (!err.empty()) return rpc::MakeStatus(v1::STATUS_CODE_INTERNAL, err);
    return rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE,
                           "committed at log index " + std::to_string(proposal.log_index) +
                               " but not applied locally in time; retrying is safe");
  }
  return rpc::MakeStatus(v1::STATUS_CODE_OK);
}

}  // namespace lsmdb::consensus
