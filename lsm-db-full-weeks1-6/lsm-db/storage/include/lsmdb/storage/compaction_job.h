#pragma once
#include <cstdint>

#include "lsmdb/storage/compaction_options.h"
#include "lsmdb/storage/compaction_picker.h"
#include "lsmdb/storage/version_set.h"

namespace lsmdb::storage {

struct CompactionStats {
  std::size_t input_files = 0, output_files = 0;
  std::uint64_t entries_in = 0, entries_out = 0, entries_dropped = 0;
};

// Merges the task's input files into new files at task.output_level and publishes the
// result with one VersionEdit.
//
// VERSION-DROP RULES (MVCC safety), per user key, newest first:
//   * every version with seq >  oldest_snapshot_seq is kept (a live snapshot may need it)
//   * the newest version with seq <= oldest_snapshot_seq is kept (it is what the oldest
//     snapshot sees) ... unless it is a tombstone AND no deeper level may hold the key,
//     in which case the tombstone has nothing left to hide and is dropped
//   * every older version is dropped (shadowed for everyone)
// Pass kMaxSequence when there are no open snapshots.
//
// A key's versions are never split across two output files, so output files keep
// non-overlapping key ranges. Inputs are checksum-verified first; on ANY failure the
// partial outputs are deleted and the Version is left untouched (throws).
CompactionStats RunCompaction(const CompactionTask& task, VersionSet& versions,
                              const CompactionOptions& options, std::uint64_t oldest_snapshot_seq);

}  // namespace lsmdb::storage
