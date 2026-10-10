// Crash/restart tests for startup recovery. They use the stand-in WAL; when Atishay's WAL
// exists, add a second WalReader implementation here and run the same checks against it.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/recovery/recovery.h"
#include "lsmdb/recovery/reference_wal.h"

using namespace lsmdb;
using namespace lsmdb::api;
using namespace lsmdb::recovery;
namespace fs = std::filesystem;

namespace {

WalRecord Put(SequenceNumber s, const std::string& k, const std::string& v) {
  return {s, ValueType::kValue, k, v};
}
WalRecord Del(SequenceNumber s, const std::string& k) { return {s, ValueType::kTombstone, k, ""}; }

// What the database should contain, computed independently of the MemTable (plain std::map).
using Expected = std::map<std::string, std::string>;
void ApplyToExpected(Expected& e, const WalRecord& r) {
  if (r.type == ValueType::kValue) e[r.key] = r.value; else e.erase(r.key);
}

std::unique_ptr<MemTableBackend> NewBackend() {
  return std::make_unique<MemTableBackend>(std::make_shared<ReferenceMemTable>());
}

class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("lsmdb_recovery_" + std::to_string(::getpid()) + "_" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir_);
    wal_ = (dir_ / "wal.log").string();
  }
  void TearDown() override { std::error_code ec; fs::remove_all(dir_, ec); }

  // Writes records, returns the file size after each one.
  std::vector<std::uint64_t> WriteWal(const std::vector<WalRecord>& recs, const std::string& path) {
    ReferenceWalWriter w(path);
    std::vector<std::uint64_t> ends;
    for (const auto& r : recs) ends.push_back(w.Append(r));
    return ends;
  }
  static void Truncate(const std::string& path, std::uint64_t n) { fs::resize_file(path, n); }

  // Compares every key in `universe` against the expected state.
  static void ExpectState(MemTableBackend& db, const Expected& want, const std::vector<std::string>& universe) {
    for (const auto& k : universe) {
      auto got = db.Get(k);
      auto it = want.find(k);
      if (it == want.end()) EXPECT_FALSE(got.has_value()) << "key " << k << " should be absent";
      else { ASSERT_TRUE(got.has_value()) << "key " << k << " missing"; EXPECT_EQ(*got, it->second) << k; }
    }
  }

  fs::path dir_;
  std::string wal_;
};

}  // namespace

TEST_F(RecoveryTest, MissingOrEmptyWalRecoversToEmptyDatabase) {
  auto db = NewBackend();
  ReferenceWalReader missing((dir_ / "nope.log").string());
  auto st = Recover(missing, *db);
  EXPECT_EQ(st.records_applied, 0u);
  EXPECT_FALSE(st.dropped_torn_tail);
  EXPECT_EQ(db->LastSequence(), 0u);
}

TEST_F(RecoveryTest, ReplayRestoresPutsInOrder) {
  WriteWal({Put(1, "a", "1"), Put(2, "b", "2"), Put(3, "a", "3")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  auto st = Recover(r, *db);
  EXPECT_EQ(st.records_applied, 3u);
  EXPECT_EQ(st.last_sequence, 3u);
  EXPECT_FALSE(st.dropped_torn_tail);
  EXPECT_EQ(*db->Get("a"), "3");
  EXPECT_EQ(*db->Get("b"), "2");
}

TEST_F(RecoveryTest, NewWritesContinueFromRecoveredSequence) {
  WriteWal({Put(1, "a", "1"), Put(2, "a", "2")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  Recover(r, *db);
  db->Put("a", "3");
  EXPECT_EQ(db->LastSequence(), 3u);
  EXPECT_EQ(*db->Get("a"), "3");
}

TEST_F(RecoveryTest, VersionsAndDeletesSurviveAndSnapshotsStillWork) {
  WriteWal({Put(1, "k", "v1"), Put(2, "k", "v2"), Del(3, "k"), Put(4, "k", "v4"), Del(5, "gone")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  Recover(r, *db);
  EXPECT_EQ(db->GetAt("k", 1).value, "v1");
  EXPECT_EQ(db->GetAt("k", 2).value, "v2");
  EXPECT_EQ(db->GetAt("k", 3).state, LookupState::kDeleted);
  EXPECT_EQ(db->GetAt("k", 4).value, "v4");
  EXPECT_EQ(db->GetAt("gone", 5).state, LookupState::kDeleted);  // tombstone for a never-seen key kept
  EXPECT_EQ(*db->Get("k"), "v4");
  EXPECT_FALSE(db->Get("gone").has_value());
}

TEST_F(RecoveryTest, EmptyValuesAndBinaryDataSurvive) {
  const std::string key("a\0b", 3), val("\0\xff\0", 3);
  WriteWal({Put(1, key, val), Put(2, "empty", "")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  Recover(r, *db);
  EXPECT_EQ(*db->Get(key), val);
  ASSERT_TRUE(db->Get("empty").has_value());
  EXPECT_EQ(*db->Get("empty"), "");
}

TEST_F(RecoveryTest, RecordsAlreadyFlushedToSstablesAreSkipped) {
  WriteWal({Put(1, "a", "old"), Put(2, "b", "old"), Put(3, "a", "new")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  auto st = Recover(r, *db, /*flushed_up_to=*/2);
  EXPECT_EQ(st.records_skipped, 2u);
  EXPECT_EQ(st.records_applied, 1u);
  EXPECT_EQ(*db->Get("a"), "new");
  EXPECT_FALSE(db->Get("b").has_value());  // lives in an SSTable; the read path (Week 4) finds it there
  db->Put("c", "x");
  EXPECT_EQ(db->LastSequence(), 4u);
}

// Crash at EVERY possible byte position of the log: the recovered state must equal the state after
// the last fully written record, and the torn tail must be reported exactly when bytes were cut.
TEST_F(RecoveryTest, CrashAtEveryByteOffsetRecoversLastCompleteRecord) {
  std::vector<WalRecord> recs;
  std::vector<std::string> universe = {"a", "b", "c", "d"};
  SequenceNumber s = 1;
  for (int i = 0; i < 12; ++i) {
    const std::string k = universe[i % 4];
    recs.push_back(i % 5 == 4 ? Del(s++, k) : Put(s++, k, "value-" + std::to_string(i)));
  }
  auto ends = WriteWal(recs, wal_);
  const std::uint64_t total = ends.back();

  for (std::uint64_t cut = 0; cut <= total; ++cut) {
    const std::string path = (dir_ / "cut.log").string();
    fs::copy_file(wal_, path, fs::copy_options::overwrite_existing);
    Truncate(path, cut);

    Expected want;
    std::uint64_t complete = 0, boundary = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
      if (ends[i] <= cut) { ApplyToExpected(want, recs[i]); ++complete; boundary = ends[i]; }
    }
    auto db = NewBackend();
    ReferenceWalReader rd(path);
    RecoveryStats st;
    ASSERT_NO_THROW(st = Recover(rd, *db)) << "cut=" << cut;
    EXPECT_EQ(st.records_applied, complete) << "cut=" << cut;
    EXPECT_EQ(st.dropped_torn_tail, cut != boundary) << "cut=" << cut;
    EXPECT_EQ(st.valid_wal_bytes, boundary) << "cut=" << cut;
    ExpectState(*db, want, universe);
  }
}

TEST_F(RecoveryTest, TornTailIsDroppedThenAppendingAgainWorks) {
  auto ends = WriteWal({Put(1, "a", "1"), Put(2, "b", "2")}, wal_);
  Truncate(wal_, ends[1] - 3);  // crash while writing record 2
  auto db = NewBackend();
  {
    ReferenceWalReader r(wal_);
    auto st = Recover(r, *db);
    EXPECT_TRUE(st.dropped_torn_tail);
    EXPECT_EQ(st.valid_wal_bytes, ends[0]);
    Truncate(wal_, st.valid_wal_bytes);  // what the WAL writer must do before appending again
  }
  EXPECT_EQ(*db->Get("a"), "1");
  EXPECT_FALSE(db->Get("b").has_value());
  db->Put("b", "new");  // sequence 2 is reused: the torn record never counted
  {
    ReferenceWalWriter w(wal_, /*truncate=*/false);
    w.Append(Put(db->LastSequence(), "b", "new"));
  }
  auto db2 = NewBackend();
  ReferenceWalReader r2(wal_);
  auto st2 = Recover(r2, *db2);
  EXPECT_FALSE(st2.dropped_torn_tail);
  EXPECT_EQ(*db2->Get("b"), "new");
  EXPECT_EQ(*db2->Get("a"), "1");
}

TEST_F(RecoveryTest, DamagedLastRecordIsTreatedAsTornTail) {
  auto ends = WriteWal({Put(1, "a", "1"), Put(2, "b", "2")}, wal_);
  {  // flip a byte inside the last record's payload (file length unchanged)
    std::fstream f(wal_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(std::streamoff(ends[1] - 2)); char c = 0x5a; f.write(&c, 1);
  }
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  auto st = Recover(r, *db);
  EXPECT_TRUE(st.dropped_torn_tail);
  EXPECT_EQ(st.records_applied, 1u);
}

TEST_F(RecoveryTest, CorruptionBeforeTheEndRefusesToStart) {
  auto ends = WriteWal({Put(1, "a", "1"), Put(2, "b", "2"), Put(3, "c", "3")}, wal_);
  {  // damage record 1 while records 2 and 3 are intact
    std::fstream f(wal_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(std::streamoff(ends[0] - 2)); char c = 0x5a; f.write(&c, 1);
  }
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  EXPECT_THROW(Recover(r, *db), RecoveryError);
}

TEST_F(RecoveryTest, NonIncreasingSequenceNumbersAreRejected) {
  WriteWal({Put(1, "a", "1"), Put(1, "b", "2")}, wal_);
  auto db = NewBackend();
  ReferenceWalReader r(wal_);
  EXPECT_THROW(Recover(r, *db), RecoveryError);
}

TEST_F(RecoveryTest, RecoverRequiresAnEmptyBackend) {
  WriteWal({Put(1, "a", "1")}, wal_);
  auto db = NewBackend();
  db->Put("x", "y");
  ReferenceWalReader r(wal_);
  EXPECT_THROW(Recover(r, *db), RecoveryError);
}

// Random workload; the "database before the crash" is an independent std::map, and the recovered
// database must match it exactly, including after a restart-write-restart cycle.
TEST_F(RecoveryTest, RandomWorkloadMatchesIndependentModelAcrossTwoRestarts) {
  std::mt19937 rng(12345);
  std::vector<std::string> universe;
  for (int i = 0; i < 50; ++i) universe.push_back("key" + std::to_string(i));
  Expected want;
  SequenceNumber seq = 0;
  {
    ReferenceWalWriter w(wal_);
    for (int i = 0; i < 3000; ++i) {
      const auto& k = universe[rng() % universe.size()];
      WalRecord rec = (rng() % 4 == 0) ? Del(++seq, k) : Put(++seq, k, "v" + std::to_string(rng() % 1000));
      w.Append(rec);
      ApplyToExpected(want, rec);
    }
  }
  {  // restart 1
    auto db = NewBackend();
    ReferenceWalReader r(wal_);
    auto st = Recover(r, *db);
    EXPECT_EQ(st.records_applied, 3000u);
    ExpectState(*db, want, universe);
    // keep working, logging each write like the real write path will
    ReferenceWalWriter w(wal_, false);
    for (int i = 0; i < 500; ++i) {
      const auto& k = universe[rng() % universe.size()];
      if (rng() % 3 == 0) { db->Delete(k); w.Append(Del(db->LastSequence(), k)); want.erase(k); }
      else { std::string v = "w" + std::to_string(i); db->Put(k, v); w.Append(Put(db->LastSequence(), k, v)); want[k] = v; }
    }
  }
  {  // restart 2
    auto db = NewBackend();
    ReferenceWalReader r(wal_);
    auto st = Recover(r, *db);
    EXPECT_EQ(st.records_applied, 3500u);
    EXPECT_EQ(st.last_sequence, 3500u);
    ExpectState(*db, want, universe);
  }
}
