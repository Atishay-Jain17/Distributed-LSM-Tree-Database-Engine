#include "lsmdb/api/reference_memtable.h"

#include <mutex>
#include <stdexcept>

namespace lsmdb::api {

void ReferenceMemTable::Add(SequenceNumber seq, ValueType type, const std::string& key,
                            const std::string& value) {
  std::unique_lock lock(mu_);
  auto k = std::make_pair(key, seq);
  if (entries_.count(k)) throw std::logic_error("duplicate (key, sequence) in MemTable");
  Entry e{type, type == ValueType::kValue ? value : std::string()};
  bytes_ += key.size() + e.value.size() + sizeof(SequenceNumber) + 1;
  entries_.emplace(std::move(k), std::move(e));
}

LookupResult ReferenceMemTable::Get(const std::string& key, SequenceNumber read_seq) const {
  std::shared_lock lock(mu_);
  auto it = entries_.lower_bound(std::make_pair(key, read_seq));
  LookupResult r;
  if (it == entries_.end() || it->first.first != key) return r;  // kNotFound
  r.sequence = it->first.second;
  if (it->second.type == ValueType::kTombstone) {
    r.state = LookupState::kDeleted;
  } else {
    r.state = LookupState::kFound;
    r.value = it->second.value;
  }
  return r;
}

std::size_t ReferenceMemTable::ApproximateMemoryUsage() const {
  std::shared_lock lock(mu_);
  return bytes_;
}

std::size_t ReferenceMemTable::EntryCount() const {
  std::shared_lock lock(mu_);
  return entries_.size();
}

}  // namespace lsmdb::api
