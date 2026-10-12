#include "lsmdb/storage/flush.h"

#include <stdexcept>

#include "lsmdb/storage/sstable_writer.h"

namespace lsmdb::storage {

std::optional<SSTableMetadata> FlushMemTableToSSTable(const MemTable& table, const std::string& path) {
  if (!table.frozen()) throw std::logic_error("FlushMemTableToSSTable requires a frozen MemTable");
  std::vector<Entry> entries = table.Entries();
  if (entries.empty()) return std::nullopt;
  SSTableWriter writer(path);
  for (const Entry& e : entries) writer.Add(e);
  return writer.Finish();
}

}  // namespace lsmdb::storage
