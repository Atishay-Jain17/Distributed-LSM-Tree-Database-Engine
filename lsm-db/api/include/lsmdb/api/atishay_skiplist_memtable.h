#pragma once

#include <shared_mutex>

#include "lsmdb/api/memtable.h"
#include "storage/skiplist.h"

namespace lsmdb::api {

// Adapts Atishay's ordered SkipList to Suhani's MVCC MemTable contract. Composite keys encode
// the user key followed by the inverted sequence number, keeping versions ordered newest-first.
// A reader/writer lock supplies the thread-safety that the standalone SkipList does not provide.
class AtishaySkipListMemTable final : public MemTable {
 public:
  void Add(SequenceNumber seq, ValueType type, const std::string& key,
           const std::string& value) override;
  LookupResult Get(const std::string& key, SequenceNumber read_seq) const override;
  std::size_t ApproximateMemoryUsage() const override;
  std::size_t EntryCount() const override;
  void ForEach(const EntryVisitor& visit) const override;

 private:
  static std::string KeyPrefix(const std::string& key);
  static std::string VersionKey(const std::string& key, SequenceNumber seq);
  static std::string DecodeUserKey(const std::string& composite, std::size_t& suffix_offset);
  static SequenceNumber DecodeSequence(const std::string& composite, std::size_t suffix_offset);

  mutable std::shared_mutex mu_;
  lsm::SkipList entries_;
};

}  // namespace lsmdb::api
