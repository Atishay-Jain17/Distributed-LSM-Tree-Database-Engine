#include "lsmdb/api/memtable_backend.h"

#include <stdexcept>

namespace lsmdb::api {

MemTableBackend::MemTableBackend(std::shared_ptr<MemTable> memtable)
    : memtable_(std::move(memtable)) {
  if (!memtable_) throw std::invalid_argument("MemTableBackend needs a MemTable");
}

void MemTableBackend::Write(ValueType type, const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(write_mu_);
  const SequenceNumber seq = next_seq_;
  memtable_->Add(seq, type, key, value);   // throws -> sequence not consumed, nothing published
  ++next_seq_;
  last_visible_.store(seq, std::memory_order_release);
}

void MemTableBackend::Restore(SequenceNumber seq, ValueType type, const std::string& key,
                              const std::string& value) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (seq < next_seq_) throw std::logic_error("Restore: sequence numbers must increase");
  memtable_->Add(seq, type, key, value);
  next_seq_ = seq + 1;
  last_visible_.store(seq, std::memory_order_release);
}

void MemTableBackend::Put(const std::string& key, const std::string& value) {
  Write(ValueType::kValue, key, value);
}

void MemTableBackend::Delete(const std::string& key) {
  Write(ValueType::kTombstone, key, std::string());
}

LookupResult MemTableBackend::GetAt(const std::string& key, SequenceNumber read_seq) const {
  return memtable_->Get(key, read_seq);
}

std::optional<std::string> MemTableBackend::Get(const std::string& key) {
  LookupResult r = memtable_->Get(key, LastSequence());
  if (r.state == LookupState::kFound) return std::move(r.value);
  return std::nullopt;
}

}  // namespace lsmdb::api
