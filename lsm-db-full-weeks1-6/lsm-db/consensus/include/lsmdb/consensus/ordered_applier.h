#pragma once
// ---------------------------------------------------------------------------
// OrderedApplier: the bridge from "committed in the Raft log" to "visible in storage".
//
// WHY IT EXISTS: replicas stay identical only if every node applies the same commands in
// the same order. The applier enforces that: entries are applied strictly by log index,
// each exactly once, even if the delivery layer reorders or repeats them.
//
//   index already applied   -> ignored (duplicate)
//   index above the next    -> buffered until the gap fills
//   next expected index     -> decoded and applied, then any buffered successors
//   undecodable payload     -> skipped (counted) so every replica skips the same entry
//   storage throws          -> applying STOPS (a replica must not silently diverge);
//                              apply_error() reports why and waiters are released
// ---------------------------------------------------------------------------
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::consensus {

class OrderedApplier {
 public:
  explicit OrderedApplier(std::shared_ptr<rpc::StorageBackend> backend, std::uint64_t first_index = 1);

  void OnCommitted(std::uint64_t index, const std::string& payload);

  std::uint64_t LastApplied() const;
  std::uint64_t skipped_corrupt() const;
  std::string apply_error() const;  // empty when healthy

  // Blocks until LastApplied() >= index. False on timeout or if applying has failed.
  bool WaitForApplied(std::uint64_t index, std::chrono::milliseconds timeout) const;

 private:
  void DrainLocked();

  std::shared_ptr<rpc::StorageBackend> backend_;
  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::uint64_t next_index_;
  std::uint64_t skipped_ = 0;
  std::string error_;
  std::map<std::uint64_t, std::string> pending_;
};

}  // namespace lsmdb::consensus
