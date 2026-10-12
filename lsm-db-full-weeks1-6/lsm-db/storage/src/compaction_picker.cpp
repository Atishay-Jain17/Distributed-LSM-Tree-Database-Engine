#include "lsmdb/storage/compaction_picker.h"

namespace lsmdb::storage {

std::uint64_t CompactionPicker::MaxBytesForLevel(int level) const {
  std::uint64_t bytes = options_.level1_max_bytes;
  for (int l = 1; l < level; ++l) bytes *= options_.level_multiplier;
  return bytes;
}

double CompactionPicker::Score(const Version& v, int level) const {
  if (level == 0) {
    return static_cast<double>(v.NumFiles(0)) / static_cast<double>(options_.l0_file_trigger);
  }
  return static_cast<double>(v.LevelBytes(level)) / static_cast<double>(MaxBytesForLevel(level));
}

std::optional<CompactionTask> CompactionPicker::Pick(std::shared_ptr<const Version> v) {
  int best_level = -1;
  double best_score = 1.0;  // anything below 1.0 means "within budget"
  for (int level = 0; level < kNumLevels - 1; ++level) {  // the last level never compacts further
    double s = Score(*v, level);
    if (s >= best_score) {
      best_score = s;
      best_level = level;
    }
  }
  if (best_level < 0) return std::nullopt;

  CompactionTask task;
  task.level = best_level;
  task.output_level = best_level + 1;
  task.base = v;

  if (best_level == 0) {
    task.inputs = v->levels[0];
  } else {
    const auto& files = v->levels[best_level];
    const std::string& ptr = compact_pointer_[best_level];
    auto chosen = files.front();
    for (const auto& f : files) {  // first file after the round-robin pointer, else wrap to the first
      if (ptr.empty() || f->meta.min_key > ptr) {
        chosen = f;
        break;
      }
    }
    compact_pointer_[best_level] = chosen->meta.max_key;
    task.inputs = {chosen};
  }

  std::string lo = task.inputs.front()->meta.min_key, hi = task.inputs.front()->meta.max_key;
  for (const auto& f : task.inputs) {
    if (f->meta.min_key < lo) lo = f->meta.min_key;
    if (f->meta.max_key > hi) hi = f->meta.max_key;
  }
  task.next_inputs = v->Overlapping(task.output_level, lo, hi);
  return task;
}

}  // namespace lsmdb::storage
