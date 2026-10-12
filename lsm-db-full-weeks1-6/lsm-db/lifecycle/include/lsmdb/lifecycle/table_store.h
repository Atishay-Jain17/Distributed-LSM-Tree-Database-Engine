#pragma once
// The disk side of the LSM tree, as seen by the flush/compaction lifecycle (Week 4).
//
// Atishay's SSTable writer + sparse index, Sarthak's Bloom filters and Vinayak's compaction
// plug in behind this interface. LsmStore never touches files itself; it only decides WHEN to
// flush and compact. ReferenceTableStore (reference_table_store.h) is a simple correct version.
//
// Thread safety: every method may be called concurrently (reads from many threads, one flush
// and one compaction at a time, both while reads are running).
#include <string>

#include "lsmdb/api/memtable.h"

namespace lsmdb::lifecycle {

using api::LookupResult;
using api::MemTable;
using api::SequenceNumber;

class TableStore {
 public:
  virtual ~TableStore() = default;

  // FLUSH: write every entry of `imm` (via MemTable::ForEach, tombstones included) into a new
  // table and make it visible to Get() atomically. Must be all-or-nothing: if it throws, no
  // partial table may be visible. The new table is the NEWEST table.
  virtual void AddTable(const MemTable& imm) = 0;

  // Newest entry for `key` with sequence <= read_seq across all tables (newest table first).
  // Returns kDeleted when a tombstone decides the result, kNotFound if no table has the key.
  virtual LookupResult Get(const std::string& key, SequenceNumber read_seq) const = 0;

  // True when the table layout wants a compaction (e.g. too many tables / level over budget).
  virtual bool NeedsCompaction() const = 0;

  // One compaction cycle: merge tables into new ones, publish the new set atomically, then
  // retire the old files. Reads running at the same time must keep working: an old file may only
  // be deleted after the last reader using it is done.
  //
  // `oldest_snapshot` is the smallest sequence any current or future read may still use. The
  // compaction must keep, for every key, all versions NEWER than it plus the newest version at or
  // below it; only older versions may be dropped, and a tombstone may be dropped only when it is
  // at or below `oldest_snapshot` and nothing older than it remains below the output.
  virtual void Compact(SequenceNumber oldest_snapshot) = 0;
};

}  // namespace lsmdb::lifecycle
