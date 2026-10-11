#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmdb/storage/version.h"

namespace lsmdb::storage {

struct VersionEdit {
  std::vector<std::shared_ptr<FileMeta>> added;
  std::vector<std::shared_ptr<FileMeta>> removed;
};

// Owns the sequence of Versions. Readers call Current() and keep the returned
// shared_ptr for as long as they need a stable file set.
//
// File names encode the level: "L<level>-<number 6 digits>.sst", so Recover() can rebuild
// the level layout by scanning the directory (the SSTable footer supplies key ranges).
//
// KNOWN LIMITATION: there is no manifest/atomic multi-file install. If the process dies
// between publishing compaction outputs and deleting inputs, both may exist on restart.
// Data stays correct (duplicates have identical (key, seq)), but the L1+ "no overlap"
// rule can be violated until the next compaction. A manifest is the standard fix.
class VersionSet {
 public:
  explicit VersionSet(std::string dir);  // creates the directory if needed

  // Removes leftover "*.tmp" files and loads every "L*-*.sst" file. Throws on corruption.
  void Recover();

  std::shared_ptr<const Version> Current() const;
  std::uint64_t NewFileNumber() { return next_file_number_++; }
  std::string FilePath(int level, std::uint64_t number) const;

  // Registers an SSTable that was just written (typically a flush into level 0).
  std::shared_ptr<FileMeta> AddFile(int level, std::uint64_t number, SSTableMetadata meta);

  // Atomically publishes a new Version. Throws std::logic_error (and changes nothing)
  // if a removed file is not present or the level-ordering invariant would break.
  // Removed files are marked obsolete; they are deleted once no Version references them.
  void Apply(const VersionEdit& edit);

  const std::string& dir() const { return dir_; }

 private:
  std::string dir_;
  mutable std::mutex mu_;
  std::shared_ptr<const Version> current_;
  std::atomic<std::uint64_t> next_file_number_{1};
};

}  // namespace lsmdb::storage
