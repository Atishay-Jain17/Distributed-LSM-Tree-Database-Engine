#pragma once
// What startup recovery needs from the WAL (Atishay's WAL, Week 3). The real WAL reader
// should implement WalReader; recovery code does not care how records are stored.
#include <cstdint>
#include <string>

#include "lsmdb/api/memtable.h"

namespace lsmdb::recovery {

struct WalRecord {
  api::SequenceNumber sequence = 0;
  api::ValueType type = api::ValueType::kValue;
  std::string key;
  std::string value;  // empty for tombstones
};

enum class WalReadResult {
  kRecord,         // `out` holds the next valid record
  kEndOfLog,       // clean end: every byte was a valid record
  kTruncatedTail,  // the LAST record is incomplete/damaged (crash during append). Safe to drop.
  kCorrupt,        // damage with valid data after it. NOT safe: refuse to start.
};

class WalReader {
 public:
  virtual ~WalReader() = default;
  // Returns records in log order. After kEndOfLog / kTruncatedTail / kCorrupt it must keep
  // returning that same result.
  virtual WalReadResult Next(WalRecord& out) = 0;
  // Number of bytes at the start of the log that hold complete, valid records. The writer
  // must truncate the file to this length before appending again after a torn tail.
  virtual std::uint64_t ValidBytes() const = 0;
};

}  // namespace lsmdb::recovery
