#pragma once
// An immutable snapshot of which SSTables exist at which level.
//
// Level rules (leveled compaction):
//   L0      : files come straight from MemTable flushes; key ranges MAY overlap.
//             Kept in ascending file-number order (oldest first).
//   L1..L6  : files are sorted by key and their ranges NEVER overlap.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lsmdb/storage/entry.h"
#include "lsmdb/storage/file_meta.h"

namespace lsmdb::storage {

inline constexpr int kNumLevels = 7;

struct Version {
  Version() : levels(kNumLevels) {}
  std::vector<std::vector<std::shared_ptr<FileMeta>>> levels;

  std::size_t NumFiles(int level) const { return levels[level].size(); }
  std::uint64_t LevelBytes(int level) const;
  // Files at `level` whose key range intersects [min_key, max_key].
  std::vector<std::shared_ptr<FileMeta>> Overlapping(int level, const std::string& min_key,
                                                     const std::string& max_key) const;
};

// Newest version of `key` with seq <= snapshot_seq across ALL files (tombstones included).
// Correct for any compaction state because it compares sequence numbers instead of
// assuming which level is newer. Returns nullopt if no file has such a version.
std::optional<Entry> VersionGet(const Version& version, const std::string& key,
                                std::uint64_t snapshot_seq = kMaxSequence);

// True if any file in a level deeper than `level` has a key range containing `key`.
// Conservative (range based): "true" may be a false alarm, "false" is always safe.
bool KeyMayExistBelow(const Version& version, const std::string& key, int level);

}  // namespace lsmdb::storage
