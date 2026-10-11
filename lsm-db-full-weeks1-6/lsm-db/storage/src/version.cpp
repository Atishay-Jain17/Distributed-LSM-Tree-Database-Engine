#include "lsmdb/storage/version.h"

#include <algorithm>

namespace lsmdb::storage {
namespace {
// L1+ file whose range contains `key`, or nullptr (files are sorted and disjoint).
const FileMeta* FindInSortedLevel(const std::vector<std::shared_ptr<FileMeta>>& files,
                                  const std::string& key) {
  auto it = std::lower_bound(files.begin(), files.end(), key,
                             [](const std::shared_ptr<FileMeta>& f, const std::string& k) {
                               return f->meta.max_key < k;
                             });
  if (it == files.end() || (*it)->meta.min_key > key) return nullptr;
  return it->get();
}
}  // namespace

std::uint64_t Version::LevelBytes(int level) const {
  std::uint64_t total = 0;
  for (const auto& f : levels[level]) total += f->meta.file_size;
  return total;
}

std::vector<std::shared_ptr<FileMeta>> Version::Overlapping(int level, const std::string& min_key,
                                                            const std::string& max_key) const {
  std::vector<std::shared_ptr<FileMeta>> out;
  for (const auto& f : levels[level]) {
    if (!(f->meta.max_key < min_key || f->meta.min_key > max_key)) out.push_back(f);
  }
  return out;
}

std::optional<Entry> VersionGet(const Version& v, const std::string& key, std::uint64_t snapshot_seq) {
  std::optional<Entry> best;
  auto consider = [&](const FileMeta& f) {
    if (key < f.meta.min_key || key > f.meta.max_key) return;
    auto e = f.Reader()->Get(key, snapshot_seq);
    if (e && (!best || e->seq > best->seq)) best = std::move(e);
  };
  for (const auto& f : v.levels[0]) consider(*f);
  for (int level = 1; level < kNumLevels; ++level) {
    if (const FileMeta* f = FindInSortedLevel(v.levels[level], key)) consider(*f);
  }
  return best;
}

bool KeyMayExistBelow(const Version& v, const std::string& key, int level) {
  for (int l = level + 1; l < kNumLevels; ++l) {
    if (FindInSortedLevel(v.levels[l], key)) return true;
  }
  return false;
}

}  // namespace lsmdb::storage
