#pragma once
// Core record type shared by MemTable, SSTable and compaction.
#include <cstdint>
#include <limits>
#include <string>

namespace lsmdb::storage {

enum class EntryType : std::uint8_t { kPut = 0, kDelete = 1 };

// One versioned record. `seq` is the version number (the Raft log index in the
// distributed system). A tombstone has type kDelete and an empty value.
struct Entry {
  std::string key;
  std::uint64_t seq = 0;
  EntryType type = EntryType::kPut;
  std::string value;
};

inline constexpr std::uint64_t kMaxSequence = std::numeric_limits<std::uint64_t>::max();

// The ordering used EVERYWHERE (MemTable, SSTable files, compaction merge):
// key ascending (bytewise), then seq DESCENDING so the newest version comes first.
inline bool EntryLess(const Entry& a, const Entry& b) {
  if (int c = a.key.compare(b.key); c != 0) return c < 0;
  return a.seq > b.seq;
}

}  // namespace lsmdb::storage
