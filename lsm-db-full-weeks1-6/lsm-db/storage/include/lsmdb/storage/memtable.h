#pragma once
// ---------------------------------------------------------------------------
// ConcurrentMemTable (Week 2, locking layer).
//
// LOCK DESIGN
//   One std::shared_mutex protects the ordered map:
//     * Get / Entries / counters take a SHARED lock  -> many readers in parallel
//     * Put / Delete / Freeze take an EXCLUSIVE lock -> one writer, no readers
//   Rationale: an LSM MemTable is read-heavy relative to its lock hold time
//   (lookups are short), writers are serialized anyway by the WAL, and a single
//   lock is simple to reason about and explain.
//
// DATA STRUCTURE
//   The ordered container below is a std::map ordered by (key asc, seq desc).
//   It is a stand-in for Atishay's SkipList: when that lands, replace the
//   `table_` member and the three places that touch it (Insert/Find/iterate).
//   The locking, freezing and versioning rules in this class stay unchanged.
//
// VERSIONS
//   Every write carries a sequence number supplied by the caller (WAL / Raft
//   index). Get(key, snapshot_seq) returns the newest version with seq <= snapshot.
//   Writing the same (key, seq) twice overwrites, which makes replays idempotent.
//
// FREEZING
//   Freeze() makes the table immutable (writes return false). The flush path
//   freezes it, then writes Entries() to an SSTable while readers keep using it.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "lsmdb/storage/entry.h"

namespace lsmdb::storage {

class MemTable {
 public:
  // kExclusiveOnly makes readers take the exclusive lock too. It exists ONLY so the
  // contention benchmark can show what the shared lock buys. Never use it in the engine.
  enum class LockMode { kSharedReaders, kExclusiveOnly };

  enum class LookupState { kFound, kDeleted, kNotFound };
  struct LookupResult {
    LookupState state = LookupState::kNotFound;
    std::string value;        // valid when kFound
    std::uint64_t seq = 0;    // version that produced the result (kFound or kDeleted)
  };

  explicit MemTable(LockMode mode = LockMode::kSharedReaders) : mode_(mode) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Return false (and change nothing) if the table is frozen.
  [[nodiscard]] bool Put(std::string key, std::string value, std::uint64_t seq);
  [[nodiscard]] bool Delete(std::string key, std::uint64_t seq);

  LookupResult Get(const std::string& key, std::uint64_t snapshot_seq = kMaxSequence) const;

  void Freeze();
  bool frozen() const;

  std::size_t ApproximateBytes() const;  // used to decide when to flush
  std::size_t EntryCount() const;

  // Consistent sorted copy (key asc, seq desc) for flushing to an SSTable.
  std::vector<Entry> Entries() const;

 private:
  using InternalKey = std::pair<std::string, std::uint64_t>;
  struct InternalKeyLess {
    bool operator()(const InternalKey& a, const InternalKey& b) const {
      if (int c = a.first.compare(b.first); c != 0) return c < 0;
      return a.second > b.second;  // newest first
    }
  };
  struct Slot {
    EntryType type;
    std::string value;
  };

  bool Insert(std::string key, EntryType type, std::string value, std::uint64_t seq);
  // Runs f() under the reader lock for the configured mode.
  template <class F>
  auto WithReadLock(F&& f) const {
    if (mode_ == LockMode::kExclusiveOnly) {
      std::unique_lock<std::shared_mutex> lock(mu_);
      return f();
    }
    std::shared_lock<std::shared_mutex> lock(mu_);
    return f();
  }

  const LockMode mode_;
  mutable std::shared_mutex mu_;
  std::map<InternalKey, Slot, InternalKeyLess> table_;
  std::size_t bytes_ = 0;
  bool frozen_ = false;
};

}  // namespace lsmdb::storage
