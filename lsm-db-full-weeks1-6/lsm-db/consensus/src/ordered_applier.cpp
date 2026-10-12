#include "lsmdb/consensus/ordered_applier.h"

#include <exception>

#include "lsmdb/consensus/command.h"

namespace lsmdb::consensus {

OrderedApplier::OrderedApplier(std::shared_ptr<rpc::StorageBackend> backend, std::uint64_t first_index)
    : backend_(std::move(backend)), next_index_(first_index) {}

void OrderedApplier::OnCommitted(std::uint64_t index, const std::string& payload) {
  std::lock_guard<std::mutex> lock(mu_);
  if (index < next_index_) return;  // already applied: duplicate delivery
  pending_.emplace(index, payload);
  DrainLocked();
  cv_.notify_all();
}

void OrderedApplier::DrainLocked() {
  while (error_.empty()) {
    auto it = pending_.find(next_index_);
    if (it == pending_.end()) return;
    auto command = DecodeCommand(it->second);
    if (!command) {
      ++skipped_;  // same bytes -> every replica skips the same entry
    } else {
      try {
        if (command->type() == v1::COMMAND_TYPE_PUT) backend_->Put(command->key(), command->value());
        else backend_->Delete(command->key());
      } catch (const std::exception& e) {
        error_ = "apply of log index " + std::to_string(next_index_) + " failed: " + e.what();
        return;  // do NOT advance: this replica must not move past an entry it could not apply
      }
    }
    pending_.erase(it);
    ++next_index_;
  }
}

std::uint64_t OrderedApplier::LastApplied() const {
  std::lock_guard<std::mutex> lock(mu_);
  return next_index_ - 1;
}

std::uint64_t OrderedApplier::skipped_corrupt() const {
  std::lock_guard<std::mutex> lock(mu_);
  return skipped_;
}

std::string OrderedApplier::apply_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return error_;
}

bool OrderedApplier::WaitForApplied(std::uint64_t index, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait_for(lock, timeout, [&] { return next_index_ - 1 >= index || !error_.empty(); });
  return error_.empty() && next_index_ - 1 >= index;
}

}  // namespace lsmdb::consensus
