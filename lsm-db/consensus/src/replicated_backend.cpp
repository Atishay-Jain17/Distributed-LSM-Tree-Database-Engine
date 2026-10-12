#include "lsmdb/consensus/replicated_backend.h"

namespace lsmdb::consensus {

ReplicatedBackend::ReplicatedBackend(std::shared_ptr<Proposer> proposer,
                                     std::shared_ptr<rpc::StorageBackend> reader,
                                     std::chrono::milliseconds apply_timeout)
    : proposer_(std::move(proposer)), reader_(std::move(reader)), timeout_(apply_timeout) {
  if (!proposer_ || !reader_) throw std::invalid_argument("ReplicatedBackend needs a proposer and a reader");
}

void ReplicatedBackend::Replicate(const Command& command) {
  ProposeResult r = proposer_->Propose(command);
  switch (r.status) {
    case ProposeStatus::kAccepted:
      break;
    case ProposeStatus::kInvalid:
      throw std::invalid_argument(r.message);
    case ProposeStatus::kNotLeader:
      throw NotLeaderError(r.leader_hint);
    case ProposeStatus::kStopped:
      throw std::runtime_error("consensus stopped: " + r.message);
  }
  if (!proposer_->WaitApplied(r.index, timeout_))
    throw std::runtime_error("write was proposed but not applied within the timeout");
}

void ReplicatedBackend::Put(const std::string& key, const std::string& value) {
  Replicate(Command::Put(key, value));
}

void ReplicatedBackend::Delete(const std::string& key) { Replicate(Command::Delete(key)); }

std::optional<std::string> ReplicatedBackend::Get(const std::string& key) { return reader_->Get(key); }

}  // namespace lsmdb::consensus
