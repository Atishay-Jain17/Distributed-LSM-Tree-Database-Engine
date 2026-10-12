#pragma once
// Startup recovery: rebuild the MemTable from the WAL (see docs/recovery-flow.md).
#include <cstdint>
#include <stdexcept>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/restore_target.h"
#include "lsmdb/recovery/wal_reader.h"

namespace lsmdb::recovery {

struct RecoveryError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct RecoveryStats {
  std::uint64_t records_read = 0;
  std::uint64_t records_applied = 0;
  std::uint64_t records_skipped = 0;        // sequence <= flushed_up_to (already in SSTables)
  api::SequenceNumber last_sequence = 0;    // highest sequence now visible
  bool dropped_torn_tail = false;           // last record was incomplete and was ignored
  std::uint64_t valid_wal_bytes = 0;        // truncate the WAL to this before appending
};

// Replays `wal` into `backend` (which must not have data newer than `flushed_up_to`, and must not be
// serving yet).
//  * records with sequence <= flushed_up_to are skipped: SSTables already hold them
//  * a torn final record is dropped (recorded in stats), the rest is applied
//  * corruption before the end of the log, or non-increasing sequence numbers, throw RecoveryError
RecoveryStats Recover(WalReader& wal, api::RestoreTarget& backend,
                      api::SequenceNumber flushed_up_to = 0);

}  // namespace lsmdb::recovery
