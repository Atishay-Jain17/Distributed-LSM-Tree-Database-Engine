#pragma once
// MemTable contract used by the single-node API layer (Week 2).
//
// Atishay's SkipList MemTable should implement this interface (or be wrapped
// by a tiny adapter). Entries are (user key, sequence number, type, value).
// Newer sequence numbers shadow older ones for the same key. A delete is a
// TOMBSTONE entry, not a removal: later SSTable/compaction work needs it to
// hide older values stored on disk.
//
// Thread safety: Add() and Get() may be called concurrently from many threads.
#include <cstddef>
#include <cstdint>
#include <string>

namespace lsmdb::api {

using SequenceNumber = std::uint64_t;  // monotonically increasing; 0 = "nothing written yet"

enum class ValueType : std::uint8_t { kValue = 1, kTombstone = 2 };

enum class LookupState {
  kFound,     // newest visible entry is a value
  kDeleted,   // newest visible entry is a tombstone (hides any older value, incl. on disk)
  kNotFound,  // no entry for this key at or below read_seq (disk may still have it later)
};

struct LookupResult {
  LookupState state = LookupState::kNotFound;
  std::string value;               // only for kFound
  SequenceNumber sequence = 0;     // sequence of the entry that decided the result
};

class MemTable {
 public:
  virtual ~MemTable() = default;

  // Adds one entry. `value` is ignored for tombstones. Sequence numbers for the
  // same key must be strictly increasing (the API layer guarantees this).
  virtual void Add(SequenceNumber seq, ValueType type, const std::string& key,
                   const std::string& value) = 0;

  // Newest entry for `key` whose sequence <= read_seq (the MVCC snapshot rule).
  virtual LookupResult Get(const std::string& key, SequenceNumber read_seq) const = 0;

  // For the future flush threshold (Atishay, Week 2 Tue).
  virtual std::size_t ApproximateMemoryUsage() const = 0;
  virtual std::size_t EntryCount() const = 0;
};

}  // namespace lsmdb::api
