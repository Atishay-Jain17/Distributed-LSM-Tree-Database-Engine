#pragma once
// Simple, correct TableStore that keeps real files on disk (Week 4 stand-in for the SSTable
// layer). It exists so the flush/compaction lifecycle and its tests do not wait for the real
// SSTable, index, Bloom filter and compaction code. NOT intended to be fast: a lookup scans a
// file. It shows the rules a real implementation must follow:
//   * a new file is written under a temporary name and renamed into place, then published;
//   * the table list is replaced atomically (copy-on-write); readers keep the list they started on;
//   * a replaced file is deleted only when the last reader releasing it is gone.
// It does not reload existing files at start-up (the real manifest does that).
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "lsmdb/lifecycle/table_store.h"

namespace lsmdb::lifecycle {

struct ReferenceTableStoreOptions {
  std::size_t compaction_trigger = 4;   // NeedsCompaction() when this many tables exist
};

class ReferenceTableStore : public TableStore {
 public:
  explicit ReferenceTableStore(std::filesystem::path dir,
                               ReferenceTableStoreOptions options = {});
  ~ReferenceTableStore() override;

  void AddTable(const MemTable& imm) override;
  LookupResult Get(const std::string& key, SequenceNumber read_seq) const override;
  bool NeedsCompaction() const override;
  void Compact(SequenceNumber oldest_snapshot) override;

  // ---- inspection (tests, metrics) ----
  std::size_t TableCount() const;
  std::vector<std::filesystem::path> LivePaths() const;   // newest first
  // Keeps the current tables (and their files) alive until the returned handle is destroyed,
  // like a long-running read would.
  std::shared_ptr<const void> Pin() const;

 private:
  class Table;
  using TableList = std::vector<std::shared_ptr<Table>>;   // newest first
  std::shared_ptr<const TableList> Current() const;
  std::shared_ptr<Table> WriteFile(const std::vector<std::tuple<std::string, SequenceNumber,
                                   api::ValueType, std::string>>& entries);

  std::filesystem::path dir_;
  ReferenceTableStoreOptions opt_;
  std::atomic<std::uint64_t> next_file_{1};
  mutable std::mutex mu_;                       // guards tables_ (pointer swap only)
  std::shared_ptr<const TableList> tables_;
  std::mutex compact_mu_;                       // one compaction at a time
};

}  // namespace lsmdb::lifecycle
