#include "lsmdb/storage/version_set.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace lsmdb::storage {
namespace fs = std::filesystem;

namespace {
void SortLevels(Version& v) {
  std::sort(v.levels[0].begin(), v.levels[0].end(),
            [](const auto& a, const auto& b) { return a->number < b->number; });
  for (int l = 1; l < kNumLevels; ++l) {
    std::sort(v.levels[l].begin(), v.levels[l].end(),
              [](const auto& a, const auto& b) { return a->meta.min_key < b->meta.min_key; });
  }
}

void CheckInvariants(const Version& v) {
  for (int l = 1; l < kNumLevels; ++l) {
    const auto& files = v.levels[l];
    for (std::size_t i = 1; i < files.size(); ++i) {
      if (!(files[i - 1]->meta.max_key < files[i]->meta.min_key)) {
        throw std::logic_error("level " + std::to_string(l) + " files overlap");
      }
    }
  }
}
}  // namespace

VersionSet::VersionSet(std::string dir) : dir_(std::move(dir)), current_(std::make_shared<Version>()) {
  fs::create_directories(dir_);
}

std::shared_ptr<const Version> VersionSet::Current() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_;
}

std::string VersionSet::FilePath(int level, std::uint64_t number) const {
  char name[64];
  std::snprintf(name, sizeof name, "L%d-%06llu.sst", level, static_cast<unsigned long long>(number));
  return dir_ + "/" + name;
}

void VersionSet::Recover() {
  auto next = std::make_shared<Version>();
  std::uint64_t max_number = 0;
  for (const auto& entry : fs::directory_iterator(dir_)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") {
      fs::remove(entry.path());  // unfinished write from a crash
      continue;
    }
    int level = 0;
    unsigned long long number = 0;
    if (std::sscanf(name.c_str(), "L%d-%llu.sst", &level, &number) != 2 || level < 0 ||
        level >= kNumLevels) {
      continue;
    }
    auto reader = SSTableReader::Open(entry.path().string());  // throws on corruption
    next->levels[level].push_back(std::make_shared<FileMeta>(number, level, reader->metadata()));
    max_number = std::max<std::uint64_t>(max_number, number);
  }
  SortLevels(*next);
  CheckInvariants(*next);
  std::lock_guard<std::mutex> lock(mu_);
  current_ = next;
  next_file_number_ = max_number + 1;
}

std::shared_ptr<FileMeta> VersionSet::AddFile(int level, std::uint64_t number, SSTableMetadata meta) {
  auto file = std::make_shared<FileMeta>(number, level, std::move(meta));
  VersionEdit edit;
  edit.added.push_back(file);
  Apply(edit);
  return file;
}

void VersionSet::Apply(const VersionEdit& edit) {
  std::lock_guard<std::mutex> lock(mu_);
  auto next = std::make_shared<Version>(*current_);
  for (const auto& f : edit.removed) {
    auto& files = next->levels[f->level];
    auto it = std::find(files.begin(), files.end(), f);
    if (it == files.end()) throw std::logic_error("VersionEdit removes a file that is not present");
    files.erase(it);
  }
  for (const auto& f : edit.added) next->levels[f->level].push_back(f);
  SortLevels(*next);
  CheckInvariants(*next);
  current_ = next;
  for (const auto& f : edit.removed) f->MarkObsolete();
}

}  // namespace lsmdb::storage
