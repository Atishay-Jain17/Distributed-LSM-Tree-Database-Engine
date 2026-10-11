#include "lsmdb/storage/memtable.h"

namespace lsmdb::storage {

bool MemTable::Insert(std::string key, EntryType type, std::string value, std::uint64_t seq) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  if (frozen_) return false;
  const std::size_t add = key.size() + value.size() + sizeof(std::uint64_t) + 1;
  auto [it, inserted] = table_.try_emplace({std::move(key), seq}, Slot{type, std::string()});
  if (!inserted) bytes_ -= it->first.first.size() + it->second.value.size() + sizeof(std::uint64_t) + 1;
  it->second.type = type;
  it->second.value = std::move(value);
  bytes_ += add;
  return true;
}

bool MemTable::Put(std::string key, std::string value, std::uint64_t seq) {
  return Insert(std::move(key), EntryType::kPut, std::move(value), seq);
}

bool MemTable::Delete(std::string key, std::uint64_t seq) {
  return Insert(std::move(key), EntryType::kDelete, std::string(), seq);
}

MemTable::LookupResult MemTable::Get(const std::string& key, std::uint64_t snapshot_seq) const {
  return WithReadLock([&] {
    LookupResult result;
    // Order is (key asc, seq desc), so the first element >= (key, snapshot_seq)
    // is the newest version of `key` whose seq <= snapshot_seq (if any).
    auto it = table_.lower_bound(InternalKey{key, snapshot_seq});
    if (it == table_.end() || it->first.first != key) return result;
    result.seq = it->first.second;
    if (it->second.type == EntryType::kDelete) {
      result.state = LookupState::kDeleted;
    } else {
      result.state = LookupState::kFound;
      result.value = it->second.value;
    }
    return result;
  });
}

void MemTable::Freeze() {
  std::unique_lock<std::shared_mutex> lock(mu_);
  frozen_ = true;
}

bool MemTable::frozen() const {
  return WithReadLock([&] { return frozen_; });
}

std::size_t MemTable::ApproximateBytes() const {
  return WithReadLock([&] { return bytes_; });
}

std::size_t MemTable::EntryCount() const {
  return WithReadLock([&] { return table_.size(); });
}

std::vector<Entry> MemTable::Entries() const {
  return WithReadLock([&] {
    std::vector<Entry> out;
    out.reserve(table_.size());
    for (const auto& [k, slot] : table_) {
      out.push_back(Entry{k.first, k.second, slot.type, slot.value});
    }
    return out;
  });
}

}  // namespace lsmdb::storage
