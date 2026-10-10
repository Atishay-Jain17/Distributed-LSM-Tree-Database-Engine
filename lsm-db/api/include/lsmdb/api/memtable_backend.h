#pragma once
// Single-node storage API: PUT / GET / DELETE on top of a MemTable.
//
// Implements rpc::StorageBackend, so Atishay's integration is one line in
// node_main.cpp:
//     std::make_shared<lsmdb::api::MemTableBackend>(std::make_shared<SkipListMemTable>())
//
// Behaviour (see docs/single-node-api.md):
//  * Put      : always succeeds; overwrites by writing a newer version.
//  * Get      : value, or std::nullopt if missing OR deleted.
//  * Delete   : idempotent; ALWAYS writes a tombstone, even for a missing key,
//               because the key may exist in an older SSTable later.
//  * Every write gets the next sequence number; a write becomes visible to reads
//    only after it is fully inserted (readers never see a half-applied write).
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "lsmdb/api/memtable.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::api {

class MemTableBackend : public rpc::StorageBackend {
 public:
  explicit MemTableBackend(std::shared_ptr<MemTable> memtable);

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;

  // MVCC helpers (Sarthak's versioning work, Weeks 2-4 read paths).
  SequenceNumber LastSequence() const { return last_visible_.load(std::memory_order_acquire); }
  // Read as of `read_seq`; use LastSequence() captured earlier for a stable snapshot.
  LookupResult GetAt(const std::string& key, SequenceNumber read_seq) const;

  const MemTable& memtable() const { return *memtable_; }

 private:
  void Write(ValueType type, const std::string& key, const std::string& value);

  std::shared_ptr<MemTable> memtable_;
  std::mutex write_mu_;                       // serialises sequence assignment + insert
  SequenceNumber next_seq_ = 1;               // guarded by write_mu_
  std::atomic<SequenceNumber> last_visible_{0};
};

}  // namespace lsmdb::api
