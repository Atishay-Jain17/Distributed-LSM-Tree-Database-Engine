#pragma once
// Single-node LSM store: WAL-recovered MemTable -> flush -> tables -> compaction (Week 4).
//
// Implements rpc::StorageBackend, so it replaces MemTableBackend in node_main.cpp:
//     auto tables = std::make_shared<YourTableStore>(dir);   // Atishay / Sarthak / Vinayak
//     auto db     = std::make_shared<lsmdb::lifecycle::LsmStore>(tables, options);
//
// Lifecycle (see docs/single-node-flow.md):
//   * writes go to the ACTIVE MemTable;
//   * when it reaches `memtable_bytes` it is ROTATED: it becomes the single IMMUTABLE MemTable
//     and a fresh active one takes over (rotation is instant, writers do not wait for disk);
//   * the FLUSH worker writes the immutable MemTable through TableStore::AddTable, then drops it;
//   * after each flush the COMPACTION worker is woken if TableStore::NeedsCompaction();
//   * if the active MemTable fills up again while the previous flush is still running, the
//     writer STALLS (backpressure) until that flush finishes. Memory stays bounded.
// Reads look in: active -> immutable -> tables, all at one consistent snapshot sequence.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "lsmdb/api/memtable.h"
#include "lsmdb/api/restore_target.h"
#include "lsmdb/lifecycle/table_store.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::lifecycle {

struct LsmOptions {
  // Rotate the active MemTable once ApproximateMemoryUsage() reaches this many bytes.
  std::size_t memtable_bytes = 4u << 20;
  // Highest sequence number already stored in tables (from the table manifest). New writes
  // continue after it. 0 for a fresh database.
  SequenceNumber start_sequence = 0;
  // Creates MemTables. Default: api::ReferenceMemTable. Plug the SkipList in here.
  std::function<std::shared_ptr<MemTable>()> memtable_factory;
  // Called from the flush thread (before Flush()/FlushedUpTo() report it) once a MemTable is in a table, with the highest
  // sequence it contained. Everything <= that sequence no longer needs the WAL (recovery's
  // `flushed_up_to`), so the WAL owner can truncate/delete segments up to it.
  std::function<void(SequenceNumber)> on_flushed;
};

struct LsmStats {
  std::uint64_t rotations = 0;
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;
  std::uint64_t write_stalls = 0;   // times a writer had to wait for the previous flush
};

class LsmStore : public rpc::StorageBackend, public api::RestoreTarget {
 public:
  LsmStore(std::shared_ptr<TableStore> tables, LsmOptions options = {});
  // Stops the workers. An immutable MemTable that was not flushed yet is NOT written: its data
  // is still in the WAL and will be replayed by recovery.
  ~LsmStore() override;
  LsmStore(const LsmStore&) = delete;
  LsmStore& operator=(const LsmStore&) = delete;

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;  // idempotent, always writes a tombstone

  // Recovery only (Week 3 API): re-apply a WAL record with its original sequence number, from a
  // single thread, before serving requests. Sequence numbers must increase.
  void Restore(SequenceNumber seq, api::ValueType type, const std::string& key,
               const std::string& value) override;

  // ---- snapshots ----
  // A pinned read sequence. While a Snapshot is alive, compaction keeps the versions it needs,
  // so GetAt(key, snap.sequence()) always returns the value as of the moment it was taken.
  class Snapshot {
   public:
    Snapshot() = default;
    Snapshot(Snapshot&& o) noexcept : store_(o.store_), seq_(o.seq_) { o.store_ = nullptr; }
    Snapshot& operator=(Snapshot&& o) noexcept;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    ~Snapshot() { Release(); }
    SequenceNumber sequence() const { return seq_; }
    void Release();

   private:
    friend class LsmStore;
    Snapshot(LsmStore* s, SequenceNumber q) : store_(s), seq_(q) {}
    LsmStore* store_ = nullptr;
    SequenceNumber seq_ = 0;
  };
  Snapshot AcquireSnapshot();
  api::LookupResult GetAt(const std::string& key, SequenceNumber read_seq);

  // ---- control / observability ----
  SequenceNumber LastSequence() const override { return last_visible_.load(std::memory_order_acquire); }
  // Highest sequence known to be in tables (WAL can be truncated up to here).
  SequenceNumber FlushedUpTo() const;
  // Rotate the active MemTable (if non-empty) and block until everything written so far is in
  // tables. Throws std::runtime_error if the flush worker failed.
  void Flush();
  // Block until no flush or compaction is pending or running (or a background error happened).
  void WaitForIdle();
  LsmStats stats() const;
  // Non-empty after a flush failed. Writes that need a rotation then throw this error.
  std::string background_error() const;

 private:
  struct Pinned {
    std::shared_ptr<MemTable> active, imm;
    SequenceNumber seq = 0;
  };
  void Write(api::ValueType type, const std::string& key, const std::string& value);
  void RotateLocked(std::unique_lock<std::mutex>& bg);   // needs write_mu_ and bg_mu_
  Pinned PinState(bool register_snapshot);
  api::LookupResult Lookup(const std::string& key, const Pinned& p) const;
  void ReleaseSnapshot(SequenceNumber seq);
  void FlushLoop();
  void CompactionLoop();

  std::shared_ptr<TableStore> tables_;
  LsmOptions opt_;

  std::mutex write_mu_;                       // serialises writes and rotation
  api::SequenceNumber next_seq_;              // guarded by write_mu_
  std::atomic<SequenceNumber> last_visible_{0};

  mutable std::mutex bg_mu_;                  // guards everything below
  std::condition_variable bg_cv_;
  std::shared_ptr<MemTable> active_;          // replaced only with write_mu_ + bg_mu_ held
  std::shared_ptr<MemTable> imm_;             // null when no flush is pending
  SequenceNumber imm_last_seq_ = 0;
  SequenceNumber flushed_up_to_ = 0;
  bool compaction_requested_ = false;
  bool compacting_ = false;
  bool stopping_ = false;
  std::string bg_error_;
  std::string compaction_error_;
  std::multiset<SequenceNumber> snapshots_;
  LsmStats stats_;

  std::thread flush_thread_, compaction_thread_;
};

}  // namespace lsmdb::lifecycle
