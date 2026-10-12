#include "lsmdb/consensus/storage_state_machine.h"

#include <stdexcept>
#include <string>

namespace lsmdb::consensus {

StorageStateMachine::StorageStateMachine(std::shared_ptr<rpc::StorageBackend> storage,
                                         LogIndex last_applied)
    : storage_(std::move(storage)), last_applied_(last_applied) {
  if (!storage_) throw std::invalid_argument("StorageStateMachine needs a storage backend");
}

ApplyOutcome StorageStateMachine::Apply(LogIndex index, const std::string& payload) {
  const LogIndex last = last_applied_.load(std::memory_order_acquire);
  if (index <= last) return ApplyOutcome::kSkippedDuplicate;
  if (index != last + 1) {
    throw std::logic_error("apply gap: expected index " + std::to_string(last + 1) + ", got " +
                           std::to_string(index));
  }

  ApplyOutcome outcome;
  DecodeResult d = Decode(payload);
  if (!d) {
    outcome = ApplyOutcome::kRejectedMalformed;
  } else if (!Validate(*d.command)) {
    outcome = ApplyOutcome::kRejectedInvalid;
  } else {
    // If storage throws, last_applied_ is not advanced: the entry has not been applied.
    if (d.command->type == CommandType::kPut) storage_->Put(d.command->key, d.command->value);
    else storage_->Delete(d.command->key);
    outcome = ApplyOutcome::kApplied;
  }
  if (outcome != ApplyOutcome::kApplied) ++rejected_;
  last_applied_.store(index, std::memory_order_release);
  return outcome;
}

}  // namespace lsmdb::consensus
