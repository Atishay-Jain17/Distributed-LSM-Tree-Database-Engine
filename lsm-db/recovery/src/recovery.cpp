#include "lsmdb/recovery/recovery.h"

#include <string>

namespace lsmdb::recovery {

RecoveryStats Recover(WalReader& wal, api::RestoreTarget& backend,
                      api::SequenceNumber flushed_up_to) {
  RecoveryStats stats;
  // A store opened after SSTable recovery already reports the flushed sequence; anything
  // beyond that means it has been written to and must not be recovered into.
  if (backend.LastSequence() > flushed_up_to) throw RecoveryError("Recover: backend is not empty");
  stats.last_sequence = flushed_up_to;  // SSTables already cover this much

  api::SequenceNumber prev = 0;
  WalRecord rec;
  for (;;) {
    WalReadResult r = wal.Next(rec);
    if (r == WalReadResult::kRecord) {
      ++stats.records_read;
      if (rec.sequence == 0 || rec.sequence <= prev) {
        throw RecoveryError("WAL sequence numbers must strictly increase (got " +
                            std::to_string(rec.sequence) + " after " + std::to_string(prev) + ")");
      }
      prev = rec.sequence;
      if (rec.sequence <= flushed_up_to) {
        ++stats.records_skipped;
        continue;
      }
      backend.Restore(rec.sequence, rec.type, rec.key, rec.value);
      ++stats.records_applied;
      stats.last_sequence = rec.sequence;
    } else if (r == WalReadResult::kEndOfLog) {
      break;
    } else if (r == WalReadResult::kTruncatedTail) {
      stats.dropped_torn_tail = true;
      break;
    } else {
      throw RecoveryError("WAL is corrupt before its end; refusing to start");
    }
  }
  stats.valid_wal_bytes = wal.ValidBytes();
  return stats;
}

}  // namespace lsmdb::recovery
