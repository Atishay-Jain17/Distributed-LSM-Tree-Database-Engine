#pragma once
// What startup recovery needs from the thing it rebuilds (Week 3/4). Implemented by
// MemTableBackend (Week 2) and LsmStore (Week 4), so Recover() works with either.
#include <string>

#include "lsmdb/api/memtable.h"

namespace lsmdb::api {

class RestoreTarget {
 public:
  virtual ~RestoreTarget() = default;
  // Re-apply one WAL record with its ORIGINAL sequence number. Single thread, before serving.
  // Sequence numbers must increase.
  virtual void Restore(SequenceNumber seq, ValueType type, const std::string& key,
                       const std::string& value) = 0;
  // Highest sequence visible so far (0 = empty).
  virtual SequenceNumber LastSequence() const = 0;
};

}  // namespace lsmdb::api
