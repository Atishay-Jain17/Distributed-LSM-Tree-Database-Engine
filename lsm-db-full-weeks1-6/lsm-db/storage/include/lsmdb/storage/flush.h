#pragma once
#include <optional>
#include <string>

#include "lsmdb/storage/memtable.h"
#include "lsmdb/storage/sstable_metadata.h"

namespace lsmdb::storage {

// Writes a FROZEN MemTable to a new SSTable at `path`.
// Returns std::nullopt if the table is empty (no file is created).
// Throws std::logic_error if the table is not frozen (flushing a table that can still
// change would silently lose later writes).
std::optional<SSTableMetadata> FlushMemTableToSSTable(const MemTable& table, const std::string& path);

}  // namespace lsmdb::storage
