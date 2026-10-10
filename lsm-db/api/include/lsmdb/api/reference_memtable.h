#pragma once
// Simple correct MemTable (std::map + reader/writer lock). It exists so the API
// layer and its tests do not wait for the SkipList; swap the SkipList in later.
// NOT intended to be fast.
#include <map>
#include <shared_mutex>
#include <string>
#include <utility>

#include "lsmdb/api/memtable.h"

namespace lsmdb::api {

class ReferenceMemTable : public MemTable {
 public:
  void Add(SequenceNumber seq, ValueType type, const std::string& key,
           const std::string& value) override;
  LookupResult Get(const std::string& key, SequenceNumber read_seq) const override;
  std::size_t ApproximateMemoryUsage() const override;
  std::size_t EntryCount() const override;

 private:
  // Ordered by key ascending, then sequence DESCENDING, so the first entry at or
  // after (key, read_seq) is the newest version visible to that read.
  struct Less {
    bool operator()(const std::pair<std::string, SequenceNumber>& a,
                    const std::pair<std::string, SequenceNumber>& b) const {
      if (a.first != b.first) return a.first < b.first;
      return a.second > b.second;
    }
  };
  struct Entry {
    ValueType type;
    std::string value;
  };

  mutable std::shared_mutex mu_;
  std::map<std::pair<std::string, SequenceNumber>, Entry, Less> entries_;
  std::size_t bytes_ = 0;
};

}  // namespace lsmdb::api
