#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lsmdb/storage/compaction_options.h"
#include "lsmdb/storage/version.h"

namespace lsmdb::storage {

struct CompactionTask {
  int level = 0;
  int output_level = 1;
  std::vector<std::shared_ptr<FileMeta>> inputs;       // files from `level`
  std::vector<std::shared_ptr<FileMeta>> next_inputs;  // overlapping files from `output_level`
  std::shared_ptr<const Version> base;                 // the Version the task was picked from
};

// Decides WHAT to compact. Score per level = (files or bytes) / budget; the level with the
// highest score >= 1 is compacted. Level 0 compacts ALL its files at once (they overlap).
// Deeper levels compact one file, chosen round-robin by key so no range starves.
class CompactionPicker {
 public:
  explicit CompactionPicker(CompactionOptions options) : options_(options) {}

  std::optional<CompactionTask> Pick(std::shared_ptr<const Version> version);

  double Score(const Version& version, int level) const;
  std::uint64_t MaxBytesForLevel(int level) const;  // level >= 1

 private:
  CompactionOptions options_;
  std::vector<std::string> compact_pointer_ = std::vector<std::string>(kNumLevels);
};

}  // namespace lsmdb::storage
