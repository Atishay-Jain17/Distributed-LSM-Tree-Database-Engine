#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lsmdb/node/persistent_backend.h"
#include "lsmdb/consensus/reference_consensus.h"
#include "lsmdb/consensus/replicated_backend.h"
#include "lsmdb/consensus/storage_state_machine.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/lifecycle/reference_table_store.h"
#ifdef LSMDB_HAS_ATISHAY_SKIPLIST
#include "lsmdb/api/atishay_skiplist_memtable.h"
#endif

namespace {
namespace fs = std::filesystem;

fs::path DetermineSmokeDir() {
  const char* env_custom = std::getenv("LSMDB_TEST_TEMP");
  if (env_custom && *env_custom) return fs::path(env_custom) / "lsmdb-node-storage-smoke";
  const char* temp_env = std::getenv("TEMP");
  if (temp_env && *temp_env) return fs::path(temp_env) / "lsmdb-node-storage-smoke";
  const char* tmp_env = std::getenv("TMP");
  if (tmp_env && *tmp_env) return fs::path(tmp_env) / "lsmdb-node-storage-smoke";
  return fs::current_path() / "build" / "test-data" / "lsmdb-node-storage-smoke";
}
}  // namespace

int main() {
  const fs::path dir = DetermineSmokeDir();
  try {
    fs::remove_all(dir);
    {
      auto api = std::make_shared<lsmdb::api::MemTableBackend>(
#ifdef LSMDB_HAS_ATISHAY_SKIPLIST
          std::make_shared<lsmdb::api::AtishaySkipListMemTable>());
#else
          std::make_shared<lsmdb::api::ReferenceMemTable>());
#endif
      std::atomic<bool> correct{true};
      std::vector<std::thread> workers;
      for (int t = 0; t < 6; ++t) {
        workers.emplace_back([&, t] {
          for (int i = 0; i < 100; ++i) {
            const std::string key = "thread-" + std::to_string(t) + "-" + std::to_string(i);
            const std::string value = "value-" + std::to_string(i);
            api->Put(key, value);
            if (api->Get(key) != std::optional<std::string>(value)) correct.store(false);
            api->Delete(key);
            if (api->Get(key)) correct.store(false);
          }
        });
      }
      for (auto& worker : workers) worker.join();
      if (!correct.load() || api->LastSequence() != 1200)
        throw std::runtime_error("concurrent single-node API operations failed");
    }
#ifdef LSMDB_HAS_ATISHAY_SKIPLIST
    {
      lsmdb::api::AtishaySkipListMemTable memtable;
      const std::string binary_key("\0k", 2);
      const std::string binary_value("v\0x", 3);
      memtable.Add(1, lsmdb::api::ValueType::kValue, "a", "old");
      memtable.Add(2, lsmdb::api::ValueType::kValue, binary_key, binary_value);
      memtable.Add(3, lsmdb::api::ValueType::kTombstone, "a", "");
      memtable.Add(4, lsmdb::api::ValueType::kValue, "a", "new");
      memtable.Add(5, lsmdb::api::ValueType::kValue, std::string("\0", 1), "zero");
      if (memtable.Get("a", 2).value != "old" ||
          memtable.Get("a", 3).state != lsmdb::api::LookupState::kDeleted ||
          memtable.Get("a", 4).value != "new" ||
          memtable.Get(binary_key, 5).value != binary_value)
        throw std::runtime_error("Atishay SkipList adapter failed MVCC/tombstone/binary lookup");
      std::vector<std::pair<std::string, lsmdb::api::SequenceNumber>> ordered;
      memtable.ForEach([&](auto seq, auto, const auto& key, const auto&) { ordered.emplace_back(key, seq); });
      const std::vector<std::pair<std::string, lsmdb::api::SequenceNumber>> expected = {
          {std::string("\0", 1), 5}, {binary_key, 2}, {"a", 4}, {"a", 3}, {"a", 1}};
      if (ordered != expected) throw std::runtime_error("Atishay SkipList adapter traversal order is wrong");
      bool rejected_out_of_order = false;
      try { memtable.Add(2, lsmdb::api::ValueType::kValue, "a", "stale"); }
      catch (const std::logic_error&) { rejected_out_of_order = true; }
      if (!rejected_out_of_order) throw std::runtime_error("Atishay SkipList adapter accepted an old version");
    }
#endif
    fs::remove_all(dir);
    {
      auto db = lsmdb::node::PersistentBackend::Open(dir);
      db->Put("durable", "before-restart");
      db->Put("gone", "old");
      db->Delete("gone");
      db->Flush();
      if (fs::file_size(dir / "wal.log") != 0)
        throw std::runtime_error("WAL checkpoint retained records covered by a durable SSTable");
    }
    {
      auto db = lsmdb::node::PersistentBackend::Open(dir);
      if (db->Get("durable") != std::optional<std::string>("before-restart") || db->Get("gone"))
        throw std::runtime_error("flush/reopen did not preserve values and tombstones");
      db->Put("wal-only", "replayed");
    }
    {
      auto db = lsmdb::node::PersistentBackend::Open(dir);
      if (db->Get("wal-only") != std::optional<std::string>("replayed"))
        throw std::runtime_error("WAL replay did not restore the most recent write");
      auto raft = std::make_shared<lsmdb::consensus::ReferenceConsensus>(
          std::make_shared<lsmdb::consensus::StorageStateMachine>(db));
      lsmdb::consensus::ReplicatedBackend reads(raft, db);
      if (reads.Get("wal-only") != std::optional<std::string>("replayed"))
        throw std::runtime_error("leader read barrier failed to return the committed value");
      raft->SetLeader(false, 2);
      bool rejected = false;
      try { (void)reads.Get("wal-only"); }
      catch (const lsmdb::consensus::NotLeaderError&) { rejected = true; }
      if (!rejected) throw std::runtime_error("follower served a read without a read barrier");
    }
    {
      const fs::path tail_dir = dir / "torn-tail-check";
      {
        auto db = lsmdb::node::PersistentBackend::Open(tail_dir);
        db->Put("before-tail", "kept");
      }
      const auto complete_size = fs::file_size(tail_dir / "wal.log");
      {
        std::ofstream out(tail_dir / "wal.log", std::ios::binary | std::ios::app);
        out.write("\x01\x02\x03", 3);
      }
      {
        auto db = lsmdb::node::PersistentBackend::Open(tail_dir);
        if (!db->recovery_stats().dropped_torn_tail || fs::file_size(tail_dir / "wal.log") != complete_size)
          throw std::runtime_error("torn final WAL record was not truncated before reopening for append");
        db->Put("after-tail", "also-kept");
      }
      auto db = lsmdb::node::PersistentBackend::Open(tail_dir);
      if (db->Get("before-tail") != std::optional<std::string>("kept") ||
          db->Get("after-tail") != std::optional<std::string>("also-kept"))
        throw std::runtime_error("writes around a torn WAL tail did not survive restart");
    }
    {
      const fs::path corrupt_dir = dir / "interior-corruption-check";
      {
        auto db = lsmdb::node::PersistentBackend::Open(corrupt_dir);
        db->Put("first", "must-not-be-silently-dropped");
        db->Put("second", "valid-record-after-corruption");
      }
      const fs::path wal_path = corrupt_dir / "wal.log";
      std::fstream wal(wal_path, std::ios::binary | std::ios::in | std::ios::out);
#ifdef LSMDB_HAS_ATISHAY_WAL
      wal.seekg(29);  // Atishay format's first key byte; a second complete record follows
#else
      wal.seekg(25);  // reference format's first key byte; a second complete record follows
#endif
      char damaged = 0;
      wal.read(&damaged, 1);
      damaged ^= 0x40;
#ifdef LSMDB_HAS_ATISHAY_WAL
      wal.seekp(29);
#else
      wal.seekp(25);
#endif
      wal.write(&damaged, 1);
      wal.close();
      bool refused = false;
      try { (void)lsmdb::node::PersistentBackend::Open(corrupt_dir); }
      catch (const lsmdb::recovery::RecoveryError&) { refused = true; }
      if (!refused) throw std::runtime_error("startup accepted corruption before a later WAL record");
    }
    {
      const fs::path lifecycle_dir = dir / "lifecycle-stress";
      lsmdb::lifecycle::LsmOptions options;
      options.memtable_bytes = 512;
      {
        auto db = lsmdb::node::PersistentBackend::Open(lifecycle_dir, options);
        for (int i = 0; i < 100; ++i) db->Put("key-" + std::to_string(i), "value-" + std::to_string(i));
        for (int i = 0; i < 100; i += 2) db->Delete("key-" + std::to_string(i));
        db->Flush();
        db->store().WaitForIdle();
        if (db->store().stats().flushes < 2 || db->store().stats().compactions == 0)
          throw std::runtime_error("small-threshold workload did not exercise flush and compaction");
      }
      auto db = lsmdb::node::PersistentBackend::Open(lifecycle_dir);
      for (int i = 0; i < 100; ++i) {
        const auto value = db->Get("key-" + std::to_string(i));
        if (i % 2 == 0 && value) throw std::runtime_error("delete was lost across flush/compaction/restart");
        if (i % 2 != 0 && value != std::optional<std::string>("value-" + std::to_string(i)))
          throw std::runtime_error("value was lost across flush/compaction/restart");
      }
    }
    {
      const fs::path table_dir = dir / "compaction-check";
      const fs::path stale_copy = dir / "stale-copy.sst";
      {
        auto tables = std::make_shared<lsmdb::lifecycle::ReferenceTableStore>(table_dir);
        lsmdb::api::ReferenceMemTable value;
        value.Add(1, lsmdb::api::ValueType::kValue, "key", "old");
        tables->AddTable(value);
        lsmdb::api::ReferenceMemTable tombstone;
        tombstone.Add(2, lsmdb::api::ValueType::kTombstone, "key", "");
        tables->AddTable(tombstone);
        fs::copy_file(tables->LivePaths().back(), stale_copy);
        tables->Compact(2);
        if (tables->LastSequence() != 2) throw std::runtime_error("compaction lost sequence high-water mark");
      }
      // Simulate a crash leaving an old input file behind after the live manifest was published.
      fs::copy_file(stale_copy, table_dir / "000001.sst");
      auto tables = std::make_shared<lsmdb::lifecycle::ReferenceTableStore>(table_dir);
      if (tables->LastSequence() != 2) throw std::runtime_error("restart lost compacted sequence high-water mark");
      if (tables->Get("key", 2).state != lsmdb::api::LookupState::kNotFound)
        throw std::runtime_error("restart resurrected a stale table omitted by the live manifest");
      lsmdb::lifecycle::LsmOptions options;
      options.start_sequence = tables->LastSequence();
      lsmdb::lifecycle::LsmStore after_compaction(tables, options);
      after_compaction.Put("next", "value");
      if (after_compaction.LastSequence() != 3) throw std::runtime_error("sequence was reused after compaction");
    }
    {
      const fs::path table_dir = dir / "missing-sequence-check";
      {
        auto tables = std::make_shared<lsmdb::lifecycle::ReferenceTableStore>(table_dir);
        lsmdb::api::ReferenceMemTable value;
        value.Add(7, lsmdb::api::ValueType::kValue, "key", "old");
        tables->AddTable(value);
        lsmdb::api::ReferenceMemTable tombstone;
        tombstone.Add(9, lsmdb::api::ValueType::kTombstone, "key", "");
        tables->AddTable(tombstone);
        fs::remove(table_dir / "sequence.manifest");
      }
      {
        auto tables = std::make_shared<lsmdb::lifecycle::ReferenceTableStore>(table_dir);
        if (tables->LastSequence() != 9)
          throw std::runtime_error("startup did not recover the sequence high-water from tables");
        tables->Compact(9);  // drops the newest floor tombstone and the value it hides
      }
      auto tables = std::make_shared<lsmdb::lifecycle::ReferenceTableStore>(table_dir);
      if (tables->LastSequence() != 9 ||
          tables->Get("key", 9).state != lsmdb::api::LookupState::kNotFound)
        throw std::runtime_error("compaction lost the sequence high-water or resurrected a tombstoned value");
      lsmdb::lifecycle::LsmOptions options;
      options.start_sequence = tables->LastSequence();
      lsmdb::lifecycle::LsmStore after_compaction(tables, options);
      after_compaction.Put("next", "value");
      if (after_compaction.LastSequence() != 10)
        throw std::runtime_error("sequence was reused after rebuilding a missing checkpoint");
    }
    fs::remove_all(dir);
    std::cout << "PASS: WAL recovery, table discovery, tombstone, sequence and read-barrier path\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 1;
  }
}
