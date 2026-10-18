#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "lsmdb/lifecycle/table_store.h"
#include "lsmdb/storage/sstable_reader.h"

namespace lsmdb::node {

// Production TableStore adapter over Atishay's checksummed, indexed SSTables.
// Files are immutable; readers pin a copy-on-write table list while flush and
// compaction publish replacements through the manifest.
class AtishaySSTableTableStore final : public lifecycle::TableStore {
 public:
  explicit AtishaySSTableTableStore(std::filesystem::path dir,
                                    std::size_t compaction_trigger = 4);
  ~AtishaySSTableTableStore() override;

  void AddTable(const lifecycle::MemTable& memtable) override;
  lifecycle::LookupResult Get(const std::string& key,
                              lifecycle::SequenceNumber read_seq) const override;
  bool NeedsCompaction() const override;
  void Compact(lifecycle::SequenceNumber oldest_snapshot) override;

  lifecycle::SequenceNumber LastSequence() const { return last_sequence_.load(); }
  std::size_t TableCount() const;

 private:
  class Table;
  using TableList = std::vector<std::shared_ptr<Table>>;  // newest first

  std::shared_ptr<const TableList> Current() const;
  std::shared_ptr<Table> WriteTable(std::vector<storage::Entry> entries);
  void PersistManifest(const TableList& tables) const;
  void PersistSequence(lifecycle::SequenceNumber sequence) const;

  std::filesystem::path dir_;
  const std::size_t compaction_trigger_;
  std::atomic<std::uint64_t> next_file_{1};
  std::atomic<lifecycle::SequenceNumber> last_sequence_{0};
  mutable std::mutex mu_;
  std::shared_ptr<const TableList> tables_;
  std::mutex compact_mu_;
};

}  // namespace lsmdb::node
